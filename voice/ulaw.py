#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""G.711 μ-law 编解码（下行音频压缩用）。

===================== 为什么需要它 =====================

板子播的是 16kHz/16bit PCM，也就是 **32KB/s**。而这条串口链路（CH343 @1Mbaud）
实测几乎没有余量：突发形态下板子只能收到 37% 的帧，一旦写快一点就大面积丢，
听感就是"一卡一卡"。PC 侧再怎么调发送节奏都治不了根 —— 需要的数据量摆在那里。

所以把下行压一半：8bit μ-law，**16KB/s**。

===================== 为什么选 μ-law，不选 ADPCM =====================

ADPCM 能压到 4bit（8KB/s），但要维护解码器状态（预测值 + 步长索引），
它有两条麻烦：
  · 状态耦合：丢一帧，后面所有帧都跟着错（这条链路本来就爱丢帧）；
  · 编解码器要严格镜像，两边差一位就是噪声。
μ-law 是**逐样本查表**：一字节进、一个 16bit 出，没有状态、不传播错误、
板子侧一张 256 项的 int16 表就够了（表还能在开机时算出来，不用写一大坨常量）。
对语音来说 8bit μ-law 的听感约等于 12bit PCM，代价完全可以接受。

板子侧的解码见 board/bsp_voice_link.c 的 vl_on_frame 里 VL_T_AUDIO_ULAW 分支。
**两边必须严格一致**：本文件是权威实现，改这里就要同步改那边，
selftest() 会把往返信噪比打出来，改坏了立刻能看出来。
"""
import numpy as np

BIAS = 0x84
CLIP = 32635
# 掩码从高到低，用来定 exponent（= floor(log2(sample)) - 7，夹到 0..7）
_MASKS = (0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100)


def lin2ulaw(x):
    """int16 数组 -> uint8 μ-law 数组。"""
    xi = np.asarray(x)
    if xi.dtype != np.int16:
        xi = xi.astype(np.int16)
    s = xi.astype(np.int32)
    sign = (s >> 8) & 0x80            # 参考实现就是取 bit8 当符号位
    s = np.abs(s)
    s = np.minimum(s, CLIP) + BIAS
    # exponent：从高位掩码往下扫，**第一个命中**的掩码决定它（前面的都"降级"）。
    # 等价的解析式是 clip(最高有效位 - 7, 0, 7)，但那样要算 log2，这里是纯位运算。
    exp = np.zeros(s.shape, dtype=np.int32)
    found = np.zeros(s.shape, dtype=bool)
    for i, m in enumerate(_MASKS):
        hit = ((s & m) != 0) & (~found)
        exp = np.where(hit, 7 - i, exp)
        found |= hit
    mant = (s >> (exp + 3)) & 0x0F
    return ((~(sign | (exp << 4) | mant)) & 0xFF).astype(np.uint8)


def ulaw2lin(u):
    """uint8 μ-law 数组 -> int16 数组（板子侧要靠它对齐）。"""
    ui = np.asarray(u).astype(np.int32) & 0xFF
    v = (~ui) & 0xFF
    t = ((v & 0x0F) << 3) + BIAS
    t = t << ((v & 0x70) >> 4)
    out = np.where(v & 0x80, BIAS - t, t - BIAS)
    return np.clip(out, -32768, 32767).astype(np.int16)


def selftest():
    """往返信噪比。改坏编码/解码这里立刻会掉。"""
    rng = np.random.default_rng(1)
    # 用语音样的信号：带宽受限的随机噪声 + 包络，比纯随机更接近真实
    n = 16000
    env = 0.3 + 0.7 * np.abs(np.sin(np.linspace(0, 6, n)))
    sig = rng.standard_normal(n) * env
    sig = np.convolve(sig, np.ones(8) / 8, mode="same")
    sig = (sig / np.max(np.abs(sig)) * 12000).astype(np.int16)
    back = ulaw2lin(lin2ulaw(sig))
    err = (sig.astype(np.float64) - back.astype(np.float64))
    snr = 10 * np.log10((sig.astype(np.float64) ** 2).mean() /
                        (err ** 2).mean() + 1e-20)
    print("μ-law 往返：峰值 %d -> 信噪比 %.1f dB（语音 8bit 通常 35~40dB）"
          % (int(np.max(np.abs(sig))), snr))
    # 端点取值自检，防止实现整体偏移
    for v in (-32768, -1, 0, 1, 32767):
        b = int(lin2ulaw(np.array([v], dtype=np.int16))[0])
        print("   %6d -> 0x%02X -> %6d" % (v, b, int(ulaw2lin(np.array([b], np.uint8))[0])))
    return snr


if __name__ == "__main__":
    s = selftest()
    raise SystemExit(0 if s > 30 else 1)
