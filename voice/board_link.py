#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
board_link.py — 眠语板（SF32LB52）与网关之间的串口语音链路

为什么走串口而不是 WiFi/BLE：
  实测板上**没有 WiFi**（原理图无 RF 网络、无天线，BOM 无 WiFi 模组），
  只有 BLE（LCPU 上的 H4 栈）。BLE 传 16kHz PCM 要自己搭 GATT 音频流，
  带宽也紧张。而板上现成的 Type-C ①号口就是 CH343 串口（烧录用的那个），
  一根线既能烧录又能跑数据 —— 1.5 Mbaud 下 32KB/s 的单向 PCM 只占约 25%，
  且语音对话本来就基本是半双工（要么在听、要么在说），完全够用。

【帧格式】全部小端
    A5 5A | type | flags | seq | len(u16) | payload(len 字节) | crc16(u16)
    crc16 = CRC16-CCITT-FALSE，覆盖 type/flags/seq/len/payload

  选 2 字节同步头 + crc 的原因：串口链路上板子可能中途复位、USB 可能重枚举，
  上位机必须能自己找回帧边界，不能假设"从第一个字节开始就是帧"。
  seq 用于统计丢帧率（语音链路可以容忍丢帧，但不能不知道丢了多少）。

【帧类型】
    0x01 AUDIO_UP    板→PC   MIC 采到的 PCM（16k/16bit/mono）
    0x02 AUDIO_DOWN   PC→板   送去喇叭播的 PCM（16k/16bit/mono）
    0x03 EVENT        板→PC   事件（按下说话/启动完成…）
    0x04 CMD          PC→板   命令（功放开关/音量/打断/麦克风开关/提示音）
    0x05 PING/PONG    双向    保活
    0x06 LOG          板→PC   调试文本
"""
import struct
import time

SYNC0 = 0xA5
SYNC1 = 0x5A

T_AUDIO_UP = 0x01
T_AUDIO_DOWN = 0x02
T_EVENT = 0x03
T_CMD = 0x04
T_PING = 0x05
T_LOG = 0x06

T_NAMES = {
    T_AUDIO_UP: "AUDIO_UP", T_AUDIO_DOWN: "AUDIO_DOWN", T_EVENT: "EVENT",
    T_CMD: "CMD", T_PING: "PING", T_LOG: "LOG",
}

# 命令码（CMD 帧 payload[0]）
CMD_SET_AMP = 0x01      # payload[1] = 0/1  功放 NS4150B 使能（PA10）
CMD_SET_VOL = 0x02      # payload[1] = 0..100
CMD_PLAY_STOP = 0x03    # 停播 + 清空缓冲（用户打断 AI）
CMD_SET_MIC = 0x04      # payload[1] = 0/1  麦克风上行开关（省带宽）
CMD_TONE = 0x05         # payload[1] = 提示音编号（本地合成，不用下载音频）
CMD_SET_DOWNLINK = 0x06  # payload[1] = 0/1 下行接收开关
CMD_LOOPBACK = 0x07     # payload[1] = 0/1 板上声学回环自检（0 = 功放关，做串扰对照）
# ---- 光引导（附加 2 的下行端）----
# Agent 说了算的是"光怎么带呼吸"：说得慢一点就把呼气拉长，快睡着了就把峰值
# 压下去。板子只把参数落地，节奏算法本身在 app/mianyu/src/mianyu_breathe.c 里。
CMD_SET_BREATHE = 0x08  # [0x08, inhale(2 LE), hold(2 LE), exhale(2 LE), peak(1)] ms / %
CMD_SET_HALO = 0x09     # [0x09, peak%]  只改光晕峰值
CMD_LIGHT_ONOFF = 0x0A  # [0x0A, on]     开/关灯

# 板子产品模式下没有交互控制台（init 入口是 mianyu_main，不是 nsh_main）——
# 否则 nsh 会和语音链路抢同一个串口，把 PC 发下来的 PCM 吃掉（实测下行成功率 1%）。
# 发这个命令会让链路把控制台交还给 nsh；之后下行 PCM 就不可用了。
CMD_SHELL = 0x10

# 事件码（EVENT 帧 payload[0]）
EV_KEY_DOWN = 0x01
EV_KEY_UP = 0x02
EV_BOOT_READY = 0x03
# 入睡判定状态（附加 1 的上行端）：[0x04, state, conf%, bpm]
# state: 0=清醒 1=困倦 2=已入睡。板子只在状态跳变时发，另每 5 分钟补一次心跳。
EV_SLEEP_STATE = 0x04

EV_NAMES = {EV_KEY_DOWN: "KEY_DOWN", EV_KEY_UP: "KEY_UP",
            EV_BOOT_READY: "BOOT_READY", EV_SLEEP_STATE: "SLEEP_STATE"}

SLEEP_STATE_NAMES = {0: "清醒", 1: "困倦", 2: "已入睡"}

MAX_PAYLOAD = 1024          # AUDIO_DOWN 一帧最多 1024B = 512 样本 = 32ms@16k

HEAD_LEN = 5                # type(1) + flags(1) + seq(1) + len(2)
HEAD_FMT = "<BBBH"          # 字段顺序必须与 HEAD_LEN 一致


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF, 不反转, 不异或输出)"""
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else \
                  (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, payload: bytes = b"", seq: int = 0,
                flags: int = 0) -> bytes:
    head = struct.pack(HEAD_FMT, ftype, flags, seq & 0xFF, len(payload))
    body = head + payload
    return bytes([SYNC0, SYNC1]) + body + struct.pack("<H", crc16_ccitt(body))


