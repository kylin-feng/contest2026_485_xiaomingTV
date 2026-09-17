#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
selftest_e2e.py — 语音链路端到端自检（不需要板子、不需要 API key、不需要网络）

为什么要有这个：语音链路上任何一环错都会表现成同一个症状——"没反应"。
串口帧错了、Opus 编解码错了、VAD 阈值错了、协议字段错了、采样率错了，
在耳朵里听起来都是"它没说话"。所以必须先有一层能**定性定位**的自检：

    假麦克风(wav) → 网关 VAD → Opus 16k → [ws] → 假服务器
                                     ↓ 解码 16k 判静音
    假输出(null) ← 16k PCM ← Opus 24k ← [ws] ← 假服务器合成旋律

跑通它，就证明**除了"板子硬件"以外的所有环节**都是对的，剩下的问题一定在板子。
之后换成真服务器（去掉 --server 参数指向官方地址），行为完全一致。

用法：
    python selftest_e2e.py              # 生成测试音频并跑完整自检
    python selftest_e2e.py --keep       # 保留生成的 wav 便于自己听
"""
import argparse
import asyncio
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import voice_codec as vc
import gateway as gw_mod
import mock_xiaozhi_server as mock
from xiaozhi_proto import UP_SAMPLE_RATE


# ---------------------------------------------------------------------------
# 造一段"像人说话"的测试音频
# ---------------------------------------------------------------------------

def synth_speech(rate=16000, n_syllable=6, syllable_ms=150, f0=130.0,
                 amp=0.30, seed=7):
    """合成一段有音节起伏的浊音。

    不是要"像真人"，而是要满足 VAD 的三个机械条件：
      1) 整体能量明显高于静音（否则一帧都不算语音）
      2) 逐帧能量有起伏（音节之间掉下去再起来，模拟辅音/停顿）
      3) 谐波结构接近人声（Opus 用 VOIP 模式，纯白噪声会被编码器当背景噪声压掉）
    所以用"基频 + 谐波 + 慢包络"，而不是白噪声。
    """
    rng = np.random.default_rng(seed)
    n = int(rate * syllable_ms / 1000)
    out = []
    for i in range(n_syllable):
        t = np.arange(n, dtype=np.float64) / rate
        pitch = f0 * (1.0 + 0.12 * np.sin(2 * np.pi * i / 5.0))   # 语调起伏
        w = np.zeros(n)
        for h, a in ((1, 1.0), (2, 0.55), (3, 0.32), (4, 0.18), (5, 0.10)):
            w += a * np.sin(2 * np.pi * pitch * h * t + rng.uniform(0, 6.28))
        w /= 2.15
        env = np.hanning(n) ** 0.45          # 音节内包络，别让帧边界是硬切
        out.append(w * env)
        out.append(np.zeros(int(rate * 0.03), dtype=np.float64))   # 音节间 30ms
    pcm = np.concatenate(out)
    pcm *= amp * 32767.0 / (np.max(np.abs(pcm)) + 1e-9)
    return pcm.astype(np.int16)


def make_test_wav(path, rate=16000, gap_s=3.5):
    """两句话 + 中间停顿 + 句尾 1.5s 静音。

    gap_s 控制停顿长度，决定测的是哪件事：
      · gap_s > TTS 时长(约 2.8s)：两轮各自独立、可复现 —— 默认测法
      · gap_s < TTS 时长(如 1.3s)：第二段语音落在朗读中途，会触发**打断**
        —— 这同样是必须成立的行为（用户说"别说了"要能打断），单独用一个
        用例来测，而不是让它污染主流程
    """
    gap = np.zeros(int(rate * gap_s), dtype=np.int16)
    tail = np.zeros(int(rate * 1.5), dtype=np.int16)
    s1 = synth_speech(rate=rate, n_syllable=6, seed=7)
    s2 = synth_speech(rate=rate, n_syllable=9, f0=155.0, seed=19)
    pcm = np.concatenate([s1, gap, s2, tail])

    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())

    def db(x):
        r = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)) + 1e-9)
        return 20 * np.log10(r / 32768.0)

    print("测试音频: %s" % path)
    print("  总长 %.2fs / 语音段1 %.2fs(%.1f dB) / 语音段2 %.2fs(%.1f dB) / 静音 %.1f dB"
          % (pcm.size / float(rate), s1.size / float(rate), db(s1),
             s2.size / float(rate), db(s2), db(gap)))
    return pcm


# ---------------------------------------------------------------------------
# 自检主体
# ---------------------------------------------------------------------------

async def run_test(wav, verbose=False, timeout=45.0, expect=None):
    import websockets

    # 找一个空闲端口，避免和用户已经跑着的服务撞车
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()

    srv = mock.MockServer(verbose=verbose)
    seen = {"hello": 0, "stt": [], "tts": [], "llm": 0, "listen": [],
            "down_samples": 0, "down_calls": 0, "up_frames": 0, "aborts": 0,
            "up_opus": 0, "log": []}

    async with websockets.serve(srv.handler, "127.0.0.1", port,
                                max_size=None, ping_interval=None):
        cfg = {
            "server": "ws://127.0.0.1:%d/xiaozhi/v1/" % port,
            "token": "", "device_id": "selftest-01",
            "port": None, "baud": 1500000,
            "source": "wav", "sink": "null", "wake": "vad",
            "wav": wav, "wav_loop": False,
        }
        gw = gw_mod.Gateway(cfg)
        gw.start_audio()

        # 挂观测点：不改逻辑，只在网关的输入/输出口子上记一笔
        _on_text = gw._on_text
        def on_text(raw):
            ev = gw.proto.parse(raw)
            t = ev["type"]
            seen["log"].append((time.time(), t, ev))
            if t == "hello":
                seen["hello"] += 1
            elif t == "stt":
                seen["stt"].append(ev["text"])
            elif t == "tts":
                seen["tts"].append(ev["state"])
            elif t == "llm":
                seen["llm"] += 1
            elif t == "listen":
                seen["listen"].append(ev.get("state"))
            return _on_text(raw)
        gw._on_text = on_text

        _write = gw.audio.write_pcm
        def write_pcm(pcm):
            seen["down_calls"] += 1
            seen["down_samples"] += int(pcm.size)
            return _write(pcm)
        gw.audio.write_pcm = write_pcm

        task = asyncio.create_task(gw.run())
        t0 = time.time()
        satisfied_at = None
        reason = "超时"
        while time.time() - t0 < timeout:
            await asyncio.sleep(0.2)
            ok_now = (len(seen["stt"]) >= 2
                      and seen["tts"].count("stop") >= 2
                      and seen["down_samples"] > 0)
            if ok_now:
                # 满足后还要再多看 1 秒，确认状态已经稳下来而不是中间态
                if satisfied_at is None:
                    satisfied_at = time.time()
                elif time.time() - satisfied_at > 1.0:
                    reason = "两轮对话均已完成"
                    break
            else:
                satisfied_at = None
            # 音频放完了却一句都没识别出来 —— 早退，不用干等满超时
            if gw.audio.done and len(seen["stt"]) == 0 and time.time() - t0 > 15:
                reason = "音频已放完但未识别出任何语音"
                break
        seen["elapsed"] = time.time() - t0
        seen["reason"] = reason
        seen["aborts"] = gw.stats["aborts"]
        seen["up_opus"] = gw.stats["up_opus"]
        print("[selftest] 结束原因: %s（历时 %.1fs）" % (reason, seen["elapsed"]))

        gw.quit = True
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        except Exception:
            pass
        gw.shutdown()

    return seen, port


def report(seen, expect=None):
    print("\n" + "=" * 66)
    print("端到端自检结果" + ("（打断用例）" if expect == "barge_in" else ""))
    print("=" * 66)

    checks = []
    def chk(name, ok, detail=""):
        checks.append((name, ok, detail))
        print("  [%s] %-34s %s" % ("OK" if ok else "!!", name, detail))

    chk("WebSocket 握手 (hello 往返)", seen["hello"] >= 1,
        "收到 %d 次服务器 hello" % seen["hello"])
    chk("音频上行 (Opus 编码已发出)", seen["up_opus"] > 0,
        "已上传 %d 帧 Opus（16k/60ms）" % seen["up_opus"])
    chk("VAD 断句 (识别到 2 段语音)", len(seen["stt"]) >= 2,
        "STT 结果 %d 条" % len(seen["stt"]))
    chk("TTS 流程 (start/sentence/stop)", seen["tts"].count("stop") >= 1,
        "tts 事件 %s" % seen["tts"])
    chk("音频下行 (Opus 解码+PCM 落库)", seen["down_samples"] > 0,
        "%d 帧 / %d 样本 (%.2fs @16k)"
        % (seen["down_calls"], seen["down_samples"], seen["down_samples"] / 16000.0))
    chk("LLM 情感字段解析", seen["llm"] >= 1, "emotion 事件 %d 次" % seen["llm"])
    if expect == "barge_in":
        chk("打断 (朗读中说话能掐断)", seen["aborts"] >= 1,
            "abort 已发出 %d 次" % seen["aborts"])

    print("-" * 66)
    for i, t in enumerate(seen["stt"], 1):
        print("  识别#%d: %s" % (i, t))
    print("-" * 66)
    bad = [c for c in checks if not c[1]]
    if bad:
        print("失败 %d/%d 项" % (len(bad), len(checks)))
    else:
        print("全部通过 —— 除板子硬件外，语音链路各环节均已验证")
    print("=" * 66)
    return len(bad) == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", help="用自己的 wav（16k/16bit/mono）代替合成音频")
    ap.add_argument("--keep", action="store_true", help="保留合成音频")
    ap.add_argument("--verbose", action="store_true", help="打印所有协议报文")
    ap.add_argument("--timeout", type=float, default=45.0)
    ap.add_argument("--barge-in", dest="barge_in", action="store_true",
                    help="改测打断：第二句话落在朗读中途，断言能掐断")
    a = ap.parse_args()

    if not vc.have_opus():
        print("没有 libopus，请先 pip install pyogg")
        return 2

    wav = a.wav
    gen = os.path.join(HERE, "_selftest_speech.wav")
    if not wav:
        make_test_wav(gen, UP_SAMPLE_RATE, gap_s=1.3 if a.barge_in else 3.5)
        wav = gen
    else:
        print("使用自带 wav: %s" % wav)

    seen, port = asyncio.run(run_test(wav, a.verbose, a.timeout,
                                      "barge_in" if a.barge_in else None))
    ok = report(seen, "barge_in" if a.barge_in else None)

    if not a.keep and not a.wav and os.path.exists(gen):
        try:
            os.remove(gen)
        except Exception:
            pass
    elif a.keep:
        print("合成音频已保留: %s（可直接播放来听一遍）" % gen)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
