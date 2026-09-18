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
import collections
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
                        T_CMD, T_PING, T_LOG, CMD_SET_AMP, CMD_SET_VOL,
                        CMD_PLAY_STOP, CMD_SET_MIC, CMD_TONE, CMD_UI_STATE,
                        CMD_SET_TIME, MAX_PAYLOAD, T_AUDIO_ULAW,
                        EV_KEY_DOWN, EV_KEY_UP, EV_BOOT_READY, EV_NAMES, T_NAMES)
import voice_codec as vc
import ulaw
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
        self._up_left = np.zeros(0, dtype=np.int16)   # 上一片凑不满时留下的余量
        # 必须是 None：板子侧的开麦状态对网关是**未知**的（可能被上一次
        # 会话、调试脚本或别的工具改过）。若这里写 True，第一次
        # set_mic(True) 会因为"值没变"而**不发命令**，板子就一直是关麦的，
        # 上行收不到任何音频，表现为"链路连着但怎么说都没反应"。
        self.mic_on = None
        # 板子上电时放音通路的音量是编解码器默认值（实测偏响），
        # 这里记一份"PC 已经设过的值"，-1 表示还没设过。
        self.volume_pct = -1
        self._dl_buf = bytearray()      # 待发的 μ-law 字节（喂料线程取走）
        self._dl_lock = threading.Lock()
        self._dl_dropped = False

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
        """取一段上行 PCM。

        必须**精确保留余量**。串口来的每片长度和这里要的 960 样本并不整除，
        旧实现是"凑够 960 就把多出来的样本直接丢掉"——丢的不只是数据量，
        而是把上行音频流切成了有洞的碎块。VAD 只关心能量、看不出来，
        但 ASR 是逐帧识别，缺帧会明显掉准确率。今改为把余量存起来下次补回。
        """
        if self._up_left.size >= max_samples:
            out = self._up_left[:max_samples]
            self._up_left = self._up_left[max_samples:]
            return out
        parts = [self._up_left]
        got = int(self._up_left.size)
        while got < max_samples:
            try:
                chunk = self.rx_pcm.get_nowait()
            except queue.Empty:
                break
            if len(chunk) % 2:                 # 半个样本不该出现，防御性砍掉
                chunk = chunk[:-1]
            if not chunk:
                continue
            arr = np.frombuffer(chunk, dtype=np.int16)
            parts.append(arr)
            got += int(arr.size)
        if got == 0:
            return np.zeros(0, dtype=np.int16)
        all_pcm = parts[0] if len(parts) == 1 else np.concatenate(parts)
        out = all_pcm[:max_samples].copy()
        self._up_left = all_pcm[max_samples:].copy()
        return out

    def write_pcm(self, pcm: np.ndarray):
        """把 PCM 编成 μ-law 入队；真正的发送由喂料线程按均匀小片做。

        为什么要压缩 + 均匀喂：板子播 16kHz/16bit 要 32KB/s，而 1Mbaud 这条
        链路实测几乎没有余量（1024 字节一片突发只有 37% 能到板子的解析器），
        结果就是播放环反复见底、"一卡一卡"。μ-law 把下行砍到 16KB/s，
        再按 8ms 一拍、128 字节一片均匀发 —— 这两件事都做上才有余量。
        """
        if pcm.size == 0:
            return
        with self._dl_lock:
            self._dl_buf.extend(ulaw.lin2ulaw(pcm).tobytes())
            if len(self._dl_buf) > 65536:      # 板子卡住时的兜底，别无限涨
                del self._dl_buf[:len(self._dl_buf) - 32768]
                if not self._dl_dropped:
                    self._dl_dropped = True
                    log("serial", "下行积压超过 4 秒，丢弃最老的一半（板子在卡？）")

    def flush_downlink(self, max_bytes):
        """把攒下的下行发出去最多 max_bytes（一次一帧）。返回发出的字节数。"""
        with self._dl_lock:
            n = len(self._dl_buf)
            if n == 0:
                return 0
            if n > max_bytes:
                n = max_bytes
            if n > MAX_PAYLOAD:
                n = MAX_PAYLOAD
            chunk = bytes(self._dl_buf[:n])
            del self._dl_buf[:n]
        self.link.send(T_AUDIO_ULAW, chunk)
        return n

    def downlink_pending(self):
        with self._dl_lock:
            return len(self._dl_buf)

    def play_stop(self):
        # 先丢掉还没发出去的音频，再让板子清缓冲 —— 否则"还没发完的旧音频"
        # 会在打断之后继续追上去播，听感是"说了打断它还在响"。
        with self._dl_lock:
            self._dl_buf.clear()
        self.link.send_cmd(CMD_PLAY_STOP)

    def tone(self, kind):
        self.link.send_cmd(CMD_TONE, {"listen": 1, "done": 2, "error": 3}.get(kind, 0))

    def set_amp(self, on):
        self.link.send_cmd(CMD_SET_AMP, 1 if on else 0)

    def set_volume(self, pct, force=False):
        """放音音量 0..100。板子上电时没有设过放音通路音量，停在编解码器默认值
        （实测偏响），所以要由 PC 明确给一个值。

        force=True 时绕过去重。心跳重发必须用它 —— 否则"值没变就不发"会把
        心跳整个吃掉，板子一重启（或那一条命令丢了）音量就永远回不到设定值，
        听感上就是"怎么又变响了"（踩过）。"""
        pct = max(0, min(100, int(pct)))
        if force or pct != self.volume_pct:
            self.volume_pct = pct
            self.link.send_cmd(CMD_SET_VOL, pct)

    def set_time(self, t=None):
        """把 PC 的本地时间发给板子（表盘要用）。

        板子上没有 RTC 电池，`gettimeofday` 开机给的是 1970 或构建时刻，
        所以表盘上的日期时间必然是错的 —— 只能由 PC 校时。
        直接发年月日时分秒、不涉及时区：板子的 TZ 是 UTC，我们把这几个数
        当成 UTC 塞进系统时钟，`localtime` 读回来就正好是这几个数。
        （发 epoch 反而要处理时区，板子那边不一定有 TZ 数据。）"""
        t = t or time.localtime()
        self.link.send(T_CMD, bytes([CMD_SET_TIME,
                                     (t.tm_year - 2000) & 0xFF,
                                     t.tm_mon & 0xFF, t.tm_mday & 0xFF,
                                     t.tm_hour & 0xFF, t.tm_min & 0xFF,
                                     t.tm_sec & 0xFF]))

    def set_mic(self, on, force=False):
        # force=True 时绕过"值没变就不发"的去重。自愈逻辑要用它：
        # 板子单方面哑掉时，网关这边 mic_on 仍然是 True，不去重就永远
        # 不会再发一次开麦命令。
        if force or on != self.mic_on:
            self.mic_on = on
            self.link.send_cmd(CMD_SET_MIC, 1 if on else 0)

    def set_ui_state(self, code):
        """把语音状态告诉板子，界面拿它显示"在听/在想/在说"。"""
        self.link.send_cmd(CMD_UI_STATE, int(code) & 0xFF)

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

    def set_volume(self, pct, force=False):
        pass

    def set_time(self, t=None):
        pass

    def set_mic(self, on, force=False):
        pass

    def set_ui_state(self, code):
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

    def set_volume(self, pct, force=False):
        pass

    def set_time(self, t=None):
        pass

    def set_mic(self, on, force=False):
        pass

    def set_ui_state(self, code):
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

    def set_volume(self, pct, force=False):
        pass

    def set_time(self, t=None):
        pass

    def set_mic(self, on, force=False):
        pass

    def set_ui_state(self, code):
        pass

    def close(self):
        pass


