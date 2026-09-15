#!/bin/bash
# 验证手表界面时，把串口语音链路（bsp_voice_link）关掉，用完要恢复。
#
# 为什么必须关：vlink 一起来就以 1 Mbaud 满速往上灌 MIC 帧（实测 130 秒
# 18 MB），USB 串口根本吃不下，日志被二进制噪声撞得七零八落 —— 想看的
# [ft6146] / [watch_ui] 那些行全被截断丢掉，等于没有日志。
# 界面验证不需要麦克风上行，先关掉；音频链路本身之前已经单独验过。
#
#   bash patch_vlink_quiet.sh off    # 关掉（验证界面时用）
#   bash patch_vlink_quiet.sh on     # 恢复
set -e
cd /root/vela
AP=vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/sifli_ap.c
BAK=/tmp/sifli_ap.c.vlink_orig

MODE="${1:-off}"

if [ "$MODE" = "off" ]; then
  [ -f "$BAK" ] || cp "$AP" "$BAK"
  python3 - <<'PYEOF'
AP = 'vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/sifli_ap.c'
s = open(AP, encoding='utf-8').read()
old = '''    tmpret = bsp_voice_link_start();
    if (tmpret < 0)
      {
        serr("WARN: bsp_voice_link_start failed: %d\\n", tmpret);
      }'''
new = '''    tmpret = 0;
    if (0)   /* 界面验证期临时关闭：vlink 满速上行会把串口日志冲烂 */
      {
        tmpret = bsp_voice_link_start();
        if (tmpret < 0)
          {
            serr("WARN: bsp_voice_link_start failed: %d\\n", tmpret);
          }
      }'''
assert old in s, 'voice_link start pattern not found'
open(AP, 'w', encoding='utf-8').write(s.replace(old, new, 1))
print('[ok] voice_link 已临时关闭')
PYEOF
else
  [ -f "$BAK" ] && cp "$BAK" "$AP" && echo "[ok] voice_link 已恢复"
fi

export PATH=/root/vela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
cd /root/vela/cmake_out/sf32lb52_devkit_lcd
ninja -j96 2>&1 | tail -n 6
ls -la nuttx.bin
md5sum nuttx.bin