# ---- 附加 1 / 附加 2 的编解码小 helper ----
# 两边（板级 bsp_voice_link.c / 这里）的字节序必须一致：时长是 2 字节小端。

def build_set_breathe(inhale_ms: int, hold_ms: int, exhale_ms: int,
                      peak_pct: int, seq: int = 0) -> bytes:
    """组一帧 CMD_SET_BREATHE：Agent 调整光引导的呼吸节拍（吸/屏/呼 ms + 峰值%）。"""
    payload = struct.pack("<BHHHB", CMD_SET_BREATHE,
                          inhale_ms & 0xFFFF, hold_ms & 0xFFFF,
                          exhale_ms & 0xFFFF, peak_pct & 0xFF)
    return build_frame(T_CMD, payload, seq)


def build_set_halo(peak_pct: int, seq: int = 0) -> bytes:
    """组一帧 CMD_SET_HALO：只改光晕峰值，不碰节奏。"""
    return build_frame(T_CMD, struct.pack("<BB", CMD_SET_HALO,
                                          peak_pct & 0xFF), seq)


def build_light_onoff(on: bool, seq: int = 0) -> bytes:
    """组一帧 CMD_LIGHT_ONOFF：开/关灯。"""
    return build_frame(T_CMD, struct.pack("<BB", CMD_LIGHT_ONOFF,
                                          1 if on else 0), seq)


def decode_sleep_state(payload: bytes):
    """解 EVENT 帧里的入睡判定状态。

    返回 (state, conf_pct, bpm)；不是 SLEEP_STATE 事件则返回 None。
    用法：`for ftype, seq, pl in p.frames(): r = decode_sleep_state(pl)`
    """
    if len(payload) < 4 or payload[0] != EV_SLEEP_STATE:
        return None
    return payload[1], payload[2], payload[3]