class DuplexAudio:
    """把"输入端点"和"输出端点"拼成一个，两者独立。

    需要它的场景就是 `--source wav --sink board`：拿一段真机录音当麦克风
    喂进去、让板子的喇叭出声。这是可复现的全链路验证方式，也是唯一能在
    没有人在板子旁边时验证"VAD -> 缓冲 -> 归一化 -> 编码 -> ASR -> TTS ->
    喇叭"整条链路的办法。

    读（read_pcm / events）走 src，写（write_pcm / play_stop / tone /
    set_amp / set_mic）走 sink，feed_frames 两边都喂（板子来的帧里既有
    音频也有事件，src 用不到也要给 sink 的链路留着）。
    """

    def __init__(self, src, sink):
        self.src, self.sink = src, sink
        self.name = "%s->%s" % (src.name, sink.name)

    @property
    def events(self):
        return self.src.events

    @property
    def mic_on(self):
        # 电平日志要读它。输入是 wav 时"开麦"没有意义，交给输出侧回答。
        return getattr(self.sink, "mic_on", None)

    def feed_frames(self, frames):
        self.src.feed_frames(frames)
        self.sink.feed_frames(frames)

    def read_pcm(self, max_samples=960):
        return self.src.read_pcm(max_samples)

    def write_pcm(self, pcm):
        self.sink.write_pcm(pcm)

    def flush_downlink(self, max_bytes):
        """转发给输出端点。**必须转发**：喂料线程是通过 self.audio 找这个方法的，
        而 --source wav 时 self.audio 是 DuplexAudio —— 漏了它一片都发不出去。"""
        fl = getattr(self.sink, "flush_downlink", None)
        return fl(max_bytes) if fl else 0

    def downlink_pending(self):
        fn = getattr(self.sink, "downlink_pending", None)
        return fn() if fn else 0

    def play_stop(self):
        self.sink.play_stop()

    def tone(self, kind):
        self.sink.tone(kind)

    def set_amp(self, on):
        self.sink.set_amp(on)

    def set_mic(self, on, force=False):
        self.sink.set_mic(on, force)

    def set_ui_state(self, code):
        self.sink.set_ui_state(code)

    def set_volume(self, pct, force=False):
        self.sink.set_volume(pct, force)

    def set_time(self, t=None):
        self.sink.set_time(t)

    def close(self):
        self.src.close()
        self.sink.close()


# ==========================================================================
# 语音活动检测（网关侧，省带宽 + 决定何时开/关聆听）
# ==========================================================================

