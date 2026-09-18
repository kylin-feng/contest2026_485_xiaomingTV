#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
real_server.py — 真后端：ASR + LLM + TTS 全走云 API（默认）或全本地（--backend local）

和 mock_xiaozhi_server.py 的关系：
  协议部分（hello / listen / Opus / tts start-stop）与 mock 完全一致，
  只把两个"假"换成"真"：
      mock 的 canned REPLIES  ->  真 ASR 认板子说的话 + 真 LLM 生成回复
      mock 的 melody() 合成音 ->  真 TTS 真人声

于是整条链变成真的：
  板子麦克风 -> 串口 -> 网关 -> ws -> [ASR] -> [LLM] -> [TTS] -> ws -> 网关 -> 板子喇叭

后端（--backend）：
  api    （默认）全部走云 API，用环境变量里的 key，密钥不落文件：
         ASR = 阿里百炼 qwen3-asr-flash（base64 音频，同步返回文本 + 语种 + 情绪）
         LLM = DeepSeek deepseek-chat（OpenAI 兼容）
         TTS = 阿里百炼 qwen-tts（返回 wav URL）
  local  全离线，不花一分钱、不外发音频，但慢：
         ASR = faster-whisper（本机已缓存 base 模型）
         LLM = Ollama（本机模型）
         TTS = Windows SAPI

用法：
    python real_server.py                                  # API 后端，端口 8990
    python real_server.py --llm deepseek --llm-model deepseek-chat
    python real_server.py --llm volcano --llm-model doubao-seed-1-6-250615
    python real_server.py --backend local --model grpo-writer:latest

需要的环境变量（api 后端）：
    BAILIAN_API_KEY     ASR(qwen3-asr-flash) + TTS(qwen-tts)
    DEEPSEEK_API_KEY    或 VOLCANO_ARK_API_KEY（--llm 切换）