class FrameParser:
    """增量帧解析器：喂多少字节都行，内部自己找边界。

    用法:
        p = FrameParser()
        p.feed(chunk)
        for ftype, seq, payload in p.frames():
            ...

    【关于 feed 的大小 —— 别改成"延迟解析"】
    解析在 feed() 里就做掉了，产出的帧放进 _pending 等 frames() 取走。
    这不是多余的层次，是为了修一个真实踩到的坑：
    老实现只在 frames() 里解析，feed() 里只做"缓冲超过 64KB 就砍掉前面"的自保。
    结果一次 feed 进 64KB（SerialBoardLink.poll 的默认预算就是 64KB）时，
    64KB 里成千上万个还没被解析的真帧，会在任何解析发生之前就被当垃圾砍掉。
    实测：一次喂 6 秒的抓包（159744 字节 / 303 个合法帧）
        分 4KB 喂 -> 302 帧   （对）
        分 64KB 喂 -> 192 帧
        一次喂完   -> 14 帧   （丢掉 95%！）
    这类 bug 不报错、不计数、CRC 全对，只是"声音断断续续"，最难查。
    """

    MAX_BUF = 65536         # 只用于兜住"一直同步不上"的纯垃圾流

    def __init__(self):
        self._buf = bytearray()
        self._pending = []
        self.crc_err = 0
        self.resync = 0

    def feed(self, data: bytes):
        self._buf += data

        # 解析到底（能出多少出多少），并把成品存起来
        self._pending.extend(self._extract())

        # 到这里 _buf 里剩下的才是"真的没解析完"的内容：
        #   · 一个还没收全的帧（保留）
        #   · 或者压根没有同步头的长垃圾（这种情况才需要砍）
        # 所以砍的时候必须从**最后一个同步头**开始留，不能按长度砍。
        if len(self._buf) > self.MAX_BUF:
            i = self._buf.rfind(b"\xA5\x5A")
            self._buf = bytearray(self._buf[i:]) if i >= 0 \
                else bytearray(self._buf[-1:])

    def frames(self):
        """产出上次 feed 之后新解析出的 (ftype, seq, payload)。"""
        out = self._pending
        self._pending = []
        return iter(out)

    def _extract(self):
        """从 _buf 里尽可能多地抠出完整帧，其余的留在 _buf 等下次 feed。"""
        while True:
            # 1) 找同步头
            i = self._buf.find(b"\xA5\x5A")
            if i < 0:
                # 没有同步头；保留最后 1 字节（可能是 A5 的一半）
                if len(self._buf) > 1:
                    self.resync += 1
                    del self._buf[:-1]
                return
            if i > 0:
                self.resync += 1
                del self._buf[:i]

            # 2) 头部够不够 (sync2 + head5 + crc2)
            if len(self._buf) < 2 + HEAD_LEN + 2:
                return
            ftype, flags, seq, ln = struct.unpack_from(HEAD_FMT, self._buf, 2)

            if ln > MAX_PAYLOAD:
                # 长度不合理 -> 同步头是假的，跳过 1 字节重新找
                del self._buf[:1]
                self.resync += 1
                continue

            total = 2 + HEAD_LEN + ln + 2
            if len(self._buf) < total:
                return          # 帧还没收全

            body = bytes(self._buf[2:2 + HEAD_LEN + ln])
            want = struct.unpack_from("<H", self._buf, 2 + HEAD_LEN + ln)[0]
            if crc16_ccitt(body) != want:
                # CRC 错 -> 丢弃这个同步头再找（不整帧丢，避免连带丢后面的好帧）
                del self._buf[:1]
                self.crc_err += 1
                continue

            payload = bytes(self._buf[2 + HEAD_LEN:2 + HEAD_LEN + ln])
            del self._buf[:total]
            yield ftype, seq, payload


# --------------------------------------------------------------------------
# 串口实现（真机用）
# --------------------------------------------------------------------------

class SerialBoardLink:
    """通过 CH343 串口连接眠语板。

    open() 会自动扫端口并按 VID:PID 1A86:55D3（CH343P）优先挑选，
    找不到才退回第一个可用串口 —— 免得用户插了别的串口设备时连错。
    """

    CH343_VIDPID = "1A86:55D3"

    def __init__(self, port: str = None, baud: int = 1500000, timeout: float = 0.02):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None
        self.parser = FrameParser()
        self.tx_seq = 0

    @staticmethod
    def list_ports():
        try:
            import serial.tools.list_ports as lp
            return list(lp.comports())
        except Exception:
            return []

    @classmethod
    def autodetect(cls):
        """返回 (端口名, 描述) 或 (None, None)。优先 CH343。"""
        ports = cls.list_ports()
        for p in ports:
            if cls.CH343_VIDPID.lower() in (p.hwid or "").lower():
                return p.device, p.description
        for p in ports:
            if "CH34" in (p.description or "") or "USB-SERIAL" in (p.description or "").upper():
                return p.device, p.description
        if ports:
            return ports[0].device, ports[0].description
        return None, None

    def open(self):
        import serial
        if self.port is None:
            self.port, desc = self.autodetect()
            if self.port is None:
                raise RuntimeError("没有找到任何串口设备：板子没插好或驱动没起来")
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        self.ser.rts = False
        self.ser.dtr = False
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        return self

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            finally:
                self.ser = None

    # ---- 收 ----

    def poll(self, budget_bytes: int = 65536):
        """非阻塞收：把已到的字节喂给解析器，返回本次新产出的帧列表。"""
        if not self.ser:
            return []
        out = []
        data = self.ser.read(budget_bytes)
        if data:
            self.parser.feed(data)
            for ftype, seq, payload in self.parser.frames():
                out.append((ftype, seq, payload))
        return out

    # ---- 发 ----

    def send(self, ftype: int, payload: bytes = b""):
        if not self.ser:
            raise RuntimeError("串口未打开")
        self.ser.write(build_frame(ftype, payload, self.tx_seq))
        self.tx_seq = (self.tx_seq + 1) & 0xFF

    def send_audio_down(self, pcm: bytes):
        """把 PCM 切成 MAX_PAYLOAD 一片发下去（一片 = 32ms@16k，延迟可接受）。"""
        for i in range(0, len(pcm), MAX_PAYLOAD):
            self.send(T_AUDIO_DOWN, pcm[i:i + MAX_PAYLOAD])

    def send_cmd(self, cmd: int, arg: int = 0):
        self.send(T_CMD, bytes([cmd, arg & 0xFF]))

    def send_ping(self):
        self.send(T_PING, b"")

    def stats(self):
        return {"crc_err": self.parser.crc_err, "resync": self.parser.resync}