class EnergyVad:
    """极简能量 VAD，带**开机自动底噪标定**。

    小智设备用 ESP-SR 做本地唤醒词；我们芯片不同、也不值得为它训模型。
    对"哄睡"这个场景，能量 VAD 足够：安静环境下的说话起始点非常明显。
    需要真唤醒词时再换 openwakeword/sherpa-onnx，接口不变。

    带滞回：起始阈值高（防误触），维持阈值低（防断句）。

    ---------- 为什么要自动标定 ----------
    两个阈值都是**绝对 dBFS**，而绝对电平和三件事绑在一起：麦克风灵敏度、
    采集增益、房间底噪。这三样任何一个变了，写死的阈值就废：
      - 阈值太高 -> 说话够不到，表现成"链路连着但怎么说都没反应"；
      - 阈值太低 -> 底噪一直在上面，表现成"没说话也自己开始听"；
      - keep 低于底噪 -> "静音"判据永不成立，一旦触发就永远收不了尾。
    这三种故障我都踩过，而且表面症状互相像，光看日志分不出来。
    所以开机先量 25 帧（1.5 秒）的底噪，再按它推阈值——换房间、换固件增益
    都不用重新调参。
    """

    CAL_FRAMES = 40           # 2.4 秒 @60ms（要算 90 分位，样本太少没有意义）
    START_OVER_EDGE = 3.0     # 起始阈值：底噪上边缘之上 3dB（再高就够不到人声了）
    KEEP_OVER_MED = 3.0       # 维持阈值：底噪中位之上 3dB（必须高于中位，否则收不了尾）
    ONSET_FRAMES = 2          # 起音要连续 2 帧过阈值（120ms）才算说话

    def __init__(self, start_db=None, keep_db=None, hang_ms=1000):
        # hang_ms 给 1000ms 而不是几百毫秒：说话的句读停顿（"小鸣……讲个故事"）
        # 完全可能到 500ms 以上，hang 太短会把一句话切成几段分别送去识别，
        # 每段都太短、认不出，表现成"说了但没反应"。
        self.hang_frames = max(1, int(hang_ms / FRAME_MS))
        self.start_db = self._f(start_db)
        self.keep_db = self._f(keep_db)
        self.active = False
        self._silent = 0
        self._onset = 0
        self._cal = []
        self.calibrated = bool(start_db is not None and keep_db is not None)
        self.noise_db = None
        self._noise_edge = None

    @staticmethod
    def _f(v):
        return None if v is None else float(v)

    @staticmethod
    def _db(pcm):
        if pcm.size == 0:
            return -120.0
        rms = float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)) + 1e-9)
        return 20.0 * np.log10(rms / 32768.0)

    def calibrate(self, pcm):
        """喂底噪样本。够 CAL_FRAMES 帧就定阈值，返回 True（表示这一帧刚标定完）。

        **数字静音帧要丢掉、不计数。** 实测踩到过：网关卡在开机瞬间标定，
        那时板子的麦克风命令还在路上、上行是**全零**，于是标定出 "-270dB"
        这种不存在的底噪，推出 起始 -57 / 维持 -59 的荒谬阈值 —— 之后任何
        一点噪声都能触发 VAD，屏上会莫名其妙地跳"在听"。
        丢掉静音帧的代价只是标定多花几帧，而麦克风一开马上就够了。
        """
        if self.calibrated:
            return False
        d = self._db(pcm)
        if d < -60.0:
            return False              # 全零/近全零：麦克风还没开，不算底噪
        self._cal.append(d)
        if len(self._cal) < self.CAL_FRAMES:
            return False
        arr = np.array(self._cal)
        # 两个基准分开取，因为它们要回答的是两个不同的问题：
        #   "会不会误触发" -> 看底噪的**最坏情况**，用 90 分位（上边缘）；
        #   "能不能判出静音" -> 看底噪的**典型值**，用 40 分位（中位）。
        # 只用中位会栽在误触发上，只用上边缘会栽在收不了尾上（都踩过）。
        edge = float(np.percentile(arr, 90))
        med = float(np.percentile(arr, 40))
        self.noise_db = med
        self._noise_edge = edge
        edge = min(max(edge, -60.0), -33.0)
        med = min(max(med, -62.0), -36.0)
        self.start_db = min(edge + self.START_OVER_EDGE, -26.0)
        self.keep_db = min(med + self.KEEP_OVER_MED, self.start_db - 1.0)
        self.calibrated = True
        log("vad", "底噪 中位 %.1f / 上边缘 %.1f dB -> 起始 %.1f / 维持 %.1f dB"
            % (self.noise_db, self._noise_edge, self.start_db, self.keep_db))
        if self._noise_edge < -60.0:
            log("vad", "注意：底噪低于 -60dB，麦克风可能没在工作（不是房间安静）")
        elif self._noise_edge > -33.0:
            log("vad", "注意：底噪高于 -33dB，环境偏吵，VAD 会不稳（关风扇/挪位置）")
        return True

    def update(self, pcm):
        """返回 (is_speech, just_started, just_ended)"""
        d = self._db(pcm)
        started = ended = False
        if not self.calibrated:
            self.calibrate(pcm)          # 标定期间一律不算说话，防误触发
            return False, False, False
        if not self.active:
            if d > self.start_db:
                self._onset += 1
                # 起音要连续 2 帧：单帧偶发尖峰（桌子响一下、远处一声咳嗽）
                # 单独一帧就能越过阈值，直接开听的话，会白开一次会话、
                # 服务器那边听不到人声，看起来就像"触发了却没识别"。
                if self._onset >= self.ONSET_FRAMES:
                    self.active = True
                    self._silent = 0
                    started = True
            else:
                self._onset = 0
        else:
            if d > self.keep_db:
                # **漏桶**：过阈值的帧把静音计数减 1，而不是清零。
                # 原来清零的写法在"人声弱、底噪抖"的现场基本不可能收尾 ——
                # 只要偶尔冒出一个底噪尖峰，计数就功亏一篑，一句话能拖到
                # 51 秒才结束（实测），服务器收到的是一段又长又稀的音频，
                # ASR 只能返回空。减 1 既容忍零星尖峰，又不会永远攒不满。
                self._silent = max(0, self._silent - 1)
            else:
                self._silent += 1
                if self._silent >= self.hang_frames:
                    self.active = False
                    ended = True
        return self.active, started, ended

    def reset(self):
        """只清"这一轮"的状态。**标定结果要留着** —— 底噪是房间和固件增益
        决定的，掉一轮聆听就重新标定属于白折腾，而且标定期是听不见话的。"""
        self.active = False
        self._silent = 0
        self._onset = 0


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
        # 上行电平诊断：每 LVL_EVERY_S 秒打一行，用来判断"到底板子没出声，
        # 还是出了声但够不到阈值"。掉麦/阈值过高/turn 死锁，这三种故障
        # 表面上都是"怎么说都没反应"，只有电平日志能一眼分开。
        self._lvl_t = time.time()
        self._lvl_n = 0
        self._lvl_max = -120.0
        self._lvl_peak_all = -120.0
        self._up_dead = 0            # 连续收不到上行样本的帧数（上行哑掉检测）
        self._lvl_lost = 0           # 本窗内"一片都没收到"的帧数
        # 界面语音状态：给屏幕报"在听/在想/在说"。
        # _await_reply 是四态里唯一串口上看不出来的那个 —— 音频已经送出去了、
        # 模型还在算，这段时间没有任何字节在动，只有网关自己知道。
        self._await_reply = False
        self._ui_sent = -1           # 上次发给板子的状态码（-1 = 还没发过）
        self._ui_last = 0.0
        self._await_t = 0.0          # 置 _await_reply 的时刻（超时兜底用）
        self._mic_last = 0.0         # 上次发开麦/关麦的时刻（心跳重发用）
        self._vol_last = 0.0         # 上次发音量的时刻
        self._time_last = 0.0        # 上次校时的时刻
        self._dl_n = 0               # 本窗下行给板子的样本数（吞吐遥测）
        self._dl_frames = 0          # 本窗下行帧数

    # ---------------- 设备侧链路 ----------------

    def start_link(self):
        if self.cfg["source"] != "board" and self.cfg["sink"] != "board":
            return
        port = self.cfg["port"]
        self.link = SerialBoardLink(port, self.cfg["baud"])
        self.link.open()
        log("serial", "已连接 %s @%d (CH343)" % (self.link.port, self.cfg["baud"]))
        threading.Thread(target=self._link_reader, daemon=True).start()
        if self.cfg["sink"] == "board":
            threading.Thread(target=self._dl_pacer, daemon=True).start()

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

    # 下行喂料节拍：每 4ms 最多发 128 字节 = 32KB/s **容量**。
    #
    # 注意这里是"容量"不是"速率"：源（μ-law 后的 16kHz 语音）只有 16KB/s，
    # 所以每拍实际只会发到 ~64 字节，片长自然落在 64~128 字节 —— 正是实测
    # 100% 到达的那种形态。而容量留成 2 倍是有意的：服务器在每轮开头会
    # 连发 8 帧垫底（real_server.py 的 PRIME_FRAMES），容量掐死在 16KB/s
    # 的话，这 480ms 的垫底会被节拍器拖成 480ms 才发完，等于白垫、
    # 缓冲永远建不起来（踩过：待放最高只有 1856 样本）。
    DL_CHUNK, DL_PERIOD = 128, 0.004

    def _dl_pacer(self):
        """下行喂料线程：按固定节拍把 μ-law 小片均匀发给板子。

        **必须是线程、不能是 asyncio 任务**：Windows 上 asyncio 的事件循环
        分辨率不够，`sleep(8ms)` 会被拉长，长期速率追不上下行需求，缓冲一路涨。
        线程 + 自旋补偿才能把长期速率钉在标称值上。

        **绝对时间表 + 自旋补偿**：`time.sleep(8ms)` 在 Windows 上实际睡
        8.5ms 左右，直接用 sleep 当节拍长期只有 15KB/s，低于 16KB/s 的需求，
        缓冲会慢慢涨起来（踩过）。所以先睡到还差 0.5ms，剩下空转补足。
        """
        next_t = time.monotonic()
        while not self.quit and self.link:
            next_t += self.DL_PERIOD
            while True:
                dt = next_t - time.monotonic()
                if dt <= 0.0005:
                    break
                time.sleep(min(dt - 0.0005, 0.004))
            while time.monotonic() < next_t:
                pass
            try:
                fl = getattr(self.audio, "flush_downlink", None)
                if fl is not None:
                    fl(self.DL_CHUNK)
            except Exception as e:
                # 整个循环体都在 try 里：这条线程一死，下行就彻底没有节流，
                # 而表现只是"声音卡"——不记日志根本查不出来。
                log("serial", "下行喂料异常: %s" % e)
                # 兜底里也要防 self.audio 为 None（线程在 start_audio 之前就起了）：
                # 这条线程一死，下行就彻底没有节流，而表现只是"声音卡"，极难查。
                try:
                    if self.audio is not None and self.audio.downlink_pending() > 32768:
                        self.audio.flush_downlink(512)
                except Exception:
                    pass
                next_t = time.monotonic()
                time.sleep(0.02)

    def start_audio(self):
        """选音频端点。**输入和输出必须能各自独立选。**

        原来的写法是 `want_board = "board" in (src, sink)` —— 只要 sink 是
        board 就一律用 BoardAudio，于是 `--source wav --sink board`（拿一段
        录音当麦克风喂进去、让板子喇叭出声）永远走不到 WavAudio 分支。
        这个组合很重要：它是**可复现**的全链路验证方式 —— 同一段音频进去，
        VAD 的起止点、ASR 的文本每次都该一样，断言才有意义；而真人对着
        麦克风喊一轮要十几秒、每次内容还不同，根本没法当回归测试用。
        """
        src, sink = self.cfg["source"], self.cfg["sink"]
        if src == sink:
            self.audio = self._make_endpoint(src)
        else:
            self.audio = DuplexAudio(self._make_endpoint(src),
                                     self._make_endpoint(sink))
        log("audio", "音频端点: source=%s sink=%s" % (src, sink))
        self.codec = vc.OpusCodec(UP_SAMPLE_RATE, 24000, FRAME_MS)

    def _make_endpoint(self, name):
        if name == "board":
            return BoardAudio(self.link)
        if name == "wav":
            a = WavAudio(self.cfg["wav"], loop=self.cfg["wav_loop"])
            log("audio", "wav 输入: %s（%.2fs）"
                % (self.cfg["wav"], a.pcm.size / float(BOARD_RATE)))
            return a
        if name == "pc":
            a = PcAudio()
            log("audio", "PC 声卡已打开（%d Hz）" % BOARD_RATE)
            return a
        return NullAudio()

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
            # 识别结果回来了，"在想"到此结束（接下来要么开播、要么空结果）
            self._await_reply = False
            log("asr", "「%s」" % ev["text"])
        elif t == "tts":
            st = ev["state"]
            if st == "start":
                self._await_reply = False     # 开始播报了，屏上要切到"在说"
                # 只在**真打断**（上一轮还在放）时才清空下行缓冲。
                # 板子侧 bsp_audio_play_clear() 与 DMA 中断之间有竞态：
                # 它分两步把 head/tail 清 0，中断挤在中间会读到
                # head=0/tail=旧值，avail 在无符号减法下回绕成天文数字，
                # 之后 tail 永远跑在 head 前面（待放长期为负），
                # 播放端持续欠载 —— 听感一段一段的。
                # 正常轮次交接时上一轮早放完了，清空既无必要又有风险。
                if self.state == "SPEAKING":
                    self.audio.play_stop()
                self._tts_turn = self.turn
                self.state = "SPEAKING"
                log("tts", "开始朗读")
            elif st == "sentence_start":
                log("tts", "句: %s" % ev["text"])
            elif st == "stop":
                # 关键：被打断的那一轮，服务器仍然会把 tts stop 发完。
                # 如果无条件接受它，就会把刚建立的"新一轮聆听"状态打回 IDLE，
                # 用户下一句话直接丢——表现成"打断之后就再也不理我了"。
                if self.turn == self._tts_turn:
                    self.state = "IDLE"
                    # 这里原本会放一声"嘟"（CMD_TONE done = 1320Hz/90ms）作为
                    # "我说完了"的提示。实测用户反馈很难受 —— 哄睡场景里每轮
                    # 对话末尾来一声电子音，比不说话更打断入睡。去掉。
                    # 板上提示音通道（CMD_TONE）留着，开机/出错时还能用。
                    log("tts", "朗读结束")
                else:
                    log("tts", "忽略上一轮的 tts stop（轮次已作废）")
        elif t == "llm":
            if ev["emotion"]:
                log("llm", "情感: %s" % ev["emotion"])
        elif t == "listen":
            # 服务端要求开始/结束聆听。**原先没有这个分支** —— 一轮回复结束后
            # 状态停在 IDLE，用户下一句话就永远进不来，表现成
            # "只说了第一句，之后再也不理我"。这是常态化对话的必要一环。
            st = (ev.get("raw") or {}).get("state") or ev.get("state")
            if st == "start":
                if self.state != "LISTENING":
                    log("ws", "服务器要求开始聆听")
                    asyncio.create_task(self._begin_listen())
            else:
                if self.state == "LISTENING":
                    self.state = "IDLE"
                log("ws", "服务器要求结束聆听")
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
            self._dl_n += int(pcm.size)
            self._dl_frames += 1
            self.audio.write_pcm(pcm)

    async def _begin_listen(self):
        # 幂等：hello 回包和服务端主动下发的 listen 都会走到这里，两边都发
        # listen_start 会在服务端开出**两个**聆听会话，第二个把第一个顶掉，
        # 音频归属就乱了。已经在听就直接返回。
        if self.state == "LISTENING":
            return
        self.turn += 1
        await self._pending_send(self.proto.listen_start("auto"))
        self.state = "LISTENING"
        self.vad.reset()
        log("state", "聆听中（VAD 自动断句）")

    # 送识别的最短时长：再短多半是噪声毛刺或碰了一下桌子，送过去只会
    # 换来一句"没听清"，还会把上一轮的 TTS 打断，不如直接丢。
    MIN_UTT_SECS = 0.35
    # 整句峰值归一化的目标（0.5 -> 约 -6dBFS）和最大放大倍数。
    # 上限存在的意义：真遇到全静音的一句，别把本底噪声吹成满量程。
    NORM_TARGET_PEAK = 0.5
    NORM_MAX_GAIN = 12.0

    def _push_mic_state(self):
        """开麦/关麦：状态变了就发，另外每秒重发一次。

        为什么要重发（实测踩到过）：这条命令是一帧，混在下行的音频洪流里
        会被冲掉 —— 播报结束后那条 `CMD_SET_MIC 1` 丢失，板子的麦克风就
        **永久静音**下来（`上行 N 帧` 计数停住不动），症状和历史上那个
        "只说一句就再也不理人"一模一样。每秒重发一次的代价是 8 字节。
        """
        want = self.state != "SPEAKING"
        now = time.time()
        if want != getattr(self.audio, "mic_on", None) or now - self._mic_last >= 1.0:
            self.audio.set_mic(want, force=True)
            self._mic_last = now

    def _push_time(self):
        """校时：连上就发一次，之后每 60 秒一次（板子重启也能自动纠回来）。

        为什么不像其它命令那样"变了才发"：板的时钟本身会走，但一旦重启就
        回到 1970；每 60 秒无脑重发的代价只有 8 字节，换来的是"表盘永远准"。
        """
        now = time.time()
        if now - self._time_last < 60.0:
            return
        self._time_last = now
        self.audio.set_time()

    def _push_time(self):
        """校时：连上就发一次，之后每 60 秒一次（板子重启也能自动纠回来）。

        为什么不像别的命令那样"变了才发"：板的时钟自己会走，但一重启就回到
        1970；每 60 秒无脑重发只要 8 字节，换来的是"表盘上的时间永远准"。
        """
        now = time.time()
        if now - self._time_last < 60.0:
            return
        self._time_last = now
        self.audio.set_time()

    def _push_volume(self):
        """音量：设过一次之后每 10 秒重发（同样怕丢；丢了就一直是默认的响度）。"""
        now = time.time()
        if now - self._vol_last < 10.0:
            return
        self._vol_last = now
        self.audio.set_volume(self.cfg["volume"], force=True)

    # 语音状态码（与板侧 CMD_UI_STATE 的取值一一对应，改一处必须改两处）
    UI_IDLE, UI_LISTEN, UI_THINK, UI_SPEAK = 0, 1, 2, 3

    def _ui_state_code(self):
        """当前该在屏幕上显示成什么。

        优先级是有讲究的：
          · 播报中（SPEAKING）最高 —— 那时候喇叭在响，屏上必须说"在说"，
            否则用户会以为它在听，对着它说话、结果被自己的声音顶掉；
          · 其次 VAD 命中（在听）—— 用户正在说话，这是当下最该显示的事实；
            注意这里用 vad.active 而不是 state == LISTENING：后者几乎恒为真
            （服务端会一直续着会话），拿它当判据屏幕会永远显示"在听"，
            等于没有信息；
          · 再其次"已经送出去在等结果"（在想）—— 这是四态里唯一**串口上
            完全看不出来**的一个，只有网关知道，所以必须由网关报。
        """
        # 超时兜底：服务器要是不回 stt/tts，屏幕不能一直卡在"在想"。
        # `_await_t > 0` 这个条件不能省：初值是 0，不判的话置位瞬间就会被
        # 当成"已经超时"清掉，"在想"这个状态永远不会显示出来（自测踩到过）。
        if self._await_reply and self._await_t > 0 and \
                time.time() - self._await_t > 15.0:
            self._await_reply = False
        if self.state == "SPEAKING":
            return self.UI_SPEAK
        if self.vad.active:
            return self.UI_LISTEN
        if self._await_reply:
            return self.UI_THINK
        return self.UI_IDLE

    def _push_ui_state(self):
        """状态变了才发（外加 3 秒一次的兜底重发）。

        为什么要兜底：CMD 帧混在音频流里，链路忙的时候有极小概率丢掉一帧；
        真丢了的话屏幕会**永远停在错的状态**（比如一直显示"在说"），
        而这类故障从日志上完全看不出来。3 秒重发一次的成本是每 3 秒 8 字节。
        """
        code = self._ui_state_code()
        now = time.time()
        if code == self._ui_sent and now - self._ui_last < 3.0:
            return
        if code != self._ui_sent:
            # 只在**变化**时打一行（一轮对话最多四次），既方便验证"屏上显示
            # 的状态真的跟着链路走了"，也不会把 [level] 那些周期日志淹掉。
            # 夹一下再取：这条日志在 _mic_loop 里，一旦 IndexError 就会
            # 打断整个异步循环、把 ws 会话搞崩（一条日志不该有这种权力）。
            log("ui", "屏幕状态 -> %s"
                % ("空闲", "在听", "在想", "在说")[max(0, min(3, code))])
        self._ui_sent = code
        self._ui_last = now
        self.audio.set_ui_state(code)

    @classmethod
    def _norm_peak(cls, pcm):
        """把整句按**峰值**归一到约 -6dBFS。

        为什么值得做：板载麦克风的电平本来就低（说话峰值常在 -36dBFS
        上下），而 ASR 对绝对电平敏感 —— 送一段峰值只有满量程 1.5% 的
        音频过去，识别率会明显掉。按峰值归一化不改信噪比，只是把量化的
        动态范围用满，是 ASR 前处理的常规做法。
        限定最大倍数，是为了避免"没人说话也把噪声拉满"。
        """
        if pcm.size == 0:
            return pcm
        pk = int(np.max(np.abs(pcm)))
        if pk < 32:                                  # 基本是静音，不动
            return pcm
        g = min(cls.NORM_TARGET_PEAK * 32768.0 / pk, cls.NORM_MAX_GAIN)
        if g <= 1.0:
            return pcm
        return np.clip(pcm.astype(np.float32) * g, -32768, 32767).astype(np.int16)

    async def _flush_utterance(self, ws, chunks, peak_db):
        """把攒好的一整句编码送出去，然后收尾聆听会话。"""
        if self.state != "LISTENING":
            return
        pcm = np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.int16)
        secs = pcm.size / float(UP_SAMPLE_RATE)
        if secs < self.MIN_UTT_SECS:
            log("state", "一句话只有 %.2fs，太短，丢弃（峰值 %.1f dB）"
                % (secs, peak_db))
            await ws.send(self.proto.listen_stop())
            return
        lvl_in = self.vad._db(pcm)
        pcm = self._norm_peak(pcm)
        lvl_out = self.vad._db(pcm)
        n = 0
        fs = self.codec.frame_samples
        for i in range(0, pcm.size, fs):
            for f in self.codec.encode(pcm[i:i + fs]):
                await ws.send(f)
                self.stats["up_opus"] += 1
                n += 1
        await ws.send(self.proto.listen_stop())
        # 从这里到服务器回 stt/tts 之间，串口上没有任何字节在动 —— 但用户
        # 正等着，这段时间屏上必须写着"在想"。置位后由 _on_text 在收到
        # stt/tts 时清掉；再加一道超时兜底，免得服务器不回时屏幕一直卡在"在想"。
        self._await_reply = True
        self._await_t = time.time()
        log("state", "一句话结束：%.1fs，峰值 %.1f dB，归一化 %.1f->%.1f dB，送 %d 帧"
            % (secs, peak_db, lvl_in, lvl_out, n))

    _ws_ref = None
    async def _pending_send(self, payload):
        ws = self._ws_ref
        if ws is not None:
            await ws.send(payload)

    async def _mic_loop(self, ws):
        self._ws_ref = ws
        frame_n = UP_SAMPLE_RATE * FRAME_MS // 1000
        # 预卷：VAD 是在"已经越过阈值"之后才判出起音的，从那一刻才开始收
        # 的话，话头（"小鸣"这两字）已经过去了——ASR 收到的是半句话，
        # 常常认不出来。所以平时一直留着最近几帧，开口时先补进去。
        pre = collections.deque(maxlen=5)      # 5×60ms = 300ms
        utt = []                               # 当前这一句的原始 PCM 帧
        utt_peak = -120.0
        _opus0 = self.stats["up_opus"]

        while not self.quit:
            await asyncio.sleep(FRAME_MS / 1000.0)

            # 板子事件（按键 = 用户想说话 / 打断）
            self._drain_events(ws)

            pcm = self.audio.read_pcm(frame_n)
            got = int(pcm.size)          # 真实收到的样本数（补零之前）
            if pcm.size == 0:
                pcm = np.zeros(frame_n, dtype=np.int16)

            # ---- 上行哑掉自愈 ----
            # 现场踩到过：一轮播报结束后板子**完全不再发上行帧**，而网关
            # 这边 mic_on 还是 True —— set_mic 的去重逻辑让它永远不再发
            # 开麦命令，于是就成"只说一句就再也不理人"。
            # 注意不能拿"电平低"当判据：板子静音帧电平也很低，分不开。
            # 只能拿"有没有真实样本"来判 —— read_pcm 返回空才是真哑。
            # 状态必须和板子侧一致：关麦期间本来就没有上行，别误判成故障。
            # 只在**输入来自板子**时才有意义：source=wav/pc 时板子上行本来就
            # 不参与读取，got 恒为 0，会一路误报"上行哑了"并狂发开麦命令。
            if self.cfg["source"] != "board":
                self._up_dead = 0
            elif self.state == "SPEAKING":
                self._up_dead = 0        # 播报中本来就关着麦，不计
            elif got > 0:
                self._up_dead = 0        # 有真实样本 = 上行活着
            else:
                self._up_dead += 1
                if self._up_dead in (25, 50, 100, 200):      # 1.5/3/6/12 秒
                    log("recover", "聆听中收不到上行样本（%.1fs），强制重发开麦"
                        % (self._up_dead * FRAME_MS / 1000.0))
                    self.audio.set_mic(True, force=True)
                elif self._up_dead == 400:
                    log("recover", "重发 4 次仍无上行：板子可能挂了，"
                                   "拔插 USB 或复位后重启网关")

            # ---- 语音状态 → 板子（界面拿它显示"在听/在想/在说"）----
            # 为什么由网关算：板子只能看到串口上有没有音频，分不出是用户在说
            # 还是 AI 在说，更看不到"在想"（那是模型在算，串口上没有任何动静）。
            # 只有 PC 侧同时知道这三件事：VAD、下行播放、以及"已经送出去在等结果"。
            self._push_ui_state()

            # ---- 上行电平诊断（每 2s 一行）----
            # 计的是**真实收到的样本**，不是补零之后的。补零计数会让"板子
            # 一片没发"看起来像"30.7KB/s 一切正常"，我在这上面白绕了一大圈。
            speech, started, ended = self.vad.update(pcm)
            self._lvl_n += got
            cur_db = self.vad._db(pcm)
            if got:
                self._lvl_max = max(self._lvl_max, cur_db)
                self._lvl_peak_all = max(self._lvl_peak_all, cur_db)
            else:
                self._lvl_lost += 1
            _now = time.time()
            # 只要链路一头是板子就值得打这行：下行吞吐跟输入来源无关，
            # 而"下行能不能跟上实时"正是听感卡不卡的关键（source=wav 时
            # 麦克风那几列没意义，但下行那几列照样是有效读数）。
            if _now - self._lvl_t >= 2.0 and ("board" in (self.cfg["source"],
                                                         self.cfg["sink"])):
                _dt = _now - self._lvl_t
                # 标定还没完成时阈值是 None，别用 %.0f 去格式化它（会抛 TypeError
                # 把整个 mic_loop 打断，表现成"日志突然不动了"）。
                _th = ("标定中(%d/%d)" % (len(self.vad._cal), self.vad.CAL_FRAMES)
                       if not self.vad.calibrated
                       else "%.0f/%.0f" % (self.vad.start_db, self.vad.keep_db))
                log("level", "上行 %5.1f KB/s | 丢 %2d 帧 | 送识别 %4.1f 帧/s | "
                             "下行 %5.1f KB/s=%4.2fx 实时(%d帧) 丢旧%d | "
                             "本窗峰值 %6.1f dB | 全局峰值 %6.1f dB | 阈值 %s | 麦%s%s"
                    % (self._lvl_n * 2 / 1000.0 / _dt, self._lvl_lost,
                       (self.stats["up_opus"] - _opus0) / _dt,
                       self._dl_n * 2 / 1000.0 / _dt,
                       (self._dl_n / float(UP_SAMPLE_RATE)) / _dt,
                       self._dl_frames, self.stats["stale_down"],
                       self._lvl_max, self._lvl_peak_all, _th,
                       {True: "开", False: "关", None: "未知"}[getattr(self.audio, "mic_on", None)],
                       "  <-- 上行哑了" if (self._lvl_lost > 4
                                            and self.cfg["source"] == "board") else ""))
                self._lvl_t, self._lvl_n, self._lvl_max = _now, 0, -120.0
                self._lvl_lost = 0
                self._dl_n = 0
                self._dl_frames = 0
                _opus0 = self.stats["up_opus"]

            # ---- 上行：只送"人声那一段"，攒够一整句再发 ----
            #
            # 为什么不实时送：服务器自己也有一层 VAD（IN_DB=-40dB，静音 720ms
            # 触发回复），它会把收到的帧全算进去。实时送的话，一句话前后夹着
            # 几十秒环境噪声，服务器等于拿"11 秒里只有 0.5 秒人声"的一大段
            # 去做识别 —— 实测日志 `触发回复（静音 720ms）：9 帧 / 11.10s
            # -> 没听清（空结果）`。只送人声那一段，识别才有东西可认。
            #
            # 麦只在"播报中"关：1 Mbaud 串口要同时跑上行 32KB/s 和下行 48KB/s，
            # 两个都开会把链路撑满，板子播放缓冲反复见底、声一卡一卡。
            # 其余时候（含 IDLE）必须开着 —— 否则 VAD 收不到音频、
            # 一轮结束后再也回不到 LISTENING，变成"只说一句就不理人"。
            if self.cfg["wake"] == "vad" and self.state != "LISTENING":
                if started:
                    if self.state == "SPEAKING":
                        await self._barge_in(ws)       # 打断：先切状态再继续收
                    await self._begin_listen()

            if started:
                utt.extend(pre)                        # 先把话头（预卷）补上
                pre.clear()
            # 注意 `ended` 那一帧 vad.active 已经是 False，必须单独带上，
            # 否则会走进 else 把刚攒好的一整句清掉 —— flush 只能收到空。
            if self.vad.active or ended:
                utt.append(pcm)
                utt_peak = max(utt_peak, cur_db)
                if ended:
                    await self._flush_utterance(ws, utt, utt_peak)
                    utt, utt_peak = [], -120.0
                    self.state = "IDLE"
            elif self.state != "SPEAKING":
                pre.append(pcm.copy())                 # 留着当下一句的预卷
            self._push_mic_state()
            self._push_volume()
            self._push_time()

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
        # 放音音量 0..100（映射到 -36..+6dB，见 bsp_audio_set_volume_pct）。
        # 板子上电时**没有**设过放音通路音量，停在编解码器默认值，所以由 PC
        # 明确给一个。70 => 约 -7dB：白天/演示够响，又不至于一上来顶满。
        # 夜里哄睡建议 25~40（--volume 25）。
        "volume": 70,
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
              "source", "sink", "wake", "wav", "wav_loop", "volume"):
        v = getattr(args, k, None)
        if v is not None:
            cfg[k] = v
    return cfg


