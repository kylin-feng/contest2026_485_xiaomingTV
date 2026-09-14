#!/usr/bin/env bash
# xm-flash.sh - 把 mianyu 固件烧到 SF32LB52-Devkit-LCD
#
# 用法:
#   ./xm-flash.sh <固件目录>            # 正式烧录（先自动备份）
#   ./xm-flash.sh <固件目录> --dry-run  # 只打印将要执行的命令
#   ./xm-flash.sh <固件目录> --no-backup # 跳过备份（不推荐）
#   ./xm-flash.sh --restore <备份文件>  # 回滚到备份（恢复出厂固件）
#
# 环境变量:
#   PORT=COM6 BAUD=1000000 CHIP=SF32LB52
#
# 依赖: 同目录下 sftool.exe (v0.2.5+)

set -euo pipefail

ARG1="${1:-}"
ARG2="${2:-}"

PORT="${PORT:-COM6}"
BAUD="${BAUD:-1000000}"
CHIP="${CHIP:-SF32LB52}"
TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
SFTOOL="$TOOLS_DIR/sftool/sftool.exe"
BACKUP_DIR="$TOOLS_DIR/backup"

# sftool 是 Windows 程序，必须喂 Windows 风格路径
winpath() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}

# --- 回滚模式 ---
if [ "$ARG1" = "--restore" ]; then
  BK="$ARG2"
  if [ -z "$BK" ] || [ ! -f "$BK" ]; then
    echo "用法: $0 --restore <备份文件.bin>" >&2
    exit 2
  fi
  SZ=$(stat -c%s "$BK" 2>/dev/null || stat -f%z "$BK")
  echo "=== 回滚出厂固件 ==="
  echo "  文件: $BK ($SZ 字节) -> 0x12000000"
  "$SFTOOL" -c "$CHIP" -p "$PORT" -b "$BAUD" --before default_reset --after soft_reset \
    --connect-attempts 5 --compat true write_flash --verify "$(winpath "$BK")@0x12000000"
  echo "回滚完成，板子已软复位。"
  exit 0
fi

FW_DIR="$ARG1"
MODE="$ARG2"

# 思澈官方 flash 布局（SDK 1.3.0+）
#   ftab       0x12000000   flash 分区表，必须最后写、且与其余分区一致
#   bootloader 0x12010000
#   ER_IROM1   0x12020000   应用主体
#   ER_IROM3   0x12268000   附加只读段
#   ER_IROM2   0x12A28000   资源段
declare -A MAP=(
  [ftab.bin]=0x12000000
  [bootloader.bin]=0x12010000
  [ER_IROM1.bin]=0x12020000
  [ER_IROM3.bin]=0x12268000
  [ER_IROM2.bin]=0x12A28000
)
# 兼容其他常见命名
declare -A ALIAS=(
  [rtthread.bin]=ER_IROM1.bin
  [app.bin]=ER_IROM1.bin
  [main.bin]=ER_IROM1.bin
  [ftab_a.bin]=ftab.bin
)

if [ -z "$FW_DIR" ]; then
  echo "用法: $0 <固件目录> [--dry-run|--no-backup]" >&2
  echo "      $0 --restore <备份文件.bin>" >&2
  exit 2
fi
if [ ! -d "$FW_DIR" ]; then
  echo "错误: 固件目录不存在: $FW_DIR" >&2
  exit 2
fi
if [ ! -x "$SFTOOL" ] && [ ! -f "$SFTOOL" ]; then
  echo "错误: 找不到 sftool.exe: $SFTOOL" >&2
  exit 2
fi

# --- 收集固件文件 ---
WRITE_ARGS=()
FOUND=()
while IFS= read -r f; do
  base="$(basename "$f")"
  key="$base"
  if [ -n "${ALIAS[$base]:-}" ]; then key="${ALIAS[$base]}"; fi
  if [ -z "${MAP[$key]:-}" ]; then
    echo "跳过未知文件: $base（不在官方布局表中）" >&2
    continue
  fi
  WRITE_ARGS+=("$(winpath "$f")@${MAP[$key]}")
  FOUND+=("$base -> ${MAP[$key]}")
done < <(find "$FW_DIR" -maxdepth 2 -type f -name "*.bin" | sort)

if [ ${#WRITE_ARGS[@]} -eq 0 ]; then
  echo "错误: $FW_DIR 里没找到任何可烧录的 .bin" >&2
  exit 2
fi

echo "=== 待烧录 ==="
for x in "${FOUND[@]}"; do echo "  $x"; done
echo "  端口=$PORT 波特率=$BAUD 芯片=$CHIP"
echo ""

if [ "$MODE" = "--dry-run" ]; then
  echo "[dry-run] 备份命令:"
  echo "  $SFTOOL -c $CHIP -p $PORT -b $BAUD read_flash $BACKUP_DIR/board-backup-<ts>.bin@0x12000000:0x200000"
  echo "[dry-run] 烧录命令:"
  echo "  $SFTOOL -c $CHIP -p $PORT -b $BAUD --before default_reset --after soft_reset --compat true write_flash --verify ${WRITE_ARGS[*]}"
  exit 0
fi

# --- 先备份 ---
if [ "$MODE" != "--no-backup" ]; then
  mkdir -p "$BACKUP_DIR"
  TS="$(date +%Y%m%d-%H%M%S)"
  BK="$BACKUP_DIR/board-backup-$TS-0x12000000-2M.bin"
  echo "=== 步骤 1/2: 备份当前固件 ==="
  echo "  -> $BK"
  "$SFTOOL" -c "$CHIP" -p "$PORT" -b "$BAUD" --before default_reset --after no_reset \
    --connect-attempts 5 --compat true read_flash "$(winpath "$BK")@0x12000000:0x200000" >/dev/null
  echo "  备份完成: $(stat -c%s "$BK" 2>/dev/null || stat -f%z "$BK") 字节"
else
  echo "=== 步骤 1/2: 跳过备份（--no-backup）==="
fi

# --- 再烧录 ---
echo "=== 步骤 2/2: 烧录 ==="
"$SFTOOL" -c "$CHIP" -p "$PORT" -b "$BAUD" --before default_reset --after soft_reset \
  --connect-attempts 5 --compat true write_flash --verify "${WRITE_ARGS[@]}"

echo ""
echo "烧录完成。板子已软复位，应开始运行新固件。"
echo "看日志:  PORT=$PORT ./xm-serial.ps1   (Windows PowerShell)"
