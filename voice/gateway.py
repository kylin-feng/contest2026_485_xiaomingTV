#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gateway.py — 眠语语音对话网关（把小智的对话能力接给 SF32LB52 板子）

【为什么是这个架构】
小智（xiaozhi-esp32）的固件是 ESP32-S3 专用的，我们的板子是 SF32LB52 / openvela，
指令集和 SDK 都不同，固件搬不过来。但小智的**架构**是：

    设备：唤醒 + 采音 + 放音   ← 薄，几乎不含智能
    服务器：VAD → ASR → LLM → TTS   ← 厚，全部智能
    中间：一套 WebSocket 协议

只要有人替板子说这套协议，板子就等于一台小智设备。网关就是干这个的：

    ┌── 眠语板 SF32LB52 ──┐  串口 1.5M   ┌── 本网关 ──┐  ws  ┌─ 小智服务器 ─┐
    │ MIC→PCM   PCM→喇叭  │ ═══════════▶ │ PCM↔Opus   │ ───▶ │ VAD ASR LLM  │
    │ (ADC)     (DAC+PA10)│ ◀═══════════ │ 唤醒/打断   │ ◀─── │ TTS          │
    └─────────────────────┘              └────────────┘      └──────────────┘

网关放在 PC / 树莓派 / 手机上（板子没有 WiFi，只有 BLE；串口是最省事、最稳的通道，
而且那根线本来就插着——就是烧录用的 Type-C ①号口）。

【三种用法】
  1) 接官方/自建小智服务器：
       python gateway.py --server ws://127.0.0.1:8989/xiaozhi/v1/ --token 你的token
  2) 离线自测（不需要服务器、不需要板子，验证整条链路）：
       python mock_xiaozhi_server.py &      # 先起假服务器
       python gateway.py --server ws://127.0.0.1:8989/xiaozhi/v1/ --source wav --sink null
  3) 只跑网关不发音频（检查串口与协议）：
       python gateway.py --source board --sink null --wake key
