#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
voice_codec.py — 网关侧的音频编解码：Opus + 重采样

小智协议规定音频是 **Opus / 16kHz / mono / 60ms 帧**（上行）。
我们的板子是裸 PCM（音频框架给的就是 16k/16bit），所以网关要负责转换：

  上行：板子 PCM(16k)  ──Opus 编码──▶  服务器
  下行：服务器 Opus(24k) ──Opus 解码──▶ 重采样 24k→16k ──▶ 板子 PCM(16k)

下行采样率不能写死：以服务器 hello 回包里的 audio_params.sample_rate 为准
（官方文档示例就是 24000），所以这里两个方向都做成可配的。

Opus 编解码走 pyogg（自带 libopus 动态库，Windows 上不用另外装 DLL）。
"""
import ctypes
import os

import numpy as np

# ---------------------------------------------------------------------------
# libopus 绑定
#
# 为什么不用 pyogg 的高层类：这个版本的 pyogg 只暴露 OpusFile / OpusFileStream
# （解码 ogg 文件用），没有裸的 OpusEncoder/OpusDecoder。但它**自带了
# opus.dll/so/dylib**，所以直接 ctypes 调 libopus 反而更直接、行为更可控，
# 也免去用户额外装 opuslib（那个还要自己配 libopus 路径）。
# ---------------------------------------------------------------------------

OPUS_APPLICATION_VOIP = 2048
OPUS_OK = 0
_opus = None
_opus_path = None


def _find_libopus():
    """按优先级找 libopus：pyogg 自带的 -> 系统路径。"""
    candidates = []
    try:
        import pyogg
        d = os.path.dirname(pyogg.__file__)
        candidates += [os.path.join(d, "opus.dll"), os.path.join(d, "libopus.so"),
                       os.path.join(d, "libopus.dylib")]
    except Exception:
        pass
    candidates += ["opus.dll", "libopus.so.0", "libopus.so", "libopus.dylib"]
    for c in candidates:
        try:
            if os.path.isabs(c) and not os.path.exists(c):
                continue
            return ctypes.CDLL(c), c
        except OSError:
            continue
    return None, None


def load_opus():
    global _opus, _opus_path
    if _opus is not None:
        return _opus
    lib, path = _find_libopus()
    if lib is None:
        raise RuntimeError(
            "找不到 libopus（opus.dll / libopus.so）。\n"
            "  pip install pyogg   # 自带 opus.dll，最省事\n"
            "  或 Linux: apt install libopus0")
    _opus = lib
    _opus_path = path

    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int,
                                        ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encoder_destroy.restype = None
    lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
    lib.opus_encode.restype = ctypes.c_int
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16),
                                ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte),
                                ctypes.c_int]
    lib.opus_encoder_ctl.restype = ctypes.c_int
    lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int,
                                        ctypes.POINTER(ctypes.c_int)]
    lib.opus_decoder_destroy.restype = None
    lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
    lib.opus_decode.restype = ctypes.c_int
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte),
                                ctypes.c_int, ctypes.POINTER(ctypes.c_int16),
                                ctypes.c_int, ctypes.c_int]
    lib.opus_strerror.restype = ctypes.c_char_p
    lib.opus_strerror.argtypes = [ctypes.c_int]
    return lib


def have_opus() -> bool:
    try:
        load_opus()
        return True
    except Exception:
        return False


def resample(pcm: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    """int16 单声道重采样。

    语音场景不需要发烧级插值，但直接抽点会产生明显混叠（TTS 的齿音会变沙）。
    这里先做一次线性插值，再靠回声消除不了的那点误差就交给 Opus/ASR 去容忍。
    对 24k→16k（2/3 下采样）和 16k→24k（3/2 上采样）这两条本项目唯一的路径，
    线性插值的误差已经远小于麦克风本身的信噪比。
    """
    if src_rate == dst_rate or pcm.size == 0:
        return pcm.astype(np.int16, copy=False)

    n_out = int(round(pcm.size * dst_rate / float(src_rate)))
    if n_out <= 0:
        return np.zeros(0, dtype=np.int16)

    # 用浮点做映射，避免整数除法累积相位误差
    x_src = np.linspace(0.0, pcm.size - 1, num=pcm.size, dtype=np.float64)
    x_dst = np.linspace(0.0, pcm.size - 1, num=n_out, dtype=np.float64)
    y = np.interp(x_dst, x_src, pcm.astype(np.float64))
    return np.clip(np.round(y), -32768, 32767).astype(np.int16)


class OpusCodec:
    """Opus 编/解码器。上下行采样率可以不同（小智就是 16k 上、24k 下）。"""

    MAX_PACKET = 1500

    def __init__(self, up_rate: int = 16000, down_rate: int = 24000,
                 frame_ms: int = 60, bitrate: int = 24000):
        self.lib = load_opus()
        self.up_rate = up_rate
        self.down_rate = down_rate
        self.frame_ms = frame_ms
        self.frame_samples = up_rate * frame_ms // 1000      # 960 @16k/60ms

        # 24kbps 对 16k 单声道语音很充裕（ASR 只需要可懂度）
        self.bitrate = int(bitrate)
        self.enc = self._new_encoder(up_rate)
        self._enc_cache = {up_rate: self.enc}     # 下行的 24k 编码器按需建

        self.dec = self._new_decoder(down_rate)

    def _strerr(self, code):
        try:
            return self.lib.opus_strerror(code).decode()
        except Exception:
            return "code %d" % code

    def _new_encoder(self, rate):
        """opus 的编码器是**绑定采样率**的，不能一个实例编两种速率。

        上行走 16k、下行 TTS 是 24k，所以这里按速率缓存实例。
        """
        err = ctypes.c_int(0)
        e = self.lib.opus_encoder_create(
            rate, 1, OPUS_APPLICATION_VOIP, ctypes.byref(err))
        if err.value != OPUS_OK or not e:
            raise RuntimeError("opus_encoder_create(%d) 失败: %s"
                               % (rate, self._strerr(err.value)))
        self.lib.opus_encoder_ctl(e, 4002, self.bitrate)   # SET_BITRATE
        return e

    def _new_decoder(self, rate):
        err = ctypes.c_int(0)
        d = self.lib.opus_decoder_create(rate, 1, ctypes.byref(err))
        if err.value != OPUS_OK or not d:
            raise RuntimeError("opus_decoder_create 失败: %s"
                               % self._strerr(err.value))
        return d

    # ---------- 上行：PCM -> Opus ----------

    def _encode_with(self, enc, pcm16, frame_samples):
        out = []
        n = frame_samples
        buf = ctypes.c_ubyte * self.MAX_PACKET
        pcm16 = np.ascontiguousarray(pcm16, dtype=np.int16)
        for i in range(0, pcm16.size, n):
            chunk = pcm16[i:i + n]
            if chunk.size < n:
                chunk = np.concatenate(
                    [chunk, np.zeros(n - chunk.size, dtype=np.int16)])
            src = chunk.ctypes.data_as(ctypes.POINTER(ctypes.c_int16))
            dst = buf()
            got = self.lib.opus_encode(enc, src, n, dst, self.MAX_PACKET)
            if got < 0:
                raise RuntimeError("opus_encode 失败: %s" % self._strerr(got))
            out.append(bytes(bytearray(dst[:got])))
        return out

    def encode(self, pcm16: np.ndarray):
        """按 60ms 一帧编码（上行 16k）。返回 Opus 帧列表（bytes）。

        最后一帧不足 60ms 会被**补零**到整帧。理由：Opus 解码端按固定帧长解，
        发一个短帧会让服务器 VAD 和 ASR 的时序错乱，而补几十毫秒静音代价极小。
        """
        return self._encode_with(self.enc, pcm16, self.frame_samples)

    def encode_as(self, pcm16: np.ndarray, rate: int):
        """按指定采样率编码（下行 TTS 用：24k）。

        为什么需要它：mock 服务器 / 本地回环要**合成** TTS 音频发给设备，
        而合成出来的是 24k PCM，不能塞进 16k 的编码器（帧长、采样率都不对，
        解出来是变调的鬼叫）。真实小智服务器下发的是已经编好的 24k Opus，
        走不到这里；这个方法只服务于"自己造音频"的测试路径。
        """
        enc = self._enc_cache.get(rate)
        if enc is None:
            enc = self._new_encoder(rate)
            self._enc_cache[rate] = enc
        return self._encode_with(enc, pcm16, rate * self.frame_ms // 1000)

    # ---------- 下行：Opus -> PCM ----------

    def decode(self, opus_frame: bytes, want_rate: int = 16000) -> np.ndarray:
        # 服务器一帧最多对应 120ms，留足空间（120ms@48k = 5760 样本）
        maxn = max(self.down_rate * 120 // 1000, 5760)
        pcm = (ctypes.c_int16 * maxn)()
        src = (ctypes.c_ubyte * len(opus_frame)).from_buffer_copy(opus_frame)
        got = self.lib.opus_decode(self.dec, src, len(opus_frame), pcm, maxn, 0)
        if got < 0:
            raise RuntimeError("opus_decode 失败: %s" % self._strerr(got))
        # 注意：这里必须用 frombuffer(count=) 而不是 np.ctypeslib.as_array(pcm,
        # shape=(got,))——传入 ctypes **数组**（而非 POINTER）时 as_array 会忽略
        # shape，直接按底层数组长度返回整个缓冲区，于是每次解码都"多出"一截
        # 尾部静音。用 count 明确截断才是对的。
        arr = np.frombuffer(pcm, dtype=np.int16, count=got).copy()
        if self.down_rate != want_rate:
            arr = resample(arr, self.down_rate, want_rate)
        return arr

    def set_down_rate(self, rate: int):
        """服务器 hello 回包改了采样率后重建解码器。"""
        if rate == self.down_rate:
            return
        if self.dec:
            self.lib.opus_decoder_destroy(self.dec)
        self.down_rate = rate
        self.dec = self._new_decoder(rate)

    def close(self):
        for r, e in list(getattr(self, "_enc_cache", {}).items()):
            if e:
                self.lib.opus_encoder_destroy(e)
        self._enc_cache = {}
        self.enc = None
        if getattr(self, "dec", None):
            self.lib.opus_decoder_destroy(self.dec)
            self.dec = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


# --------------------------------------------------------------------------
# 本地提示音：不占用下行带宽，也让设备"有反应"更快
# --------------------------------------------------------------------------

def tone(kind: str, rate: int = 16000) -> np.ndarray:
    """生成一段短提示音 PCM。

    kind: "listen"（升调，表示"我在听"）、"done"（降调，表示"知道了"）、
          "error"（低双音）
    本地生成而不是从服务器下载：延迟从"一次网络往返"降到 0，而且断网也能用。
    """
    def _seg(freq, ms, amp=0.25):
        n = int(rate * ms / 1000)
        t = np.arange(n, dtype=np.float64) / rate
        w = np.sin(2 * np.pi * freq * t)
        # 两端各 5ms 淡入淡出，避免"啪"的爆音
        k = max(1, int(rate * 0.005))
        env = np.ones(n)
        env[:k] = np.linspace(0, 1, k)
        env[-k:] = np.linspace(1, 0, k)
        return (w * env * amp * 32767).astype(np.int16)

    if kind == "listen":
        return np.concatenate([_seg(880, 70), _seg(1320, 90)])
    if kind == "done":
        return np.concatenate([_seg(1320, 70), _seg(880, 90)])
    if kind == "error":
        return np.concatenate([_seg(300, 120), _seg(300, 120)])
    return np.zeros(0, dtype=np.int16)


if __name__ == "__main__":
    print("pyogg/opus 可用:", have_opus())

    # 重采样自检
    a = (np.sin(np.linspace(0, 20 * np.pi, 16000)) * 20000).astype(np.int16)
    b = resample(a, 16000, 24000)
    c = resample(b, 24000, 16000)
    print("重采样 16k->24k->16k: %d -> %d -> %d 样本" % (a.size, b.size, c.size))
    assert abs(b.size - 24000) <= 1 and abs(c.size - 16000) <= 1

    if have_opus():
        codec = OpusCodec(16000, 24000, 60)
        pcm = tone("listen", 16000)
        pcm = np.concatenate([pcm, np.zeros(960 * 3 - pcm.size, dtype=np.int16)])
        frames = codec.encode(pcm)
        print("Opus 编码: %d 样本 -> %d 帧, 字节数 %s"
              % (pcm.size, len(frames), [len(f) for f in frames]))
        assert len(frames) == 3
        back = codec.decode(frames[0], 16000)
        print("Opus 解码一帧: %d 字节 -> %d 样本 @16k (期望 960)" % (len(frames[0]), back.size))
        assert 900 < back.size < 1000

        # 静音帧也要能编能解（VAD 靠它判断"说完了"）
        sil = np.zeros(960, dtype=np.int16)
        sf = codec.encode(sil)
        sb = codec.decode(sf[0], 16000)
        print("静音帧: %d 字节 -> %d 样本" % (len(sf[0]), sb.size))

        # 下行 24k：TTS 合成音必须走 24k 编码器，否则解出来会变调
        m = (np.sin(np.linspace(0, 40 * np.pi, 24000)) * 12000).astype(np.int16)
        df = codec.encode_as(m, 24000)
        back24 = codec.decode(df[0], 24000)
        print("下行 24k: %d 样本 -> %d 帧, 首帧解出 %d 样本 @24k (期望 1440)"
              % (m.size, len(df), back24.size))
        assert 1400 < back24.size < 1500, back24.size

    print("voice_codec 自检通过")
