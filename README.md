# 安眠科技 · 主动式哄睡智能体（代码代号 `mianyu`）

> openvela AI 硬件开发者大赛 2026 · 队伍 485 · 小鸣TV
> 目标板：**SF32LB52-DevKit-LCD**

## 作品定位

`mianyu` 是在 openvela / SF32LB52 上跑的**主动式哄睡智能体**——在入睡后的整晚窗口里，
根据睡眠状态（入睡 / 夜醒 / 深睡 / 浅睡 / 晨起）切换噪声、灯光、衰减策略，
把睡眠事件实时标注为记忆。**全程无任何用户指令**，全靠 cron + 传感器事件驱动。

## 仓库结构

| 路径 | 说明 |
|---|---|
| `app/mianyu/` | 平台无关核心层（7 模块）+ HAL 双后端（PC sim / 真机 vela）+ LVGL 呼吸动画 + 主循环 |
| `board/` | **板级集成层**：音频通路 bring-up + 串口语音链路 + 幂等构建接入脚本（真机运行必需） |
| `tests/` | 核心层 465 项单元测试，PC 直接 `make test` 跑 |
| `demo/night_demo.c` | 单晚压缩演示，`make demo` 跑 |
| `skills/` | 自建 3 个哄睡 Skill（SKILL.md） |
| `docs/` | 真机运行证据、测试记录表、首启检查清单、系统框图与数据流图 |
| `logs/` | AI Coding 协作记录（5 天）+ 板上启动日志原文 |
| `Makefile` | 本地 `make test / app / demo` 入口 |
| `contest2026_485_xiaomingTV.xml` | repo manifest，作品链接到 `packages/demos/contest2026_485_mianyu` |

## 真机编译（openvela 工程内）

```
# 1. 板级集成（幂等，可重复执行）
python board/push_to_buildhost.py    # 把 board/*.c 推到编译主机并接入构建

# 2. 编译
cd /path/to/openvela && source build/envsetup.sh
cd cmake_out/sf32lb52_devkit_lcd && ninja     # 产物 nuttx.bin

# 3. 烧录
sftool.exe -c SF32LB52 -p COM6 -b 1000000 write_flash nuttx.bin@0x12010000
```

manifest 的 `<linkfile>` 会自动把本仓 `app/mianyu/` 软链到
`packages/demos/contest2026_485_mianyu/`，无需手动复制。

## 本地验证（PC 即可跑核心层）

```
mingw32-make test      # 465 项断言全绿
mingw32-make app       # 整晚闭环模拟
mingw32-make demo      # 单晚压缩演示
```

## 真机状态（2026-09-14）

证据全文：[`docs/真机运行证据_20260914.md`](docs/真机运行证据_20260914.md)

| 项 | 结果 |
|---|---|
| 核心层 + HAL + LVGL 入固件 | ✅ `nm` 确证 `mianyu_main` 出自 app 目标文件，固件 +67,252 B |
| app 主循环真机运行 | ✅ 打印作品横幅 + `设备就绪`，UI 线程已起 |
| 串口语音链路 | ✅ 20 s 上行丢帧 0，CRC 错仅开机握手期 1 次 |
| 音频电通路（DAC/ADC） | ✅ 寄存器回读 + DMA 中断在跑 |
| **喇叭声学输出** | ❌ **未通过**：声学回环对照 B/C=0.83，麦克风的 1 kHz 来自板内电耦合；待确认接插件 J0101 |
| 真人整夜标定 | ⏳ 未做（当前准确率数据**仅来自合成信号**，报告中已如实标注） |

## License

Apache-2.0（详见 LICENSE）。