"""
import argparse
import asyncio
import json
import os
import queue
import signal
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from board_link import (SerialBoardLink, T_AUDIO_UP, T_AUDIO_DOWN, T_EVENT,
                        T_CMD, T_PING, T_LOG, CMD_SET_AMP, CMD_PLAY_STOP,
                        CMD_SET_MIC, CMD_TONE, EV_KEY_DOWN, EV_KEY_UP,
                        EV_BOOT_READY, EV_NAMES, T_NAMES)
import voice_codec as vc
from xiaozhi_proto import XiaozhiProto, UP_SAMPLE_RATE, FRAME_MS

BOARD_RATE = 16000          # 板子音频框架的采样率（与 HAL 约定一致）


def log(tag, msg):
    print("%s [%-8s] %s" % (time.strftime("%H:%M:%S"), tag, msg), flush=True)


# ==========================================================================
# 音频端点：板子 / PC 声卡 / 假数据(离线自测)
# ==========================================================================

class BoardAudio:
    """板子作为音频端点：MIC 上行、PCM 下行。"""

    name = "board"

    def __init__(self, link: SerialBoardLink):
        self.link = link
        self.tx_pcm = queue.Queue(maxsize=64)      # 待下发的 PCM
        self.rx_pcm = queue.Queue(maxsize=256)     # 收到的 PCM
        self.events = queue.Queue(maxsize=64)
        self.playing = False
        self.mic_on = True

    def feed_frames(self, frames):
        """把 poll() 到的帧分发到对应队列。"""
        for ftype, seq, payload in frames:
            if ftype == T_AUDIO_UP:
                if payload:
                    self._push(self.rx_pcm, payload)
            elif ftype == T_EVENT:
                self._push(self.events, payload)
            elif ftype == T_LOG:
                log("board", payload.decode("utf-8", "replace").rstrip())

    @staticmethod
    def _push(q, item):
        try:
            q.put_nowait(item)
        except queue.Full:
            try:                       # 丢最旧的，保新的：语音里新数据更有价值
                q.get_nowait()
                q.put_nowait(item)
            except Exception:
                pass

    def read_pcm(self, max_samples=960):
        """取一段上行 PCM（可能比 max_samples 多，截断即可）。"""
        buf = bytearray()
        while len(buf) < max_samples * 2:
            try:
                buf += self.rx_pcm.get_nowait()
            except queue.Empty:
                break
        if not buf:
            return np.zeros(0, dtype=np.int16)
        arr = np.frombuffer(bytes(buf[:max_samples * 2]), dtype=np.int16)
        return arr

    def write_pcm(self, pcm: np.ndarray):
        self.link.send_audio_down(pcm.astype(np.int16).tobytes())

    def play_stop(self):
        self.link.send_cmd(CMD_PLAY_STOP)

    def tone(self, kind):
        self.link.send_cmd(CMD_TONE, {"listen": 1, "done": 2, "error": 3}.get(kind, 0))

    def set_amp(self, on):
        self.link.send_cmd(CMD_SET_AMP, 1 if on else 0)

    def set_mic(self, on):
        if on != self.mic_on:
            self.mic_on = on
            self.link.send_cmd(CMD_SET_MIC, 1 if on else 0)

    def close(self):
        pass


class PcAudio:
    """PC 声卡作为音频端点（调试网关本身用，不需要板子）。"""

    name = "pc"

    def __init__(self, rate=BOARD_RATE):
        import sounddevice as sd
        self.sd = sd
        self.rate = rate
        self.q = queue.Queue(maxsize=256)
        self.out = None
        self.playing = False

        def cb(indata, frames, t, status):
            if status:
                pass
            self._push(self.q, bytes(indata))

        try:
            self.stream = sd.InputStream(
                samplerate=rate, channels=1, dtype="int16",
                blocksize=int(rate * FRAME_MS / 1000), callback=cb)
            self.stream.start()
        except Exception as e:
            raise RuntimeError("无法打开麦克风：%s" % e)
        self.events = queue.Queue(maxsize=64)

    @staticmethod
    def _push(q, item):
        try:
            q.put_nowait(item)
        except queue.Full:
            try:
                q.get_nowait(); q.put_nowait(item)
            except Exception:
                pass

    def feed_frames(self, frames):
        pass

    def read_pcm(self, max_samples=960):
        buf = bytearray()
        while len(buf) < max_samples * 2:
            try:
                buf += self.q.get_nowait()
            except queue.Empty:
                break
        if not buf:
            return np.zeros(0, dtype=np.int16)
        return np.frombuffer(bytes(buf[:max_samples * 2]), dtype=np.int16)

    def write_pcm(self, pcm: np.ndarray):
        try:
            if self.out is None or not self.playing:
                self.out = self.sd.OutputStream(
                    samplerate=self.rate, channels=1, dtype="int16")
                self.out.start()
                self.playing = True
            self.out.write(np.ascontiguousarray(pcm, dtype=np.int16))
        except Exception as e:
            log("pc-audio", "播放失败: %s" % e)

    def play_stop(self):
        try:
            if self.out:
                self.out.abort()
                self.out.close()
        except Exception:
            pass
        self.out = None
        self.playing = False

    def tone(self, kind):
        t = vc.tone(kind, self.rate)
        if t.size:
            self.write_pcm(t)

    def set_amp(self, on):
        pass

    def set_mic(self, on):
        pass

    def close(self):
        self.play_stop()
        try:
            self.stream.stop(); self.stream.close()
        except Exception:
            pass


class WavAudio:
    """用 wav 文件冒充麦克风（source=wav）。

    调语音链路时最烦的是"对着麦克风喊一句、等两秒、看日志"，一轮下来十几秒，
    而且每次喊的内容还不一样。用一段录好的 wav 当输入，整个链路就变成**可复现**
    的：同一段音频进去，出的文本/波形每次都该一样，断言才有意义。

    按实时速度吐样本（不是一次全给），否则 VAD 的滞回、断句全乱套。
    """

    name = "wav"

    def __init__(self, path, rate=BOARD_RATE, loop=False):
        import wave
        self.rate = rate
        self.loop = loop
        self.events = queue.Queue(maxsize=64)
        self.playing = False
        self.done = False
        self.sent = 0
        with wave.open(path, "rb") as w:
            nch, sw, sr = w.getnchannels(), w.getsampwidth(), w.getframerate()
            raw = w.readframes(w.getnframes())
        if sw != 2:
            raise RuntimeError("只支持 16bit wav（现在是 %d bit）" % (sw * 8))
        arr = np.frombuffer(raw, dtype=np.int16)
        if nch > 1:                      # 多声道 -> 取第一声道
            arr = arr.reshape(-1, nch)[:, 0].copy()
        if sr != rate:
            arr = vc.resample(arr, sr, rate)
        self.pcm = np.ascontiguousarray(arr, dtype=np.int16)
        self.src_rate = sr
        self._t0 = None
        self._pos = 0

    def feed_frames(self, frames):
        pass

    def read_pcm(self, max_samples=960):
        if self._t0 is None:
            self._t0 = time.time()
        if self.done:
            return np.zeros(0, dtype=np.int16)
        # 时刻 t 应该已经消费到第几个样本 —— 用时间反推位置，天然抗抖动
        want = int((time.time() - self._t0) * self.rate)
        n = min(max_samples, max(0, want - self._pos))
        if n <= 0:
            return np.zeros(0, dtype=np.int16)
        chunk = self.pcm[self._pos:self._pos + n]
        self._pos += chunk.size
        self.sent += chunk.size
        if self._pos >= self.pcm.size:
            if self.loop:
                self._pos = 0
                self._t0 = time.time()
            else:
                self.done = True
                log("wav", "文件放完（共 %d 样本 / %.2fs）"
                    % (self.pcm.size, self.pcm.size / float(self.rate)))
        return chunk

    def write_pcm(self, pcm):
        pass

    def play_stop(self):
        pass

    def tone(self, kind):
        log("tone", "本地提示音: %s" % kind)

    def set_amp(self, on):
        pass

    def set_mic(self, on):
        pass

    def close(self):
        pass


class NullAudio:
    """不出声、不录音。用于纯协议测试 / 只测串口。"""

    name = "null"

    def __init__(self):
        self.events = queue.Queue(maxsize=64)
        self.play_calls = 0
        self.up_calls = 0
        self.samples_down = 0

    def feed_frames(self, frames):
        pass

    def read_pcm(self, max_samples=960):
        return np.zeros(0, dtype=np.int16)

    def write_pcm(self, pcm: np.ndarray):
        self.play_calls += 1
        self.samples_down += int(pcm.size)

    def play_stop(self):
        pass

    def tone(self, kind):
        log("tone", "本地提示音: %s" % kind)

    def set_amp(self, on):
        pass

    def set_mic(self, on):
        pass

    def close(self):
        pass


# ==========================================================================
# 语音活动检测（网关侧，省带宽 + 决定何时开/关聆听）
# ==========================================================================

class EnergyVad:
    """极简能量 VAD。

    小智设备用 ESP-SR 做本地唤醒词；我们芯片不同、也不值得为它训模型。
    对"哄睡"这个场景，能量 VAD 足够：安静环境下的说话起始点非常明显。
    需要真唤醒词时再换 openwakeword/sherpa-onnx，接口不变。

    带滞回：起始阈值高（防误触），维持阈值低（防断句）。
    """

    def __init__(self, start_db=-38.0, keep_db=-45.0, hang_ms=700):
        self.start_db = start_db
        self.keep_db = keep_db
        self.hang_frames = max(1, int(hang_ms / FRAME_MS))
        self.active = False
        self._silent = 0

    @staticmethod
    def _db(pcm):
        if pcm.size == 0:
            return -120.0
        rms = float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)) + 1e-9)
        return 20.0 * np.log10(rms / 32768.0)

    def update(self, pcm):
        """返回 (is_speech, just_started, just_ended)"""
        d = self._db(pcm)
        started = ended = False
        if not self.active:
            if d > self.start_db:
                self.active = True
                self._silent = 0
                started = True
        else:
            if d > self.keep_db:
                self._silent = 0
            else:
                self._silent += 1
                if self._silent >= self.hang_frames:
                    self.active = False
                    ended = True
        return self.active, started, ended

    def reset(self):
        self.active = False
        self._silent = 0


# ==========================================================================
# 网关主逻辑
# ==========================================================================

class Gateway:
    def __init__(self, cfg):
        self.cfg = cfg
        self.proto = XiaozhiProto(cfg["device_id"], token=cfg["token"])
        self.codec = None
        self.audio = None
        self.link = None
        self.vad = EnergyVad()
        self.state = "IDLE"          # IDLE / LISTENING / SPEAKING
        self.turn = 0                # 对话轮次：每开始一轮聆听/打断就 +1
        self._tts_turn = -1          # 当前这轮 TTS 属于哪一轮
        self.quit = False
        self.stats = {"up_frames": 0, "down_frames": 0, "aborts": 0,
                      "up_opus": 0, "stale_down": 0, "sessions": 0, "t0": time.time()}
        self._last_ping = time.time()
        self._first_hello_done = False

    # ---------------- 设备侧链路 ----------------

    def start_link(self):
        if self.cfg["source"] != "board" and self.cfg["sink"] != "board":
            return
        port = self.cfg["port"]
        self.link = SerialBoardLink(port, self.cfg["baud"])
        self.link.open()
        log("serial", "已连接 %s @%d (CH343)" % (self.link.port, self.cfg["baud"]))
        threading.Thread(target=self._link_reader, daemon=True).start()

    def _link_reader(self):
        """持续收板子来的帧，分发到 audio 端点。串口读放独立线程，
        避免和 asyncio 事件循环互相拖累。"""
        while not self.quit and self.link:
            try:
                frames = self.link.poll()
                if frames:
                    self.audio.feed_frames(frames)
                    for ft, sq, pl in frames:
                        if ft == T_AUDIO_UP:
                            self.stats["up_frames"] += 1
                        elif ft == T_AUDIO_DOWN:
                            self.stats["down_frames"] += 1
            except Exception as e:
                log("serial", "读取异常: %s" % e)
                time.sleep(0.2)
            time.sleep(0.002)

    def start_audio(self):
        src, sink = self.cfg["source"], self.cfg["sink"]
        want_board = "board" in (src, sink)
        if want_board:
            self.audio = BoardAudio(self.link)
        elif src == "wav" or sink == "wav":
            self.audio = WavAudio(self.cfg["wav"], loop=self.cfg["wav_loop"])
            log("audio", "wav 输入: %s（%.2fs）"
                % (self.cfg["wav"], self.audio.pcm.size / float(BOARD_RATE)))
        elif "pc" in (src, sink):
            self.audio = PcAudio()
            log("audio", "PC 声卡已打开（%d Hz）" % BOARD_RATE)
        else:
            self.audio = NullAudio()
        log("audio", "音频端点: source=%s sink=%s" % (src, sink))
        self.codec = vc.OpusCodec(UP_SAMPLE_RATE, 24000, FRAME_MS)

    # ---------------- 主循环 ----------------

    async def run(self):
        import websockets
        url = self.cfg["server"]
        backoff = 1.0
        while not self.quit:
            try:
                log("ws", "连接 %s" % url)
                async with websockets.connect(
                        url,
                        additional_headers=self.proto.headers(),
                        max_size=None, ping_interval=None) as ws:
                    backoff = 1.0
                    await self._session(ws)
            except Exception as e:
                if self.quit:
                    break
                log("ws", "断开: %s（%.1fs 后重连）" % (e, backoff))
                await asyncio.sleep(backoff)
                backoff = min(backoff * 1.6, 15.0)

    async def _session(self, ws):
        self._ws_ref = ws          # 先登记，免得 hello 回包比 mic_loop 启动更早
        await ws.send(self.proto.hello())
        log("ws", "已发 hello，等服务器应答")

        tasks = [asyncio.create_task(self._rx_loop(ws)),
                 asyncio.create_task(self._mic_loop(ws))]
        if self.cfg["wake"] == "manual":
            tasks.append(asyncio.create_task(self._stdin_loop()))
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)
        finally:
            for t in tasks:
                t.cancel()

    async def _stdin_loop(self):
        """wake=manual：终端敲回车 = 现在开始说话，再敲 = 说完了。

        比 VAD 更适合"我在调 ASR/LLM 提示词"的场景：不用喊，不用等静音判定，
        节奏完全由人控制。
        """
        loop = asyncio.get_event_loop()
        while not self.quit:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:
                return
            ws = self._ws_ref
            if ws is None:
                continue
            if self.state == "LISTENING":
                await ws.send(self.proto.listen_stop())
                self.state = "IDLE"
                log("state", "手动结束聆听")
            elif self.state == "SPEAKING":
                await self._barge_in(ws)
            else:
                await self._begin_listen()

    async def _rx_loop(self, ws):
        async for raw in ws:
            if isinstance(raw, (bytes, bytearray)):
                self._on_down_audio(bytes(raw))
            else:
                self._on_text(raw)

    def _on_text(self, raw):
        ev = self.proto.parse(raw)
        t = ev["type"]
        if t == "hello":
            sr = ev["down_sample_rate"]
            if sr != self.codec.down_rate:
                self.codec.set_down_rate(sr)
            log("ws", "服务器 hello：下行 %d Hz -> 已切解码器" % sr)
            self._first_hello_done = True
            # 握手完成后立刻开始聆听
            asyncio.create_task(self._begin_listen())
        elif t == "stt":
            log("asr", "「%s」" % ev["text"])
        elif t == "tts":
            st = ev["state"]
            if st == "start":
                self._tts_turn = self.turn
                self.state = "SPEAKING"
                self.audio.play_stop()
                log("tts", "开始朗读")
            elif st == "sentence_start":
                log("tts", "句: %s" % ev["text"])
            elif st == "stop":
                # 关键：被打断的那一轮，服务器仍然会把 tts stop 发完。
                # 如果无条件接受它，就会把刚建立的"新一轮聆听"状态打回 IDLE，
                # 用户下一句话直接丢——表现成"打断之后就再也不理我了"。
                if self.turn == self._tts_turn:
                    self.state = "IDLE"
                    self.audio.tone("done")
                    log("tts", "朗读结束")
                else:
                    log("tts", "忽略上一轮的 tts stop（轮次已作废）")
        elif t == "llm":
            if ev["emotion"]:
                log("llm", "情感: %s" % ev["emotion"])
        else:
            log("ws", "收到 %s: %s" % (t, json.dumps(ev.get("payload", ev),
                                                     ensure_ascii=False)[:160]))

    def _on_down_audio(self, opus_frame):
        # 被打断后服务器可能还有几帧音频在路上，丢掉——否则会出现
        # "说了打断、它还在响"，在哄睡场景里很出戏
        if self.turn != self._tts_turn:
            self.stats["stale_down"] += 1
            return
        try:
            pcm = self.codec.decode(opus_frame, BOARD_RATE)
        except Exception as e:
            log("opus", "解码失败: %s" % e)
            return
        if pcm.size:
            self.stats["down_frames"] += 1
            self.audio.write_pcm(pcm)

    async def _begin_listen(self):
        self.turn += 1
        await self._pending_send(self.proto.listen_start("auto"))
        self.state = "LISTENING"
        self.vad.reset()
        log("state", "聆听中（VAD 自动断句）")

    _ws_ref = None
    async def _pending_send(self, payload):
        ws = self._ws_ref
        if ws is not None:
            await ws.send(payload)

    async def _mic_loop(self, ws):
        """上行：读板子/声卡 PCM → 能量 VAD → Opus → 发服务器。"""
        self._ws_ref = ws
        buf = np.zeros(0, dtype=np.int16)
        frame_n = UP_SAMPLE_RATE * FRAME_MS // 1000

        while not self.quit:
            await asyncio.sleep(FRAME_MS / 1000.0)

            # 板子事件（按键 = 用户想说话 / 打断）
            self._drain_events(ws)

            pcm = self.audio.read_pcm(frame_n)
            if pcm.size == 0:
                pcm = np.zeros(frame_n, dtype=np.int16)

            speech, started, ended = self.vad.update(pcm)

            if self.cfg["wake"] == "vad":
                if started and self.state != "LISTENING":
                    if self.state == "SPEAKING":
                        await self._barge_in(ws)
                    await self._begin_listen()
                if ended and self.state == "LISTENING":
                    await ws.send(self.proto.listen_stop())
                    self.state = "IDLE"
                    log("state", "静音，停止聆听")

            # 只在聆听态上传音频（说话时上传会把 AI 自己的声音喂回去）
            if self.state == "LISTENING":
                buf = np.concatenate([buf, pcm]) if buf.size else pcm
                while buf.size >= frame_n:
                    chunk, buf = buf[:frame_n], buf[frame_n:]
                    for f in self.codec.encode(chunk):
                        await ws.send(f)
                        self.stats["up_opus"] += 1
                self.audio.set_mic(True)
            else:
                buf = np.zeros(0, dtype=np.int16)
                self.audio.set_mic(False)

            # 保活
            if time.time() - self._last_ping > 20:
                self._last_ping = time.time()
                if self.link:
                    self.link.send_ping()
                await ws.ping()

    def _drain_events(self, ws):
        while True:
            try:
                ev = self.audio.events.get_nowait()
            except queue.Empty:
                return
            code = ev[0] if ev else 0
            name = EV_NAMES.get(code, "0x%02x" % code)
            if name == "BOOT_READY":
                log("board", "板子就绪")
            elif name == "KEY_DOWN":
                self._on_key_down(ws)
            elif name == "KEY_UP":
                log("key", "松开")

    _key_task = None
    def _on_key_down(self, ws):
        log("key", "按下")
        self._key_task = asyncio.create_task(self._handle_key(ws))

    async def _handle_key(self, ws):
        if self.state == "SPEAKING":
            await self._barge_in(ws)
        elif self.state == "LISTENING":
            await ws.send(self.proto.listen_stop())
            self.state = "IDLE"
            log("state", "手动停止聆听")
        else:
            await self._begin_listen()

    async def _barge_in(self, ws):
        """打断：本地立刻停播 + 通知服务器中止当前回合。"""
        self.stats["aborts"] += 1
        self.turn += 1               # 作废当前轮：迟到的 tts stop / 音频都会被丢
        self.audio.play_stop()
        await ws.send(self.proto.abort("wake_word_detected"))
        self.state = "IDLE"
        log("state", "已打断")

    # ---------------- 维护 ----------------

    def shutdown(self):
        self.quit = True
        self.stats["uptime"] = time.time() - self.stats["t0"]
        try:
            if self.audio:
                self.audio.close()
        except Exception:
            pass
        try:
            if self.link:
                if self.link.stats()["crc_err"] or self.link.stats()["resync"]:
                    log("serial", "链路统计: %s" % self.link.stats())
                self.link.close()
        except Exception:
            pass
        log("stats", "板子帧 %d 上行 / %d 下行; Opus %d 上行 / %d 下行; "
                     "打断 %d; 丢弃过期下行 %d"
            % (self.stats["up_frames"], self.stats["down_frames"],
               self.stats["up_opus"], self.stats["down_frames"],
               self.stats["aborts"], self.stats["stale_down"]))


# ==========================================================================

def load_cfg(args):
    cfg = {
        "server": "ws://127.0.0.1:8989/xiaozhi/v1/",
        "token": "",
        "device_id": "mianyu-sf32lb52-01",
        "port": None,
        "baud": 1500000,
        "source": "board",
        "sink": "board",
        "wake": "vad",
        "wav": None,
        "wav_loop": False,
    }
    path = args.config or os.path.join(HERE, "config.json")
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                cfg.update(json.load(f))
            log("cfg", "已读配置 %s" % path)
        except Exception as e:
            log("cfg", "配置读取失败(%s)，用默认值" % e)
    for k in ("server", "token", "device_id", "port", "baud",
              "source", "sink", "wake", "wav", "wav_loop"):
        v = getattr(args, k, None)
        if v is not None:
            cfg[k] = v
    return cfg


def main():
    ap = argparse.ArgumentParser(description="眠语语音对话网关")
    ap.add_argument("--config", help="配置文件路径 (默认 voice/config.json)")
    ap.add_argument("--server", help="小智服务器 ws 地址")
    ap.add_argument("--token", help="Authorization Bearer token")
    ap.add_argument("--device-id", dest="device_id", help="设备唯一 ID")
    ap.add_argument("--port", help="串口，如 COM6（默认自动找 CH343）")
    ap.add_argument("--baud", type=int, help="串口波特率（默认 1500000）")
    ap.add_argument("--source", choices=["board", "pc", "wav", "null"],
                    help="麦克风来源")
    ap.add_argument("--sink", choices=["board", "pc", "wav", "null"],
                    help="播放去向")
    ap.add_argument("--wav", help="source=wav 时用作输入的 16bit wav 文件")
    ap.add_argument("--wav-loop", dest="wav_loop", action="store_true",
                    help="wav 放完后循环")
    ap.add_argument("--wake", choices=["vad", "key", "manual"],
                    help="触发方式：vad 能量检测 / key 板载按键 / manual 敲回车")
    args = ap.parse_args()

    cfg = load_cfg(args)
    log("cfg", "server=%s source=%s sink=%s wake=%s"
        % (cfg["server"], cfg["source"], cfg["sink"], cfg["wake"]))

    if not vc.have_opus():
        log("fatal", "没有 libopus。请 `pip install pyogg`（自带 opus.dll）"
                     " 或 Linux 上 `apt install libopus0`")
        return 2

    if "wav" in (cfg["source"], cfg["sink"]) and not cfg.get("wav"):
        log("fatal", "source/sink=wav 时必须给 --wav <文件>")
        return 2

    if "board" in (cfg["source"], cfg["sink"]) and not cfg.get("port"):
        log("warn", "未指定 --port，将自动搜索 CH343（1A86:55D3）")

    gw = Gateway(cfg)
    try:
        gw.start_link()
        gw.start_audio()
    except Exception as e:
        log("fatal", "初始化失败: %s" % e)
        return 3

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    def _sig(*_):
        gw.quit = True
        for t in asyncio.all_tasks(loop):
            t.cancel()
    try:
        signal.signal(signal.SIGINT, _sig)
    except Exception:
        pass

    try:
        loop.run_until_complete(gw.run())
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        gw.shutdown()
        loop.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