"""
import argparse
import asyncio
import base64
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import voice_codec as vc
from xiaozhi_proto import PROTOCOL_VERSION  # noqa: F401

PATH = "/xiaozhi/v1/"

DASHSCOPE = "https://dashscope.aliyuncs.com/api/v1"
GEN_URL = DASHSCOPE + "/services/aigc/multimodal-generation/generation"

LLM_PROVIDERS = {
    "deepseek": ("https://api.deepseek.com/v1/chat/completions", "DEEPSEEK_API_KEY"),
    "bailian": ("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
                "BAILIAN_API_KEY"),
    "volcano": ("https://ark.cn-beijing.volces.com/api/v3/chat/completions",
                "VOLCANO_ARK_API_KEY"),
}

# 哄睡智能体的人设（对应 skills/ 下那三个 Skill 的原则）
SYSTEM_PROMPT = (
    "你是「安眠科技」，一台放在床头的哄睡设备。你在跟一个睡不着的人说话，"
    "时间是深夜，他已经躺下了。\n"
    "必须遵守：\n"
    "1. 只说话，不说教。一到两句就够，最多 40 个字。\n"
    "2. 不要提问，不要追问，不要让用户做任何需要睁眼或动手的事。\n"
    "3. 不报时间，不说「几点」「该睡了」这类话。\n"
    "4. 声音是唯一的出口，语气轻、慢、暖，像在旁边陪着。\n"
    "5. 用户说睡不着、心累、明天有事，就先接住情绪，别急着给方案。\n"
    "6. 只有用户明确问呼吸、放松的方法时，才提 4-7-8 呼吸（吸4秒/屏7秒/呼8秒）。\n"
)

SAPI_PS = r"""
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(24000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile('{wav}', $fmt)
$s.Rate = {rate}
$s.Volume = 100
$s.Speak('{text}')
$s.Dispose()
"""


def meaningful(text):
    """识别结果值不值得拿去问模型。

    **要求至少有一个汉字。** 这一条比"非空"严得多，但有必要：
    ASR 遇到非语音（噪声瞬态、碰一下桌子、扬声器串音）不会报错，而是
    随便回点什么 —— 实测拿到过「.」「I.」「I'm stuck.」。只判非空的话，
    就会拿一个句号去问 LLM，AI 一本正经回一句哄睡话术，用户听到的是
    "我没说话它自己开口了"，在哄睡场景里特别出戏。

    只认汉字是有意的取舍：本产品从头到尾是中文交互，模型 prompt、
    提示音、界面文案都是中文，中文语音不会被 qwen3-asr 识别成英文。
    真要做多语种，把这里换成"有实义字符 + 最短长度"即可。"""
    if not text:
        return False
    for ch in text:
        if "\u4e00" <= ch <= "\u9fff":
            return True
    return False


def http_json(url, key, body, timeout=90, extra=None):
    h = {"Authorization": "Bearer " + key, "Content-Type": "application/json"}
    if extra:
        h.update(extra)
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=h)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def pcm16k_to_wav_bytes(pcm: np.ndarray, rate=16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.astype(np.int16).tobytes())
    return buf.getvalue()


def wav_bytes_to_pcm24k(raw: bytes) -> np.ndarray:
    """任意采样率的 wav -> 24000Hz/16bit/mono int16（板子下行格式）。"""
    with wave.open(io.BytesIO(raw), "rb") as w:
        sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
        pcm = np.frombuffer(w.readframes(n), dtype=np.int16)
    if ch > 1:
        pcm = pcm.reshape(-1, ch).mean(axis=1).astype(np.int16)
    if sr != 24000:
        t = np.linspace(0, len(pcm) / sr, int(len(pcm) * 24000 / sr), endpoint=False)
        pcm = np.interp(t, np.arange(len(pcm)) / sr, pcm).astype(np.int16)
    return pcm


def sapi_tts(text: str, rate: int = -1) -> np.ndarray:
    text = (text or "").strip()
    if not text:
        return np.zeros(0, dtype=np.int16)
    wav = os.path.join(tempfile.gettempdir(), "real_srv_tts.wav")
    safe = text.replace("'", "''")
    ps = SAPI_PS.format(wav=wav.replace("\\", "\\\\"), rate=rate, text=safe)
    subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                   capture_output=True, timeout=60)
    if not os.path.exists(wav):
        return np.zeros(0, dtype=np.int16)
    with open(wav, "rb") as f:
        return wav_bytes_to_pcm24k(f.read())


class RealServer:
    def __init__(self, args):
        self.args = args
        self.n = 0
        self.codec = vc.OpusCodec(16000, 24000, 60)
        self.busy = False
        self.history = [{"role": "system", "content": SYSTEM_PROMPT}]
        self.whisper = None
        self.wake = ""

    # ================= 三个真环节：API =================

    def asr_api(self, pcm: np.ndarray) -> str:
        key = os.environ.get("BAILIAN_API_KEY", "")
        if not key:
            raise RuntimeError("缺 BAILIAN_API_KEY")
        b64 = "data:audio/wav;base64," + base64.b64encode(
            pcm16k_to_wav_bytes(pcm)).decode()
        d = http_json(GEN_URL, key, {
            "model": "qwen3-asr-flash",
            "input": {"messages": [{"role": "user", "content": [{"audio": b64}]}]},
            "parameters": {"asr_options": {"enable_lid": True, "enable_itn": False}},
        })
        try:
            return d["output"]["choices"][0]["message"]["content"][0]["text"].strip()
        except Exception:
            return ""

    def llm_api(self, user_text: str) -> str:
        url, envname = LLM_PROVIDERS[self.args.llm]
        key = os.environ.get(envname, "")
        if not key:
            raise RuntimeError("缺 " + envname)
        self.history.append({"role": "user", "content": user_text})
        d = http_json(url, key, {
            "model": self.args.llm_model,
            "messages": self.history[-9:],
            "max_tokens": 80, "temperature": 0.7, "stream": False,
        }, timeout=120)
        reply = (d["choices"][0]["message"]["content"] or "").strip()
        self.history.append({"role": "assistant", "content": reply})
        return reply

    def tts_api(self, text: str) -> np.ndarray:
        key = os.environ.get("BAILIAN_API_KEY", "")
        if not key:
            raise RuntimeError("缺 BAILIAN_API_KEY")
        d = http_json(GEN_URL, key, {
            "model": "qwen-tts",
            "input": {"text": text, "voice": self.args.voice},
        })
        url = d["output"]["audio"]["url"]
        with urllib.request.urlopen(url, timeout=60) as r:
            return wav_bytes_to_pcm24k(r.read())

    # ================= 三个真环节：本地 =================

    def load_local(self):
        from faster_whisper import WhisperModel
        t = time.time()
        self.whisper = WhisperModel(self.args.asr_model, device="cpu",
                                    compute_type="int8")
        print("[asr ] 本地模型 %s 加载完成 %.1fs" % (self.args.asr_model,
                                                     time.time() - t), flush=True)
        self.whisper.transcribe(np.zeros(16000, dtype=np.float32), language="zh")

    def asr_local(self, pcm: np.ndarray) -> str:
        segs, _ = self.whisper.transcribe(pcm.astype(np.float32) / 32768.0,
                                          language="zh", beam_size=1, vad_filter=True)
        return "".join(s.text for s in segs).strip()

    def llm_local(self, user_text: str) -> str:
        self.history.append({"role": "user", "content": user_text})
        body = json.dumps({"model": self.args.model, "stream": False,
                           "messages": self.history[-9:],
                           "options": {"temperature": 0.7, "num_predict": 80}}).encode()
        req = urllib.request.Request(self.args.ollama + "/api/chat", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=180) as r:
            reply = (json.load(r).get("message") or {}).get("content", "").strip()
        self.history.append({"role": "assistant", "content": reply})
        return reply

    # ================= 统一入口 =================

    def asr(self, pcm):
        if pcm.size < 8000:
            return ""
        if self.args.backend == "local":
            return self.asr_local(pcm)
        return self.asr_api(pcm)

    def llm(self, text):
        if self.args.backend == "local":
            return self.llm_local(text)
        return self.llm_api(text)

    def tts(self, text):
        if self.args.backend == "local":
            return sapi_tts(text, self.args.tts_rate)
        return self.tts_api(text)

    # ================= 协议（与 mock 一致） =================

    async def handler(self, ws):
        self.n += 1
        cid = self.n
        print("[srv ] 连接#%d  backend=%s" % (cid, self.args.backend), flush=True)
        audio_buf, speech_frames, silence_frames, speaking = [], 0, 0, False
        IN_DB = -40.0

        async def send_json(d):
            await ws.send(json.dumps(d, ensure_ascii=False))

        async def fire(why):
            nonlocal speech_frames, silence_frames, audio_buf, speaking
            if self.busy or speech_frames < 4:
                return
            self.busy = True
            pcm = np.concatenate(audio_buf) if audio_buf else np.zeros(0, np.int16)
            try:
                print("[srv ] 触发回复（%s）：%d 帧 / %.2fs"
                      % (why, speech_frames, pcm.size / 16000.0), flush=True)
                await self.respond(ws, send_json, pcm=pcm)
            except Exception as e:
                print("[srv ] 回复失败:", e, flush=True)
            finally:
                speech_frames = silence_frames = 0
                audio_buf = []
                speaking = False
                self.busy = False

        try:
            async for raw in ws:
                if isinstance(raw, (bytes, bytearray)):
                    try:
                        pcm = self.codec.decode(bytes(raw), 16000)
                    except Exception:
                        continue
                    if pcm.size == 0:
                        continue
                    rms = float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)) + 1e-9)
                    db = 20 * np.log10(rms / 32768.0)
                    audio_buf.append(pcm)
                    if db > IN_DB:
                        speaking, silence_frames = True, 0
                        speech_frames += 1
                    elif speaking:
                        silence_frames += 1
                        if silence_frames >= 12:
                            speaking = False
                            await fire("静音 720ms")
                        if len(audio_buf) > 400:
                            audio_buf = audio_buf[-100:]
                    continue

                try:
                    msg = json.loads(raw)
                except Exception:
                    continue
                t = msg.get("type")
                if t == "hello":
                    await send_json({"type": "hello", "transport": "websocket",
                                     "audio_params": {"format": "opus", "sample_rate": 24000,
                                                      "channels": 1, "frame_duration": 60}})
                    print("[srv ] 回 hello（下行 24000Hz）", flush=True)
                    await send_json({"session_id": "", "type": "listen",
                                     "state": "start", "mode": "auto"})
                elif t == "listen":
                    if msg.get("state") == "start":
                        print("[srv ] 设备开始聆听", flush=True)
                    else:
                        speaking = False
                        await fire("listen stop")
                elif t == "abort":
                    print("[srv ] 收到打断 (%s)" % msg.get("reason"), flush=True)
                elif t == "text":
                    if not self.busy:
                        self.busy = True
                        try:
                            await self.respond(ws, send_json, text=msg.get("text"))
                        finally:
                            self.busy = False
        except Exception as e:
            print("[srv ] 连接#%d 结束: %s" % (cid, e), flush=True)
        finally:
            print("[srv ] 连接#%d 关闭" % cid, flush=True)

    async def respond(self, ws, send_json, pcm=None, text=None):
        t0 = time.time()
        # 1) ASR
        if text is None:
            text = await asyncio.to_thread(self.asr, pcm) if pcm is not None else ""
        if not meaningful(text):
            # 不能只判 text.strip() 非空：ASR 遇到非语音（噪声瞬态、碰一下
            # 桌子）不报错，而是回一个标点 —— 实测拿到过「.」。那样就会拿
            # 一个句号去问 LLM，AI 一本正经回一句哄睡话术，用户听到的是
            # "我没说话它自己开口了"，在哄睡场景里特别出戏。
            print("[asr ] 没听清（%s），这轮跳过" % ("空结果" if not text.strip()
                                                else "只有标点「%s」" % text.strip()),
                  flush=True)
            await send_json({"session_id": "", "type": "listen", "state": "start",
                             "mode": "auto"})
            return
        print("[asr ] 「%s」  (%.1fs)" % (text, time.time() - t0), flush=True)
        await send_json({"session_id": "", "type": "stt", "text": text})

        # 2) LLM
        t1 = time.time()
        reply = await asyncio.to_thread(self.llm, text)
        print("[llm ] 「%s」  (%.1fs)" % (reply, time.time() - t1), flush=True)

        # 3) TTS
        t2 = time.time()
        pcm_out = await asyncio.to_thread(self.tts, reply)
        frames = self.codec.encode_as(pcm_out, self.codec.down_rate)
        print("[tts ] 合成 %d 样本 -> %d 帧 Opus  (%.1fs)"
              % (pcm_out.size, len(frames), time.time() - t2), flush=True)

        await send_json({"type": "tts", "state": "start"})
        await send_json({"type": "tts", "state": "sentence_start", "text": reply})

        # ---- 下行供料节奏：先垫底，再按绝对时间表供 ----
        #
        # 1) **先连发 PRIME 帧垫底**，给板子的播放缓冲留出余量。
        #    板子的播放环形缓冲是 AUD_PCM_RING = 16384 样本 = 1.024 秒
        #    （见 board/bsp_audio_test.c）。而按实时供料时缓冲里只有
        #    "刚到的那一帧" —— 实测 PC 侧读到的 待放= 在 0 / 448 / 960
        #    之间跳（448 样本 = 28ms）。缓冲没有余量，PC 端任何一次调度
        #    抖动都直接变成断音，听感就是"一卡一卡"。
        #
        #    注意：这里早先有一版注释写"板子播放环只有 ~64ms，所以不能
        #    垫缓冲"，那个结论是错的（环就是 1.024 秒，max 待放 会达到
        #    16000+）。当时的供料节奏是照着这个错误结论设计的，一并纠正。
        #
        # 2) **之后按绝对时间表（第 i 帧应在 t0 + i*60ms 发出）而不是
        #    每帧 sleep(60ms)**。后者会把每帧几毫秒的调度误差累加起来：
        #    实测整段供料只有 0.71~0.98x 实时，低于 1.0 就是必然欠载。
        #    绝对时间表下，某帧晚了就直接不睡、把进度追回来，长期速率
        #    严格等于 1.0x，垫下的余量不会被误差吃掉。
        # 垫底要**尽量接近**板子播放环的深度（AUD_PCM_RING = 16384 样本
        # = 1.024 秒）。为什么：μ-law 之后下行的数据率（16KB/s）正好等于板子
        # 的消耗速率（16kHz×1 字节），也就是说**零余量**，链路只要丢一点帧，
        # 播放环就会被抽干、听感立刻变回"一卡一卡"。
        # 垫 15 帧 = 900ms，留出 12% 的丢帧容忍度（环满了会自动截断，不会溢出
        # 出杂音，见 bsp_audio_play_pcm 的 free_n 处理）。
        PRIME_FRAMES = 15           # 15 × 60ms = 900ms 余量
        t_dl = time.monotonic()
        n_sent = 0
        for i, f in enumerate(frames):
            await ws.send(f)
            n_sent += 1
            if i < PRIME_FRAMES:
                continue
            dt = (t_dl + i * 0.060) - time.monotonic()
            if dt > 0:
                await asyncio.sleep(dt)
        print("[srv ] 下行 %d 帧，实际用时 %.2fs（音频 %.2fs，等效 %.2fx 实时）"
              % (n_sent, time.monotonic() - t_dl,
                 n_sent * 0.060, n_sent * 0.060 / max(0.01, time.monotonic() - t_dl)),
              flush=True)
        await send_json({"type": "tts", "state": "stop"})
        await send_json({"type": "llm", "emotion": "calm"})
        print("[srv ] 本轮总耗时 %.1fs" % (time.time() - t0), flush=True)
        await send_json({"session_id": "", "type": "listen", "state": "start",
                         "mode": "auto"})


async def amain(args):
    import websockets
    srv = RealServer(args)
    if args.backend == "local":
        srv.load_local()
    else:
        need = ["BAILIAN_API_KEY", LLM_PROVIDERS[args.llm][1]]
        miss = [k for k in need if not os.environ.get(k)]
        if miss:
            print("[srv ] !! 缺环境变量:", miss, flush=True)
    print("[srv ] 监听 ws://127.0.0.1:%d%s  backend=%s  LLM=%s/%s"
          % (args.port, PATH, args.backend, args.llm, args.llm_model), flush=True)
    async with websockets.serve(srv.handler, "127.0.0.1", args.port,
                                max_size=None, ping_interval=None):
        await asyncio.Future()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8990)
    ap.add_argument("--backend", choices=["api", "local"], default="api")
    # API 后端
    ap.add_argument("--llm", choices=list(LLM_PROVIDERS), default="deepseek")
    ap.add_argument("--llm-model", default="deepseek-chat")
    ap.add_argument("--voice", default="Cherry", help="qwen-tts 音色")
    # 本地后端
    ap.add_argument("--asr-model", default="base", help="faster-whisper 模型")
    ap.add_argument("--model", default="grpo-writer:latest", help="Ollama 模型")
    ap.add_argument("--ollama", default="http://127.0.0.1:11434")
    ap.add_argument("--tts-rate", type=int, default=-1, help="SAPI 语速")
    a = ap.parse_args()
    try:
        asyncio.run(amain(a))
    except KeyboardInterrupt:
        print("\n[srv ] 退出")
