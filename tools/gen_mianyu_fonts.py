#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按界面源码里**实际用到的字**重新生成中文点阵字体。

为什么要有这个脚本，而不是写死一串字符：

上一版是手写一串 --symbols 交给 lv_font_conv 的，结果界面文案改了、字体
没跟着改，屏上就出现方块 —— 实测有 12 个字是这样漏掉的（「周二」的"二"、
「放松」的"松"、「呼气」的"呼"、"还没有记录"整句）。手抄字符集这个做法
本身就会漏。

这一版改成**从源码里抠**：把界面源文件中所有字符串字面量的非 ASCII 字符
扫出来，取并集去生成。改了文案重新跑一次这个脚本，就不会再漏。
（同理，中文标点必须显式出现在字面量里才会被收进来 —— 字体里本来
一个中文标点都没有，`--range 0x20-0x7e` 只覆盖半角 ASCII。）

用法:
    python tools/gen_mianyu_fonts.py             # 生成到 ui/ 下
    python tools/gen_mianyu_fonts.py --check     # 只列出会收进来的字符，不生成
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UI = os.path.join(ROOT, "app", "mianyu", "ui")

# 字体源。文泉驿微米黑是原始选型（GPL v2 + 字体例外）；这条链路上不一定有，
# 按顺序取第一个存在的。
# 注意：**不支持 TTC**（把 msyh.ttc / simsun.ttc 传进去会报
# "Unsupported OpenType signature ttcf"），所以候选里只放单体 TTF。
FONT_CANDIDATES = [
    os.environ.get("MIANYU_FONT", ""),
    "/tmp/wqy-microhei-0.ttf",
    "C:/Windows/Fonts/NotoSansSC-VF.ttf",
    "C:/Windows/Fonts/simhei.ttf",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
]

# 会被扫描的字面量来源：界面上画出来的文字全在这里。
TEXT_SOURCES = ["watch_ui.c"]

SIZES = [16, 32]
BPP = 4


def find_font():
    for p in FONT_CANDIDATES:
        if p and os.path.exists(p):
            return p
    return None


def literals_in(path):
    """把 C 源码里的所有双引号字面量抠出来。

    **刻意不做转义解码**：`.encode('utf-8').decode('unicode_escape')` 这种写法
    会把含反斜杠的字面量里的中文按单字节解码，中文全变乱码（实测收进来
    `¡¢§¨ª°±²´µ` 这种字符）。这里只关心"画到屏上的字形"，转义序列本身
    （\\n、\\t）产出的都是 ASCII，不在 --symbols 里也无所谓。"""
    with open(path, encoding="utf-8") as f:
        src = f.read()
    # 先去掉注释，免得注释里的中文被算进来（字体只该含真正画到屏上的字）
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return re.findall(r'"((?:[^"\\\n]|\\.)*)"', src)


def collect_chars():
    chars = set()
    for name in TEXT_SOURCES:
        p = os.path.join(UI, name)
        if not os.path.exists(p):
            print("!! 找不到 %s" % p)
            continue
        for lit in literals_in(p):
            for ch in lit:
                if ord(ch) > 0x7E:
                    chars.add(ch)
    return sorted(chars)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只列字符，不生成")
    args = ap.parse_args()

    chars = collect_chars()
    print("从 %s 收到 %d 个非 ASCII 字符：" % ("、".join(TEXT_SOURCES), len(chars)))
    print("".join(chars))
    if args.check:
        return 0

    font = find_font()
    if not font:
        print("!! 找不到可用的 TTF/TTC 字体源。设 MIANYU_FONT 指向一个中文字体。")
        return 2
    print("字体源: %s" % font)

    tool = os.environ.get("LV_FONT_CONV") or shutil.which("lv_font_conv")
    if not tool:
        # 允许用项目内装的那份（见 README 的安装说明）
        cand = os.path.join(os.path.dirname(HERE), "..", "fonttool",
                            "node_modules", "lv_font_conv", "lv_font_conv.js")
        cand = os.path.abspath(cand)
        if os.path.exists(cand):
            tool = "node " + cand
    if not tool:
        print("!! 找不到 lv_font_conv。npm i lv_font_conv 后重试，"
              "或用环境变量 LV_FONT_CONV 指定其路径。")
        return 3

    for sz in SIZES:
        out = os.path.join(UI, "lv_font_mianyu_%d.c" % sz)
        cmd = ("%s --font %s --size %d --bpp %d --format lvgl --no-compress "
               "--range 0x20-0x7e --symbols %s --lv-include lvgl/lvgl.h "
               "--lv-font-name lv_font_mianyu_%d -o %s"
               % (tool, font, sz, BPP, "".join(chars), sz, out))
        print("生成 %dpx ..." % sz)
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])
            return 4
        print("  -> %s (%d 字节)" % (out, os.path.getsize(out)))

    print("\n完成。别忘了两件事：")
    print("  1) 界面文案再改动时重跑本脚本，否则新字会变方块；")
    print("  2) 把上面那行字符清单同步到 ui/README.md。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