if __name__ == "__main__":
    # 自检：帧编解码往返 + 脏数据恢复
    import os
    p = FrameParser()
    frames = [
        build_frame(T_AUDIO_UP, b"\x01\x02\x03\x04"),
        build_frame(T_LOG, "你好，眠语".encode("utf-8"), seq=7),
        build_frame(T_AUDIO_DOWN, os.urandom(600)),
    ]
    stream = b"\x00\xff\x13" + frames[0] + frames[1] + b"\x5a\xa5\x00" + frames[2]
    p.feed(stream)
    got = list(p.frames())
    print("解析到 %d 帧 (期望 3)" % len(got))
    for ft, sq, pl in got:
        print("   %-11s seq=%d len=%d" % (T_NAMES.get(ft, hex(ft)), sq, len(pl)))
    assert len(got) == 3, "往返失败"
    assert got[1][2].decode("utf-8") == "你好，眠语"
    assert got[2][2] == frames[2][2 + HEAD_LEN:-2]
    # 逐字节喂一遍，验证增量解析
    p2 = FrameParser()
    cnt = 0
    for b in range(len(stream)):
        p2.feed(stream[b:b + 1])
        cnt += len(list(p2.frames()))
    print("逐字节喂入解析到 %d 帧 (期望 3)" % cnt)
    assert cnt == 3
    # CRC 破坏 -> 应该被丢弃且不崩
    bad = bytearray(frames[0]); bad[8] ^= 0xFF
    p3 = FrameParser(); p3.feed(bytes(bad) + frames[1])
    ok = list(p3.frames())
    print("坏帧后仍解析到 %d 帧 (期望 1), crc_err=%d" % (len(ok), p3.crc_err))
    assert len(ok) == 1 and p3.crc_err >= 1

    # 【回归】帧数必须与 feed 的分块大小无关
    # 老实现一次喂一大块会把自己的真帧当垃圾砍掉（见 FrameParser 注释里的实测数据）
    big = frames[0] * 400                       # 400 帧连续流 = 208KB
    want = 400
    for chunk in (1, 3, 64, 1000, 4096, 8192, 65536, len(big)):
        q = FrameParser()
        n = 0
        for i in range(0, len(big), chunk):
            q.feed(big[i:i + chunk])
            n += len(list(q.frames()))
        assert n == want, "分块 %d 时只解析出 %d 帧，期望 %d" % (chunk, n, want)
    print("分块无关性: 1/3/64/1000/4096/8192/64K/整块 全部解析出 %d 帧" % want)

    # 逐字节喂入的极端情形（串口最真实的形态）
    q = FrameParser()
    n = 0
    for b in range(len(big)):
        q.feed(big[b:b + 1])
        n += len(list(q.frames()))
    assert n == want
    print("逐字节喂入 208KB 仍解析出 %d 帧" % n)

    print("board_link 自检全部通过")
