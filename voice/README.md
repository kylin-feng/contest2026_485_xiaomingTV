# voice/ —— PC 侧语音网关与链路自测

板子没有 WiFi（只有 BLE 5.3），所以语音链路是：

```
MEMS MIC ──► 板子(openvela) ──串口 1Mbps──► PC 网关 ──WebSocket──► Agent(LLM)
               ▲                             │                        │
               └──── DAC + Class-D 功放 ◄──── TTS ◄────────────────────┘
```

一根串口线同时干两件事：烧录和说话。这个目录就是上图的 **PC 那一侧**，
板级对端是 `board/bsp_voice_link.c`（帧格式 `A5 5A + CRC16-CCITT-FALSE`）。

## 文件

| 文件 | 干什么 |
|---|---|
| `board_link.py` | 串口帧通道，板级 `board/bsp_voice_link.c` 的 PC 对端（`T_AUDIO_UP` / `T_AUDIO_DOWN` / `T_EVENT` / `T_CMD` / `T_PING` / `T_LOG`） |
| `xiaozhi_proto.py` | 小智（xiaozhi-esp32）那套 WebSocket 协议：连接头 / 握手 / 音频帧 / JSON 消息 |
| `voice_codec.py` | 上行 Opus 编码、下行解码（16 kHz 单声道） |
| `ulaw.py` | **下行 G.711 μ-law 编解码**。权威实现 —— 板级 `bsp_voice_link.c` 里的解码表必须与它严格一致。用它是为了把下行从原样 PCM 的 32KB/s 砍到 16KB/s：这条串口链路实测没有余量，32KB/s 会让板子的播放环每次半缓冲中断都被抽干、补静音，听感就是「一卡一卡」 |
| `gateway.py` | 主网关：串口 ↔ WebSocket，含 VAD 断句与 ASR / TTS / LLM 事件打印 |
| `mock_xiaozhi_server.py` | 假服务器，不接真实后端也能验通整条链 |
| `real_server.py` | 真后端：阿里百炼 ASR(`qwen3-asr-flash`) + DeepSeek LLM + 百炼 TTS(`qwen-tts`)。下行每轮先连发 15 帧（900ms）垫底再按 1.0x 供料，见 `PRIME_FRAMES` —— 板子播放环只有 1.024 秒，不垫底的话环会被抽干 |
| `selftest_e2e.py` | 端到端自测（**不接板子**）：握手 / 上行 / VAD / TTS / 下行 / 情感，六项；它自己起假服务器，不用先手动开 |

仓根另有两个配套脚本。它们 `sys.path.insert` 的是**同级** `voice/`，所以放在仓根：

- `listen_voice.py [秒数] [串口] [输出.wav]` —— 抓板载麦克风录音（默认 8 秒 / COM6 / `mic_up.wav`），
  顺带报 CRC 错、重同步和丢帧
- `sound_card.py [串口]` —— 声学回环判定：板子放本地 1 kHz，量麦克风收到多少，
  功放开 / 关各跑一遍取比值 B/C。**C 组也看得到 1 kHz 就说明是板内电耦合，喇叭没参与**

## 依赖

```bash
pip install pyserial numpy        # 必需
pip install websockets            # gateway / mock 服务器 / selftest 需要
pip install sounddevice pyogg     # 可选：出声后端 / Opus 的 pyogg 后端（否则走系统 libopus）
```

`pyserial` 和 `numpy` 之外都是可选后端，缺了会在真正用到时报错，不影响 `--help`。

## 不接板子先验一遍

```bash
python voice/selftest_e2e.py          # 自己起假服务器，跑完六项
```

## 接板子

```bash
# 0) 本机若配了代理，跑本地 ws 前先摘掉，否则 502
export NO_PROXY=localhost,127.0.0.1
unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy

# 1) 假服务器
python voice/mock_xiaozhi_server.py --port 8990 &

# 2) 网关（串口 <-> websocket）
python voice/gateway.py --source board --sink null --port COM6 --baud 1000000 \
    --server ws://127.0.0.1:8990/xiaozhi/v1/ --wake vad

# 3) 单独抓一段板子的麦克风
python listen_voice.py 12 COM6 mic_up2.wav
```

串口波特率固定 **1000000**，要和固件的 `CONFIG_UART_BAUD` 一致。

## 两个已知坑

- **链路一建立就以 1 Mbaud 满速上行 MIC 帧**（实测 130 秒 18 MB）。USB 串口吃不下这个量，
  板 → PC 方向的文本日志会被二进制噪声截断丢掉 —— `[watch_ui]`、`[ft6146]` 那些行
  不是没打印，是打了被撞碎。想安静看界面日志，先 `board/patch_vlink_quiet.sh off`。
- **下行 PCM 会被 nsh 抢**：nsh 在同一个口上阻塞 `read()`。所以固件把
  `CONFIG_INIT_ENTRYPOINT` 从 `nsh_main` 换成了 `mianyu_main`，让链路独占串口；
  需要 shell 时由 PC 发 `CMD 0x10` 把控制台交还 nsh（交还后下行就不可用了，是有意取舍）。
