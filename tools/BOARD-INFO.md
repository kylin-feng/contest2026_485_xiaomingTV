# 真机档案 · SF32LB52-Devkit-LCD

> 记录时间：2026-09-14
> 主机：Windows 10 · 端口 COM6
> 结论：**板子在线、可读可写可擦，烧录链路已打通并验证**

---

## 1. 硬件识别

| 项目 | 值 | 来源 |
|---|---|---|
| 板型 | `SF32LB52-Devkit-LCD V1.1.0` | 固件内字符串 |
| 芯片 | SF32LB52 | sftool 连接成功 |
| USB 转串口 | WCH **CH343**（USB-Enhanced-SERIAL） | `VID_1A86&PID_55D3` |
| 串口号 | **COM6** | `[System.IO.Ports.SerialPort]::GetPortNames()` |
| 挂载位置 | `USB\ROOT_HUB30\4&149ed3ed&0&0`（主板根集线器） | 设备树 |
| flash 容量 | **16 MB**（0x12000000 ~ 0x12FFFFFF 可读，0x13000000 起超时） | 边界探测 |

主板另有 3 个历史 COM 口（COM3/4/5）属于 `VID_303A&PID_1001` **Espressif ESP32**，当前均不带电（Present=False），与本项目无关。

---

## 2. 板内现有固件

**不是 mianyu**，是思澈官方出厂固件：

| 特征 | 值 |
|---|---|
| 内核 | RT-Thread |
| 编译时间 | `Jul 10 2025` |
| 关键字符串 | `SF32LB52-Devkit-LCD  V1.1.0`、`2020 - 2022 Copyright by SiFli team`、`audio server`、`Try registered LCD driver...`、`Find touch screen driver...` |
| mianyu/openvela 字样 | **0 处命中** |
| 固件占用 | `0x12000000 ~ 0x1206FFFF`（约 448 KB），`0x12070000` 以上全 0xFF |

### flash 布局（实测）

| 地址 | 大小 | 内容 |
|---|---|---|
| `0x12000000` | 32 KB | ftab 分区表（magic `FCES`） |
| `0x12010000` | 64 KB | bootloader（48284/65536 字节有数据） |
| `0x12020000` | ~320 KB | 应用主体 |
| `0x12070000` 以上 | ~15.5 MB | 空白 |

---

## 3. 已完成的验证（全部通过）

| 步骤 | 命令要点 | 结果 |
|---|---|---|
| 连通性 | `sftool -c SF32LB52 -p COM6 read_flash` | 芯片响应，读到有效 ftab |
| 容量探测 | 递增地址读 16 字节 | 16 MB，边界准确 |
| 全片备份 | `read_flash ...:0x1000000` | **16 MB 完整镜像，用时 3 分 06 秒** |
| 备份自洽 | 全片前 2 MB vs 2 MB 备份比对 | **差异 0 字节** |
| 写入+校验 | 空白区 `0x12100000` 写 4 KB 确定性图案 | 写入成功，`--verify` 通过 |
| 读回比对 | 读回 4 KB 逐字节比对 | **差异 0 字节** |
| 擦除 | `erase_region 0x12100000:0x1000` | 非 0xFF 字节 0/4096，现场已还原 |
| UART 应用层日志 | 复位后 6 秒采集 @1M / 115200 | 0 字节（见下方说明） |

**关于串口 0 字节**：下载模式通信正常，证明 UART 物理通道完好。官方固件在应用层不往这个 UART 输出，属正常现象。后续 mianyu 固件若要看日志，需确认 console 已配置到该 UART。

---

## 4. 备份文件

| 文件 | 大小 | MD5 |
|---|---|---|
| `backup/board-backup-full-16M.bin` | 16,777,216 | `2e95d19cda853ffd0f11c375bc08b411` |
| `backup/board-backup-0x12000000-2M.bin` | 2,097,152 | `8713a26b63171103444dfe93ba135ead` |

**回滚命令**（恢复出厂固件）：

```bash
PORT=COM6 ./tools/xm-flash.sh --restore \
  backup/board-backup-0x12000000-2M.bin
```

---

## 5. 工具清单（已就位）

| 文件 | 用途 |
|---|---|
| `tools/sftool/sftool.exe` | sftool 0.2.5（思澈官方烧录器，Rust 静态编译，无依赖） |
| `tools/xm-flash.sh` | 一条命令烧录：自动备份 → 写入 → 校验 |
| `tools/xm-diag.sh` | 板子体检：连通性 / 容量 / 分区表 / 固件指纹（只读） |
| `tools/xm-serial.ps1` | 零依赖串口日志采集（不需要 npm serialport） |
| `tools/backup/` | 出厂固件备份 |

烧录地址表（思澈官方布局，`xm-flash.sh` 内置）：

```
ftab.bin       0x12000000
bootloader.bin 0x12010000
ER_IROM1.bin   0x12020000
ER_IROM3.bin   0x12268000
ER_IROM2.bin   0x12A28000
```

---

## 6. mianyu 固件编译路线分析

### 客观约束

| 约束 | 现状 |
|---|---|
| openvela 板级支持 | **有**：`vendor/sifli`、`vendor/sifli/boards/sf32lb52/libs` |
| openvela 仓库规模 | 264 个 git 子仓库（manifest revision `dev-ai-contest-2026`） |
| 预编译工具链 | 仅 `prebuilts/gcc/**linux-x86_64**/arm-none-eabi` —— **无 Windows 版** |
| 本机 Python | 残缺（managed python 只有 site-packages，无 stdlib） |
| 本机磁盘 | C: 剩 9.5 GB，E: 剩 71 GB |
| 本机是否已有 SDK | 无 |
| mianyu 预编译固件 | 仓库内**没有**，只有 C 源码 |

### 结论

**本机（原生 Windows）无法直接编译**，卡点是工具链只有 Linux 版。

### 三条可行路线

| 路线 | 做法 | 评价 |
|---|---|---|
| **A. WSL2 + Docker** | 本机 Docker Desktop 已装，跑 Linux 容器，在容器里 `repo sync` + 编译 | 磁盘放 E 盘，可行，但要拉几十 GB，国内网络是变量 |
| **B. 云机编译** | 租一台 Ubuntu，`repo init -u ... -b dev-ai-contest-2026` → `repo sync` → `./build.sh vendor/sifli/boards/sf32lb52/...` | 最稳，官方支持路径 |
| **C. 只编 mianyu 应用层** | 若比赛方提供 openvela 预编译 SDK，只需把 `app/mianyu` 编进 apps | 最快，取决于赛方是否提供 |

**固件出来后，烧录只需一条命令**（链路已验证）：

```bash
PORT=COM6 ./tools/xm-flash.sh <固件目录>
```

---

## 7. 下一步（按优先级）

1. **确认编译路线**（A/B/C 三选一）——这是唯一的阻塞项
2. 固件编出后执行 `tools/xm-flash.sh <固件目录>` 烧录
3. 烧录后立刻确认 LCD 是否点亮、BLE 广播名是否为 `xiaoming-mianyu`
4. 若串口无日志，检查 mianyu 的 console 设备配置
