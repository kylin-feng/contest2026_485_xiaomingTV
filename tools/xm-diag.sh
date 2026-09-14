#!/usr/bin/env bash
# xm-diag.sh - SF32LB52 板子现场体检：连通性 / flash 容量 / 分区表 / 固件指纹
#
# 用法:  cd tools && PORT=COM6 ./xm-diag.sh
# 说明:  只读操作，不写不擦，安全。所有路径用相对路径，兼容 Windows sftool。
#
# 环境变量: PORT(默认 COM6) BAUD(默认 1000000) CHIP(默认 SF32LB52) NODE_BIN

set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$TOOLS_DIR"

PORT="${PORT:-COM6}"
BAUD="${BAUD:-1000000}"
CHIP="${CHIP:-SF32LB52}"
NODE_BIN="${NODE_BIN:-node}"
SFTOOL="sftool/sftool.exe"
TMP=".diag-tmp"

if [ ! -f "$SFTOOL" ]; then
  echo "错误: 找不到 $SFTOOL" >&2
  exit 2
fi

rm -rf "$TMP"; mkdir -p "$TMP"
trap 'rm -rf "$TOOLS_DIR/$TMP"' EXIT

run() {
  "$SFTOOL" -c "$CHIP" -p "$PORT" -b "$BAUD" --before default_reset --after no_reset \
    --connect-attempts 3 --compat true "$@" 2>&1
}

ok() { ! echo "$1" | grep -qi "error"; }

echo "=== 1. 连通性 ==="
OUT="$(run read_flash "$TMP/p.bin@0x12000000:16")"
if ! ok "$OUT"; then
  echo "  无法连接芯片。排查："
  echo "   - TX/RX 是否交叉（模块 TX -> 板子 RX）"
  echo "   - GND 是否共地"
  echo "   - 板子是否上电"
  echo "   - 串口是否被别的程序占用（PuTTY / 串口助手）"
  echo "   - COM 口是否为 $PORT"
  exit 1
fi
echo "  芯片在线：SF32LB52 @ $PORT @ $BAUD"

echo ""
echo "=== 2. flash 容量 ==="
LASTOK=""
FIRSTFAIL=""
for off in 0x200000 0x400000 0x800000 0xC00000 0x1000000 0x1800000 0x2000000; do
  ADDR=$(printf "0x%08X" $((0x12000000 + off)))
  if ok "$(run read_flash "$TMP/s.bin@$ADDR:16")"; then
    LASTOK=$ADDR
  else
    FIRSTFAIL=$ADDR
    break
  fi
done
echo "  最后可读地址: ${LASTOK:-none}"
echo "  首个不可读地址: ${FIRSTFAIL:-none（>= 0x14000000）}"
if [ -n "$FIRSTFAIL" ]; then
  echo "  -> flash 容量 = $FIRSTFAIL（起始 0x12000000，共 $(( (0x${FIRSTFAIL#0x} - 0x12000000) / 1024 / 1024 )) MB）"
else
  echo "  -> flash 容量 >= 32 MB"
fi

echo ""
echo "=== 3. 分区表 (ftab @0x12000000) ==="
run read_flash "$TMP/ftab.bin@0x12000000:4096" >/dev/null
if [ -s "$TMP/ftab.bin" ]; then
  MAGIC="$(head -c 4 "$TMP/ftab.bin" | xxd -p)"
  echo "  magic = $MAGIC  (期望 46434553 = 'FCES')"
  if [ "$MAGIC" = "46434553" ]; then
    echo "  -> 分区表有效"
  else
    echo "  -> 分区表异常，flash 可能是空片或格式不符"
  fi
else
  echo "  读取失败"
fi

echo ""
echo "=== 4. 数据分布 ==="
run read_flash "$TMP/img.bin@0x12000000:0x200000" >/dev/null
if [ -s "$TMP/img.bin" ]; then
  "$NODE_BIN" -e '
    const fs=require("fs");
    const b=fs.readFileSync(process.argv[1]);
    const S=65536; let last=-1;
    for(let o=0;o<b.length;o+=S){
      let n=0; for(let i=o;i<Math.min(o+S,b.length);i++) if(b[i]!==0xFF) n++;
      if(n>0) last=o;
    }
    for(let o=0;o<=last;o+=S){
      let n=0; for(let i=o;i<Math.min(o+S,b.length);i++) if(b[i]!==0xFF) n++;
      if(n>0) console.log("  0x"+(0x12000000+o).toString(16).toUpperCase()+"  "+n+"/"+S+" 字节有数据");
    }
    console.log("  固件占用上界: 0x"+(0x12000000+last+S).toString(16).toUpperCase());
  ' "$TMP/img.bin"
  if [ $? -ne 0 ]; then echo "  (需要 node 才能分析分布)"; fi

  echo ""
  echo "=== 5. 固件指纹 ==="
  strings -n 6 "$TMP/img.bin" \
    | grep -Ei "devkit|SiFli Corporation|RT-Thread|openvela|mianyu|[A-Z][a-z]{2} [ 0-9][0-9] 20[0-9]{2}" \
    | sort -u | head -12 | sed 's/^/  /'
else
  echo "  读取失败"
fi

echo ""
echo "体检完成。"
