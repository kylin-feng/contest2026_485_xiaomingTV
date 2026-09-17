#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xiaozhi_proto.py — 小智（xiaozhi-esp32）WebSocket 协议 v1 的 Python 实现

为什么需要它：
  小智的"设备"固件是 ESP32-S3 专用的，我们的板子是 SF32LB52/openvela，两套 SoC，
  固件搬不过来。但小智的**架构**是"瘦客户端 + 服务器"：

      设备：唤醒词 + 采音 + 放音        （薄，几乎不含智能）
      服务器：VAD → ASR → LLM → TTS     （厚，全部智能）

  设备与服务器之间只有一套 WebSocket 协议。所以只要我们的板子＋网关能说这套
  协议，就等价于一台小智设备，直接接进小智生态（官方服务器 / 自建 xiaozhi-esp32-server）。
  这个文件就是那套协议的编码/解码。

协议要点（来自官方文档 https://xiaozhi.me/xz-docs/docs/tutorial-comm/websocket-comm）：
  · 端点      ws://<host>:8989/xiaozhi/v1/
  · 连接头    Authorization: Bearer <token>
              Protocol-Version: 1
              Device-Id: <设备MAC>        （主要身份，重连会顶掉旧连接）
              Client-Id: <UUID>
  · 音频      Opus，上行 16000Hz/mono/60ms，
              下行采样率由服务器 hello 回包决定（常见 24000Hz）
  · 文本帧    JSON；二进制帧 = 裸 Opus（无头）

消息一览（client→server / server→client 都会出现）：
  hello          握手，携带 audio_params
  listen         start / stop / detect        设备侧监听控制
  stt            识别结果（服务端下发的显示文本）
  tts            start / stop / sentence_start 服务端朗读状态
  llm            emotion                       情感（可驱动表情/灯效）
  iot            descriptors / states          设备能力与状态
  mcp            MCP 工具调用
  abort          打断
"""
import json
import uuid

PROTOCOL_VERSION = 1
DEFAULT_WS_PATH = "/xiaozhi/v1/"
DEFAULT_PORT = 8989

# ---- 音频参数（上行） ----
UP_SAMPLE_RATE = 16000
UP_CHANNELS = 1
FRAME_MS = 60
UP_FRAME_SAMPLES = UP_SAMPLE_RATE * FRAME_MS // 1000   # 960
UP_FRAME_BYTES = UP_FRAME_SAMPLES * 2                  # 1920 (16bit PCM)

# ---- 音频参数（下行，服务器 hello 回包会覆盖） ----
DOWN_SAMPLE_RATE = 24000


class XiaozhiProto:
    """把协议字段拼成 JSON / 从 JSON 还原，不做任何 I/O。

    之所以单独抽一层：这样它可以脱离网络和音频设备被单测（见 --selftest）。
    """

    def __init__(self, device_id: str, client_id: str = None, token: str = ""):
        self.device_id = device_id
        self.client_id = client_id or str(uuid.uuid4())
        self.token = token
        self.session_id = ""          # websocket 协议下服务器不返回 session_id
        self.down_sample_rate = DOWN_SAMPLE_RATE

    # ---------------- 连接 ----------------

    def headers(self) -> dict:
        h = {
            "Protocol-Version": str(PROTOCOL_VERSION),
            "Device-Id": self.device_id,
            "Client-Id": self.client_id,
        }
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        return h

    # ---------------- 上行 ----------------

    def hello(self) -> str:
        return json.dumps({
            "type": "hello",
            "version": PROTOCOL_VERSION,
            "transport": "websocket",
            "audio_params": {
                "format": "opus",
                "sample_rate": UP_SAMPLE_RATE,
                "channels": UP_CHANNELS,
                "frame_duration": FRAME_MS,
            },
            "features": {"mcp": False},
        }, ensure_ascii=False)

    def listen_start(self, mode: str = "auto") -> str:
        """mode: auto（服务器 VAD 自动断句）/ manual（手动 stop）/ realtime（持续）"""
        return json.dumps({
            "session_id": self.session_id,
            "type": "listen",
            "state": "start",
            "mode": mode,
        }, ensure_ascii=False)

    def listen_stop(self) -> str:
        return json.dumps({
            "session_id": self.session_id,
            "type": "listen",
            "state": "stop",
        }, ensure_ascii=False)

    def listen_detect(self, wake_word: str) -> str:
        """本地唤醒词命中后告知服务器：让服务器的 ASR 直接以唤醒词后的内容开始。"""
        return json.dumps({
            "session_id": self.session_id,
            "type": "listen",
            "state": "detect",
            "text": wake_word,
        }, ensure_ascii=False)

    def abort(self, reason: str = "wake_word_detected") -> str:
        return json.dumps({
            "session_id": self.session_id,
            "type": "abort",
            "reason": reason,
        }, ensure_ascii=False)

    def text(self, content: str) -> str:
        """直接喂文本给 LLM（跳过 ASR）。调试时很有用：不用对着麦克风说话。"""
        return json.dumps({
            "session_id": self.session_id,
            "type": "text",
            "text": content,
        }, ensure_ascii=False)

    def iot_states(self, states: dict) -> str:
        return json.dumps({
            "session_id": self.session_id,
            "type": "iot",
            "states": states,
        }, ensure_ascii=False)

    # ---------------- 下行解析 ----------------

    def parse(self, raw: str) -> dict:
        """把服务端文本帧解析成规范化的事件 dict。永不抛异常——畸形包返回
        {'type':'unknown'}，上层据此忽略即可（小智官方固件也是这个态度）。

        返回的 dict 一定带 'type' 键，可能还有：
          hello   -> down_sample_rate
          stt     -> text
          tts     -> state, text
          llm     -> emotion
          iot     -> payload
        """
        try:
            msg = json.loads(raw)
        except Exception:
            return {"type": "unknown", "raw": raw}

        if not isinstance(msg, dict) or "type" not in msg:
            return {"type": "unknown", "raw": raw}

        t = msg.get("type")

        if t == "hello":
            ap = msg.get("audio_params") or {}
            sr = ap.get("sample_rate")
            if isinstance(sr, int) and sr > 0:
                self.down_sample_rate = sr
            return {"type": "hello", "down_sample_rate": self.down_sample_rate,
                    "raw": msg}

        if t == "stt":
            return {"type": "stt", "text": msg.get("text", ""), "raw": msg}

        if t == "tts":
            return {"type": "tts", "state": msg.get("state", ""),
                    "text": msg.get("text", ""), "raw": msg}

        if t == "llm":
            return {"type": "llm", "emotion": msg.get("emotion", ""), "raw": msg}

        if t == "iot":
            return {"type": "iot", "payload": msg, "raw": msg}

        if t == "mcp":
            return {"type": "mcp", "payload": msg, "raw": msg}

        if t == "server":
            return {"type": "server", "payload": msg, "raw": msg}

        return {"type": t, "payload": msg, "raw": msg}
