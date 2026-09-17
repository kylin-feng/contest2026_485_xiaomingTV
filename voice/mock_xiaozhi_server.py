#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mock_xiaozhi_server.py — 最小可用的小智协议服务器（离线自测用，不需要任何 API key）

作用：把"网关 ↔ 服务器"这一段先单独验证通。没有它的话，第一次联调就得同时
面对「板子串口 + 网关 + Opus + 小智协议 + 服务器部署 + API key」六个未知数，
出问题无从下手。有了它，可以先证明前四个都是对的，再上真服务器。

它做的事：
  · 校验 hello / 头字段，回 hello（下行 24000Hz —— 和官方文档示例一致）
  · 收 Opus 上行 → 解码 → 能量 VAD
  · VAD 判定"说完了" → 回一条 stt 文本 + tts start/sentence_start
  · 生成一段**可辨识的旋律**当 TTS 音频（琶音 + 收尾音），Opus 编码后下发
  · tts stop，然后自动重新 listen
这样链路上每一环都有可观测的输出：串口帧数、Opus 字节数、播放出的旋律、屏幕上的文本。

用法：
    python mock_xiaozhi_server.py [--port 8989] [--verbose]
"""
import argparse
import asyncio
import ctypes
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import voice_codec as vc
from xiaozhi_proto import PROTOCOL_VERSION

PATH = "/xiaozhi/v1/"

# 假服务器的"LLM"：按顺序回这些哄睡话术，轮流使用
REPLIES = [
    "嗯，我在呢。今天辛苦了，把肩膀放松下来。",
    "没事的，睡不着就不睡，我陪你说说话。",
    "跟着我，慢慢吸气……再慢慢吐出来。",
    "已经很晚了，眼睛闭上也没关系，我在这里。",
]


def melody(text_len: int, rate: int = 24000) -> np.ndarray:
    """生成一段"像在说话"的可辨识音频。

    不做真 TTS（那要模型），而是按文本长度合成一串音符：每个字一个音，
    音高在一组五声音阶里走。目的是**能听见、能数出来、能确认播放链路通**，
    而不是好听。
    """
    scale = [523, 587, 659, 784, 880]          # C D E G A
    n_notes = max(3, min(len(text_len), 24))
    seg_ms = 130
    out = []
    for i in range(n_notes):
        f = scale[(i * 2) % len(scale)]
        n = int(rate * seg_ms / 1000)
        t = np.arange(n, dtype=np.float64) / rate
        w = np.sin(2 * np.pi * f * t) * 0.22
        # 每个音符加一点包络，听起来像音节而不是长鸣
        k = max(1, int(rate * 0.012))
        env = np.ones(n)
        env[:k] = np.linspace(0, 1, k)
        env[-k:] = np.linspace(1, 0, k)
        out.append((w * env * 32767).astype(np.int16))
    out.append(np.zeros(int(rate * 0.12), dtype=np.int16))
    return np.concatenate(out)


class MockServer:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.n = 0
        self.codec = vc.OpusCodec(16000, 24000, 60)
        self.reply_i = 0
        self.busy = False
        self.abort_flag = False

    def v(self, *a):
        if self.verbose:
            print(*a, flush=True)

    async def handler(self, ws):
        self.n += 1
        cid = self.n
        path = getattr(getattr(ws, "request", None), "path", PATH)
        hdr = {}
        try:
            hdr = dict(ws.request.headers)
        except Exception:
            pass
        dev = hdr.get("Device-Id") or hdr.get("device-id") or "?"
        print("[mock] 连接#%d path=%s Device-Id=%s Protocol-Version=%s"
              % (cid, path, dev, hdr.get("Protocol-Version")), flush=True)

        audio_buf = []
        speech_frames = 0
        silence_frames = 0
        speaking = False
        IN_SPEECH_DB, OUT_SPEECH_DB = -40.0, -48.0

        async def send_json(d):
            await ws.send(json.dumps(d, ensure_ascii=False))

        async def maybe_respond(why):
            """攒够语音帧就回一轮。两条触发路径都会走到这，所以必须防重入。

            · 服务器自己数到 720ms 静音（真实小智服务器就是这么做的）
            · 或者设备发了 listen stop（我们的网关是设备侧 VAD 判定结束，
              此时尾部静音可能不足 720ms 就被掐断了）
            这两条是"或"的关系，谁先满足谁触发，另一条直接作废。
            """
            nonlocal speech_frames, silence_frames, audio_buf
            if self.busy or speech_frames < 4:
                return
            self.busy = True
            total = sum(p.size for p in audio_buf)
            try:
                print("[mock] 触发回复（%s）：%d 帧 / %.2fs"
                      % (why, speech_frames, total / 16000.0), flush=True)
                await self.respond(ws, send_json)
            finally:
                speech_frames = 0
                silence_frames = 0
                audio_buf = []
                self.busy = False

        try:
            async for raw in ws:
                if isinstance(raw, (bytes, bytearray)):
                    # ---- 上行音频 ----
                    try:
                        pcm = self.codec.decode(bytes(raw), 16000)
                    except Exception as e:
                        print("[mock] 解码失败:", e, flush=True)
                        continue
                    if pcm.size == 0:
                        continue
                    rms = float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)) + 1e-9)
                    db = 20 * np.log10(rms / 32768.0)
                    audio_buf.append(pcm)

                    if db > IN_SPEECH_DB:
                        speaking = True
                        silence_frames = 0
                        speech_frames += 1
                    elif speaking:
                        silence_frames += 1
                        if silence_frames >= 12:            # ~720ms 静音 = 说完了
                            speaking = False
                            await maybe_respond("静音 720ms")
                        if len(audio_buf) > 400:
                            audio_buf = audio_buf[-100:]
                    continue

                # ---- 文本帧 ----
                try:
                    msg = json.loads(raw)
                except Exception:
                    continue
                t = msg.get("type")
                self.v("[mock] 收到文本:", json.dumps(msg, ensure_ascii=False)[:120])

                if t == "hello":
                    await send_json({
                        "type": "hello", "transport": "websocket",
                        "audio_params": {"format": "opus", "sample_rate": 24000,
                                         "channels": 1, "frame_duration": 60},
                    })
                    print("[mock] 已回 hello (下行 24000Hz)", flush=True)
                    # 握手完成，服务器主动请设备开始聆听
                    await send_json({"session_id": "", "type": "listen",
                                     "state": "start", "mode": "auto"})
                elif t == "listen":
                    if msg.get("state") == "start":
                        print("[mock] 设备开始聆听", flush=True)
                    else:
                        print("[mock] 设备结束聆听 -> 送去识别", flush=True)
                        speaking = False
                        await maybe_respond("listen stop")
                elif t == "abort":
                    print("[mock] 收到打断 (%s)" % msg.get("reason"), flush=True)
                elif t == "text":
                    # 允许直接喂文本（跳过 ASR），便于无麦克风调试
                    if not self.busy:
                        await self.respond(ws, send_json, text=msg.get("text"))
        except Exception as e:
            print("[mock] 连接#%d 结束: %s" % (cid, e), flush=True)
        finally:
            print("[mock] 连接#%d 关闭" % cid, flush=True)

    async def respond(self, ws, send_json, text=None):
        if text is None:
            text = REPLIES[self.reply_i % len(REPLIES)]
            self.reply_i += 1
        self.abort_flag = False

        # 1) 识别结果
        await send_json({"session_id": "", "type": "stt", "text": text})
        # 2) 开始朗读
        await send_json({"type": "tts", "state": "start"})
        await send_json({"type": "tts", "state": "sentence_start", "text": text})
        # 3) 下发音频
        pcm = melody(text, self.codec.down_rate)
        frames = self.codec.encode_as(pcm, self.codec.down_rate)
        print("[mock] 下发 TTS 音频: %d 样本 -> %d 帧 Opus" % (pcm.size, len(frames)),
              flush=True)
        for f in frames:
            await ws.send(f)
            await asyncio.sleep(0.06)
        # 4) 结束
        await send_json({"type": "tts", "state": "stop"})
        await send_json({"type": "llm", "emotion": "calm"})


async def amain(args):
    import websockets
    srv = MockServer(args.verbose)
    print("[mock] 监听 ws://127.0.0.1:%d%s" % (args.port, PATH), flush=True)
    async with websockets.serve(srv.handler, "127.0.0.1", args.port,
                                max_size=None, ping_interval=None):
        await asyncio.Future()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8989)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    try:
        asyncio.run(amain(a))
    except KeyboardInterrupt:
        print("\n[mock] 退出")
