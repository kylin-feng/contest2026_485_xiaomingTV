#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sound_card.py — 判定"这块板子的喇叭到底有没有发声"。

做法：让板子自己跑声学回环（本地音源放 1kHz，量麦克风频谱），跑两遍：
    · 功放开  —— 声音只能走 DAC -> NS4150B -> 喇叭 -> 空气 -> 麦克风
    · 功放关  —— 只剩码片/PCB 内部 DAC->MIC 的电耦合
两遍一比就分开了：
    A) 关也看得到 1kHz  -> 那根线是电耦合，喇叭没参与（多数情况是喇叭没接/没响）
    B) 只有开才看得到   -> 真声学，喇叭在发声
"量的是板子上的 ADC"，所以不受 USB 时序、环形缓冲欠载这些因素干扰。

顺带把板子的日志行收回来打印，板上自检的原文一并给出。

用法: python sound_card.py [端口]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "voice"))
import board_link as bl  # noqa: E402

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
import serial  # noqa: E402


def run(ser, cmd, arg, wait=4.0, tag=""):
    """发一条命令，等一会儿，返回这段时间里收到的原始文本。

    板子的 vllog 走 syslog（控制台文本），不是 VL_T_LOG 帧 —— 因为本板只有
    一个能通 USB 的串口，日志和二进制帧是混在一条线上的，所以这里直接看
    原始字节流（二进制帧会被当成噪声，不影响用正则找那行文本）。
    """
    ser.reset_input_buffer()
    ser.write(bl.build_frame(bl.T_CMD, bytes([cmd, arg]), 0))
    raw = bytearray()
    t0 = time.time()
    while time.time() - t0 < wait:
        d = ser.read(8192)
        if d:
            raw.extend(d)
    text = raw.decode("utf-8", "replace")
    if tag:
        print("-- %s --" % tag)
        for s in text.splitlines():
            if "vlink" in s:
                print("   " + s.strip())
    return text


def parse_lift(text):
    """从文本里抠出那行回环结果 —— 板上算的数，比 PC 侧算的可靠。

    板子那行长这样：
      [vlink] CMD 声学回环 功放开 窗=12：静音 1kHz=3 邻近=4 | 放音 1kHz=64 邻近=29 | 麦克风 峰值=.. RMS=..
    """
    m = re.compile(r"静音 1kHz=(\d+) 邻近=(\d+) \| 放音 1kHz=(\d+) 邻近=(\d+)"
                   r" \| 麦克风 峰值=(\d+) RMS=(\d+)")
    g = m.search(text)
    if g:
        return tuple(int(x) for x in g.groups())
    return None


def main():
    ser = serial.Serial(port, 1000000, timeout=0.005)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("== 板载声学回环（板子自己放、自己听，板子自己算；每相位 12 窗 ≈ 0.8s）==")
    pairs = []
    for i in range(2):                      # 交替测两轮，抵消慢漂移
        on_txt = run(ser, bl.CMD_LOOPBACK, 1, 5.0, "第%d轮 功放开" % (i + 1))
        on = parse_lift(on_txt)
        off_txt = run(ser, bl.CMD_LOOPBACK, 0, 5.0, "第%d轮 功放关（电耦合对照）" % (i + 1))
        off = parse_lift(off_txt)
        if on and off:
            pairs.append((on, off))
    ser.close()

    if not pairs:
        print("!! 没拿到完整的回环结果，检查板子是否在跑 v11 固件")
        return 1

    print()
    for i, (on, off) in enumerate(pairs):
        o1k, oref, n1k, nref, pk, rms = on
        c1k, cref, d1k, dref, pk2, rms2 = off
        print("   第%d轮 功放开: 静音 1kHz=%-4d 邻近=%-4d | 放音 1kHz=%-4d 邻近=%-4d  [麦克风 峰值=%d RMS=%d]"
              % (i + 1, o1k, oref, n1k, nref, pk, rms))
        print("        功放关: 静音 1kHz=%-4d 邻近=%-4d | 放音 1kHz=%-4d 邻近=%-4d  [麦克风 峰值=%d RMS=%d]"
              % (c1k, cref, d1k, dref, pk2, rms2))

    n_on = sum(p[0][2] for p in pairs) / len(pairs)
    n_off = sum(p[1][2] for p in pairs) / len(pairs)
    ref_on = sum(p[0][3] for p in pairs) / len(pairs)
    ref_off = sum(p[1][3] for p in pairs) / len(pairs)
    print()
    print("   两轮平均 放音窗 1kHz: 功放开 %.1f / 功放关 %.1f（差 %.1f）"
          % (n_on, n_off, n_on - n_off))
    print("   两轮平均 邻近频点:    功放开 %.1f / 功放关 %.1f" % (ref_on, ref_off))
    print("   1kHz / 邻近:  功放开 %.2f  功放关 %.2f"
          % (n_on / (ref_on + 1e-9), n_off / (ref_off + 1e-9)))

    if n_off >= n_on * 0.8:
        print("== 判定：喇叭基本没发声（功放开/关的 1kHz 一样大 -> 那根线是板内电耦合）==")
        print("   先确认 J0101 喇叭连接器上插了喇叭，再重跑。")
        return 1
    if n_on >= 4 * (ref_on + 1e-9):
        print("== 判定：喇叭在发声，声学全链路通 ==")
        print("   DAC -> NS4150B -> 喇叭 -> 空气 -> 麦克风 -> ADC 全部工作。")
        print("   功放关掉后 1kHz 塌到 %.0f（只剩电耦合），说明那根线确实是喇叭挣来的。" % n_off)
        return 0
    print("== 判定：功放明显改变了 1kHz（%.0f -> %.0f），但没甩开邻近频点，测试环境太吵 =="
          % (n_off, n_on))
    print("   让房间安静下来（关掉风扇/别说话）再跑一次。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
