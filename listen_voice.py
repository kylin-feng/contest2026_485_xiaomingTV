#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""listen_voice.py — 从 PC 侧验证板子真的在按协议发音频。

做三件事：
  1) 打开 CH343 串口（1 Mbaud，必须和 CONFIG_UART_BAUD 一致），
     用 board_link.py 的 FrameParser 解析板子发上来的帧
  2) 统计各类型帧数 / CRC 错 / 重同步，判断链路质量
  3) 把 AUDIO_UP 的 PCM 拼起来存成 wav —— 能直接听板子麦克风采到了什么

用法:
    python listen_voice.py [秒数] [端口] [输出.wav]
"""
import os
import struct
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "voice"))

import board_link as bl  # noqa: E402


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    port = sys.argv[2] if len(sys.argv) > 2 else "COM6"
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, "mic_up.wav")

    import serial

    ser = serial.Serial(port, 1000000, timeout=0.05)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    parser = bl.FrameParser()
    pcm = bytearray()
    counts = {}
    events = []
    logs = []
    seq_seen = []
    t0 = time.time()

    print("== 听 %s @1Mbaud，%.1f 秒 ==" % (port, secs))
    while time.time() - t0 < secs:
        data = ser.read(65536)
        if not data:
            continue
        parser.feed(data)
        for ftype, seq, payload in parser.frames():
            counts[ftype] = counts.get(ftype, 0) + 1
            if ftype == bl.T_AUDIO_UP:
                seq_seen.append(seq)
                pcm += payload
            elif ftype == bl.T_EVENT:
                events.append(payload[0] if payload else -1)
            elif ftype == bl.T_LOG:
                logs.append(payload.decode("utf-8", "replace"))

    ser.close()

    print("-- 帧统计 --")
    for k in sorted(counts):
        print("   %-11s %d 帧" % (bl.T_NAMES.get(k, hex(k)), counts[k]))
    print("   CRC 错 %d / 重同步 %d" % (parser.crc_err, parser.resync))

    if events:
        print("-- 事件 --")
        for e in events[:8]:
            print("   %s" % bl.EV_NAMES.get(e, hex(e)))

    if logs:
        print("-- 板子日志帧 --")
        for s in logs[:8]:
            print("   %s" % s.strip())

    if seq_seen:
        gaps = sum(1 for a, b in zip(seq_seen, seq_seen[1:])
                   if ((b - a) & 0xFF) != 1)
        samples = len(pcm) // 2
        print("-- 上行音频 --")
        print("   %d 样本 = %.2f 秒 @16k，丢帧序 ${%d} 处"
              % (samples, samples / 16000.0, gaps))

        arr = struct.unpack("<%dh" % samples, pcm[:samples * 2])
        peak = max(abs(v) for v in arr) if arr else 0
        nz = sum(1 for v in arr if v != 0)
        dc = sum(arr) / len(arr) if arr else 0
        print("   峰值 %d / 32767，非零 %d/%d，直流偏置 %.1f"
              % (peak, nz, samples, dc))

        with wave.open(out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(bytes(pcm[:samples * 2]))
        print("   已存 %s" % out)

    ok = counts.get(bl.T_AUDIO_UP, 0) > 0 and parser.crc_err == 0
    print("== %s ==" % ("链路验证通过：板子在按协议发真实音频" if ok else "链路有问题，见上"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
