# 安眠科技 · 主动式哄睡智能体（代码里叫 mianyu）

openvela AI 硬件开发者大赛 2026，队伍 485，小鸣TV。
目标板：SF32LB52-DevKit-LCD。

## 这是什么

`mianyu` 是跑在 openvela / SF32LB52 上的主动式哄睡智能体。
人睡着之后的整个晚上，它按睡眠状态（入睡 / 夜醒 / 深睡 / 浅睡 / 晨起）
换噪声音色、调灯光、做音量衰减，顺便把每次睡眠事件标成记忆。
**一晚上不需要人说任何指令**，全靠 cron 加传感器事件推着走。

## 仓库里都有什么

| 路径 | 说明 |
|---|---|
| `app/mianyu/` | 平台无关的核心层（7 个模块）+ HAL 两个后端（PC 模拟 / 真机 vela）+ LVGL 呼吸动画 + 主循环 |
| `board/` | 板级集成：音频通路 bring-up、串口语音链路、幂等构建接入脚本（真机跑起来要用） |
| `tests/` | 核心层 465 项单元测试，PC 上 `make test` 直接跑 |
| `demo/night_demo.c` | 单晚压缩演示，`make demo` |
| `skills/` | 自建 3 个哄睡 Skill（SKILL.md） |
| `docs/` | 真机运行证据、测试记录表、首启检查清单、系统框图和数据流图 |
| `logs/` | AI Coding 协作记录（5 天）+ 板上启动日志原文 |
| `Makefile` | 本地 `make test / app / demo` 的入口 |
| `contest2026_485_xiaomingTV.xml` | repo manifest，把作品链到 `packages/demos/contest2026_485_mianyu` |

## 真机怎么编

```
# 1. 板级集成（幂等，可以反复跑）
python board/push_to_buildhost.py    # 把 board/*.c 推到编译主机并接进构建

# 2. 编译
cd /path/to/openvela && source build/envsetup.sh
cd cmake_out/sf32lb52_devkit_lcd && ninja     # 产物 nuttx.bin

# 3. 烧录
sftool.exe -c SF32LB52 -p COM6 -b 1000000 write_flash nuttx.bin@0x12010000
```

manifest 里的 `<linkfile>` 会自动把本仓的 `app/mianyu/` 软链到
`packages/demos/contest2026_485_mianyu/`，不用手动复制。

## 本地怎么验

```
mingw32-make test      # 465 项断言全绿
mingw32-make app       # 整晚闭环模拟
mingw32-make demo      # 单晚压缩演示
```

## 真机现在到哪一步了（2026-09-14）

过程全文：[`docs/真机运行证据_20260914.md`](docs/真机运行证据_20260914.md)

| 项 | 结果 |
|---|---|
| 核心层 + HAL + LVGL 进固件 | 通过。nm 确证 mianyu_main 出自 app 目标文件，固件多了 67252 B |
| app 主循环在板上真跑 | 通过。打印了作品横幅和「设备就绪」，界面线程也起来了 |
| 串口语音链路 | 通过。20 秒上行丢帧 0，CRC 错只有开机握手那次 |
| 音频电通路（DAC/ADC） | 通过。寄存器回读 + DMA 中断在跑 |
| 语音链路整链自测 | 通过。六项全过（不接板子） |
| 板载麦克风录音 | 通过。768 帧 / 12.29 秒，波形有信号 |
| 板子端到端（听） | 通过。板子 → 网关 → 服务器，识别出文本 |
| **喇叭出声** | **没通过**。声学回环 B/C=0.83，麦克风收到的 1 kHz 是板内电耦合。板上喇叭座 J0101 空着，还没插喇叭 |
| 真人整夜标定 | 还没做。现在的准确率数据**只来自合成信号**，报告里已经如实标了 |

## License

Apache-2.0，见 LICENSE。