def check_protocol_symbols():
    """启动自检：网关用到的协议常量是不是都真的存在。

    为什么值得专门写一个：`CMD_SET_VOL` 曾经漏在 import 列表外面，而它只在
    `_push_volume()` 里用 —— 那个函数每 10 秒才走一次真正的发送分支，所以
    表现是"**每 10 秒** ws 会话被打断重连一次"，不是启动就报错。我在这上面
    来回查了很久才定位（音量也因此从来没生效过，用户听到的一直是固件默认值）。
    一个漏 import 不该有这种杀伤力：在这里一次性点名，缺了立刻退出。
    """
    need = ["CMD_SET_AMP", "CMD_SET_VOL", "CMD_PLAY_STOP", "CMD_SET_MIC",
            "CMD_TONE", "CMD_UI_STATE", "CMD_SET_TIME", "T_AUDIO_ULAW",
            "MAX_PAYLOAD", "T_AUDIO_UP", "T_AUDIO_DOWN", "T_EVENT", "T_CMD",
            "T_PING", "T_LOG", "EV_KEY_DOWN", "EV_KEY_UP", "EV_BOOT_READY"]
    g = globals()
    missing = [n for n in need if n not in g]
    if missing:
        log("fatal", "协议常量缺失（多半是 board_link 的 import 漏了）：%s"
            % ", ".join(missing))
        return False
    if not callable(globals().get("ulaw")) and "ulaw" not in g:
        log("fatal", "ulaw 模块没导入：下行 μ-law 编码会失败")
        return False
    return True


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
    ap.add_argument("--volume", type=int,
                    help="放音音量 0..100（默认 45）。板子上电时的默认音量偏响，"
                         "哄睡场景建议 35~50")
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

    if not check_protocol_symbols():
        return 4

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
