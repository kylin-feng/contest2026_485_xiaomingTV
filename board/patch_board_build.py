#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在云机上把语音链路接进构建（幂等）。

1) CMakeLists.txt 的 SRCS 加上 bsp_voice_link.c
2) sifli_ap.c 在音频自检之后调用 bsp_voice_link_start()
3) CONFIG_INIT_ENTRYPOINT: nsh_main -> mianyu_main（三处都要改，见下）
每步改之前都留带时间戳的 .bak。
"""
import os
import shutil
import time

SRC = "/root/vela/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src"
CM = SRC + "/CMakeLists.txt"
AP = SRC + "/sifli_ap.c"
STAMP = time.strftime("%Y%m%d_%H%M%S")

# init 入口替换：这些地方都要一致，否则会出现"编译过了但行为不对"
#   1) configs/nsh/defconfig            —— 配置源头（下次 configure 用）
#   2) cmake_out/<board>/.config        —— 当前构建的配置
#   3) cmake_out/<board>/include/nuttx/config.h —— 编译器真正读的头文件
#      注意：改 defconfig 对 ninja 增量**无效**，config.h 只在 cmake configure 时
#      重生成，所以必须直接改 config.h 才能让 nx_bringup.c 用新符号重编。
ENTRY_OLD = "nsh_main"
ENTRY_NEW = "mianyu_main"
CFG_FILES = [
    "/root/vela/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh/defconfig",
    "/root/vela/cmake_out/sf32lb52_devkit_lcd/.config",
    "/root/vela/cmake_out/sf32lb52_devkit_lcd/include/nuttx/config.h",
]


def backup(path):
    dst = "%s.bak.vlink.%s" % (path, STAMP)
    shutil.copy(path, dst)
    return dst


def patch_entrypoint():
    for f in CFG_FILES:
        if not os.path.exists(f):
            print("[!!] 不存在，跳过: %s" % f)
            continue
        s = open(f, encoding="utf-8").read()
        if ENTRY_NEW in s and "CONFIG_INIT_ENTRYPOINT" in s:
            print("[--] %s 已是 %s" % (os.path.basename(f), ENTRY_NEW))
            continue
        hits = [l for l in s.splitlines() if "CONFIG_INIT_ENTRYPOINT" in l]
        if not hits:
            print("[!!] %s 里没有 CONFIG_INIT_ENTRYPOINT" % f)
            continue
        print("[ok] 备份 ->", backup(f))
        # 三种写法都要覆盖：defconfig/.config 是 ="nsh_main"，生成的 config.h 是 空格 nsh_main
        for pat in ('CONFIG_INIT_ENTRYPOINT="%s"' % ENTRY_OLD,
                    "CONFIG_INIT_ENTRYPOINT=%s" % ENTRY_OLD,
                    "CONFIG_INIT_ENTRYPOINT %s" % ENTRY_OLD):
            s = s.replace(pat, pat.replace(ENTRY_OLD, ENTRY_NEW))
        open(f, "w", encoding="utf-8").write(s)
        for l in open(f, encoding="utf-8").read().splitlines():
            if "CONFIG_INIT_ENTRYPOINT" in l:
                print("     %s -> %s" % (os.path.basename(f), l.strip()))


# ---------- 1) CMakeLists ----------
s = open(CM, encoding="utf-8").read()
if "bsp_voice_link.c" in s:
    print("[--] CMakeLists 已包含 bsp_voice_link.c")
else:
    old = "bsp_audio_test.c )"
    assert old in s, "CMakeLists 的 SRCS 行没匹配上"
    print("[ok] 备份 ->", backup(CM))
    s = s.replace(old, "bsp_audio_test.c bsp_voice_link.c )")
    open(CM, "w", encoding="utf-8").write(s)
    print("[ok] CMakeLists 已加入 bsp_voice_link.c")

for l in open(CM, encoding="utf-8").read().splitlines():
    if l.startswith("set(SRCS"):
        print("     " + l)

# ---------- 2) sifli_ap.c ----------
s = open(AP, encoding="utf-8").read()
if "bsp_voice_link_start" in s:
    print("[--] sifli_ap.c 已包含 bsp_voice_link_start")
else:
    anchor = (
        '    tmpret = bsp_audio_selftest_start();\n'
        '    if (tmpret < 0)\n'
        '      {\n'
        '        serr("WARN: bsp_audio_selftest_start failed: %d\\n", tmpret);\n'
        '      }\n'
        '  }\n'
    )
    assert anchor in s, "sifli_ap.c 的音频自检锚点没匹配上"
    print("[ok] 备份 ->", backup(AP))
    block = anchor + (
        '\n'
        '  /* 串口语音链路：板子当"耳朵 + 嘴 + 按键"，识别/合成放 PC 网关。\n'
        '   * 协议与 PC 侧 voice/board_link.py 一致（A5 5A + CRC16-CCITT-FALSE），\n'
        '   * 启动时会先跑一遍协议自检，不通过就不开链路。 */\n'
        '  {\n'
        '    extern int bsp_voice_link_start(void);\n'
        '\n'
        '    tmpret = bsp_voice_link_start();\n'
        '    if (tmpret < 0)\n'
        '      {\n'
        '        serr("WARN: bsp_voice_link_start failed: %d\\n", tmpret);\n'
        '      }\n'
        '  }\n'
    )
    open(AP, "w", encoding="utf-8").write(s.replace(anchor, block))
    print("[ok] sifli_ap.c 已插入语音链路启动")

print("--- sifli_ap.c 启动段 ---")
lines = open(AP, encoding="utf-8").read().splitlines()
for i, l in enumerate(lines):
    if "bsp_voice_link_start" in l or "bsp_audio_selftest_start" in l:
        for k in range(max(0, i - 1), min(len(lines), i + 12)):
            print("%4d %s" % (k + 1, lines[k]))
        break

# ---------- 3) init 入口：nsh_main -> mianyu_main ----------
patch_entrypoint()

print("DONE")
