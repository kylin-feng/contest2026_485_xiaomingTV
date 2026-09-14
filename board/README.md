# 板级集成层（SF32LB52-DevKit-LCD）

本目录是**把应用接到真机硬件上的那一层**。`app/mianyu/` 是平台无关的核心
（7 个模块 + HAL 接口 + LVGL UI 壳）；本目录是它落在这块板上的 BSP。

> 真机取证结果与读数见 [`../docs/真机运行证据_20260914.md`](../docs/真机运行证据_20260914.md)。

## 文件

| 文件 | 职责 |
|---|---|
| `bsp_audio_test.c` | 音频通路 bring-up 与自检：AUDCODEC 时钟/REFGEN/DAC/ADC 初始化、DMA 分配、寄存器回读、Goertzel 频谱自检、声学回环对照（`bsp_audio_loopback`）。同时对外提供 `bsp_audio_ready / play_pcm / play_clear / mic_read / set_amp / set_volume_pct` —— 这正是 `app/mianyu/hal/sf32lb52/mianyu_hal_vela.c` 调用的接口 |
| `bsp_voice_link.c` | 串口语音链路：UART1(`/dev/console`) 独占、帧协议 `A5 5A + CRC16-CCITT-FALSE`、开机协议自检、上行 MIC / 下行 PCM / 事件 / 命令分发 |
| `patch_board_build.py` | **在 openvela 工程里做幂等接入**：把两个 .c 加进板级 CMakeLists 的 SRCS、在 `sifli_ap.c` 的 `board_late_initialize()` 里调 `bsp_voice_link_start()`、把 `CONFIG_INIT_ENTRYPOINT` 从 `nsh_main` 改成 `mianyu_main`（三处配置同时改，见下） |
| `push_to_buildhost.py` | 把上面两个 .c 用 base64 推到编译主机并执行 `patch_board_build.py` |

## 硬件通路（调试地图）

```
放音：AUDCODEC DAC1 (A.33) → NS4150B 功放 U0104 → 喇叭 J0101
      └ 功放使能脚 PA10（高有效）
采集：M0100 (WMM7037) → MIC_BIAS → AUDCODEC ADC1/ADC2
      → 数字 ADC_CH0 → DMA1_Channel4（请求号 39）
串口：UART1 = /dev/console（CH343 桥，1 Mbaud），双向 PCM 走这一根线
```

## 复现步骤

```bash
# 1. 推到编译主机并接入构建（幂等，可重复执行）
python board/push_to_buildhost.py

# 2. 编译
cd /root/vela && source build/envsetup.sh
cd cmake_out/sf32lb52_devkit_lcd && ninja     # 产物 nuttx.bin

# 3. 烧录
sftool.exe -c SF32LB52 -p COM6 -b 1000000 write_flash nuttx.bin@0x12010000
```

## 两个必须知道的坑

### 坑一：`CONFIG_INIT_ENTRYPOINT` 要同时改三处

| # | 文件 | 作用 |
|---|---|---|
| 1 | `.../configs/nsh/defconfig` | 配置源头（下次 configure 用） |
| 2 | `cmake_out/<board>/.config` | 当前构建的配置 |
| 3 | `cmake_out/<board>/include/nuttx/config.h` | **编译器真正读的头文件** |

**只改 defconfig 对 `ninja` 增量无效** —— `config.h` 只在 cmake configure 时重生成，
必须直接改 `config.h` 才能让 `nx_bringup.c` 用新符号重编。否则出现"编译过了但行为不对"。

### 坑二：init 入口是 `mianyu_main`，谁都不许再定义同名符号

板级曾经也定义了一个 `int mianyu_main(...)`（想用来"占住 init 线程"）。
它与 `app/mianyu/mianyu_app_main.c` **同名**，链接器解析 init 入口时先撞上板级那个，
于是 app 的整个静态库成员**全部被丢弃** —— 编译日志全绿、固件里没有哄睡主循环。

现在板级函数叫 `vl_product_hold()`（仅作兜底）。核查手法：

```bash
nm nuttx | grep -E " (T|t) (mianyu_main|vl_product_hold)$"
nm .../mianyu_app_main.c.o | grep mianyu_main    # 确证符号出自 app
```

正常情况：只剩 `mianyu_main` 一个，且它出自 app 目标文件；
`vl_product_hold` 被 `--gc-sections` 丢弃。
体积上 app 入固件会让 `nuttx.bin` 增加约 67 KB（1,571,940 → 1,639,192 B）。

## 为什么没有 nsh 控制台

板上只有一个能通到 USB 的串口（UART1 = `/dev/console`），nsh 会在这个口上
**阻塞 read()**，把 PC 发下来的下行 PCM 抢走。实测：

| 指标 | nsh 在线 | nsh 让位后 |
|---|---|---|
| 100 个 PING 回包 | 1 个（1%） | 正常 |
| CRC 错 | 41 | 1（仅开机握手期） |
| 重同步 | 275 | 3 |

因此把 init 入口改成 `mianyu_main`，让语音链路独占串口。
需要 shell 时 PC 发 `CMD 0x10`，链路把控制台交还 nsh（**有意取舍**：交还后下行就不能用了）。
