#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""无 make 环境下的构建/自测驱动（Windows 评审友好）。

为什么有这个东西：仓库主构建入口是 Makefile（`make test`），但 Windows 上
通常没有 make（Git for Windows 不带，装一个又要折腾 PATH）。本脚本用 Python
复刻 Makefile 的 `test` / `report` / `demo` / `app` 四个目标，只依赖一个可用的
C 编译器（gcc / clang / cc 任一）+ Python 3，评审 clone 下来即可验证。

**唯一的权威判据与 Makefile 完全一致**：测试程序打印的
`MY_RESULT: pass=N fail=M` 行。不数 [PASS] 行数（原因见 Makefile 注释：
手动打印的明细行不计入程序内部 g_pass，数行法必然对不齐）。

用法：
    python tests/pc_build.py            # 等价 make test
    python tests/pc_build.py report     # 等价 make report（追加逐套件断言统计）
    python tests/pc_build.py demo       # 等价 make demo（整晚闭环演示）
    python tests/pc_build.py app        # 等价 make app（HAL 接入版真机主循环）
    python tests/pc_build.py clean
"""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")

# 路径约定与根 Makefile 一致：应用包整体在 app/mianyu/ 下
# （这一层会被 contest2026_485_xiaomingTV.xml 的 <linkfile> 映射到
# openvela 的 packages/demos/contest2026_485_mianyu）。改这里必须同步改 Makefile。
APPDIR = os.path.join(ROOT, "app", "mianyu")

MODULES = ["noise_gen", "sleep_detect", "fade", "schedule", "sleep_memory", "breathe"]
CFLAGS = ["-O2", "-Wall", "-Wextra", "-Wshadow", "-std=c11",
          "-I" + os.path.join(APPDIR, "include")]
LDLIBS = ["-lm"]
TIMEOUT_S = 30
APP_INCLUDES = ["-I" + os.path.join(APPDIR, "hal"),
                "-I" + APPDIR,
                "-I" + os.path.join(APPDIR, "ui")]


def find_cc():
    for cand in ("gcc", "clang", "cc", "tcc"):
        p = shutil.which(cand)
        if p:
            return p
    sys.exit("找不到 C 编译器（试过 gcc / clang / cc / tcc）。装一个 MinGW-w64 或 MSYS2 再来。")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True,
                          errors="replace", **kw)


def compile_to(out, srcs, extra=None):
    cc = find_cc()
    cmd = [cc] + CFLAGS + (extra or []) + ["-o", out] + srcs + LDLIBS
    p = run(cmd)
    if p.returncode != 0:
        print("编译失败: " + " ".join(srcs))
        print((p.stdout or "") + (p.stderr or ""))
        return False
    return True


def core_src(mod):
    return os.path.join(APPDIR, "src", "mianyu_%s.c" % mod)


def build_tests():
    os.makedirs(BUILD, exist_ok=True)
    ok = True
    for m in MODULES:
        exe = os.path.join(BUILD, "test_" + m)
        if not os.path.exists(core_src(m)):
            print("  缺少模块源: %s" % core_src(m))
            ok = False
            continue
        if not compile_to(exe, [os.path.join(HERE, "test_%s.c" % m), core_src(m)]):
            ok = False
    return ok


def do_test(show_report=False):
    if not build_tests():
        return 1

    print("=" * 50)
    print(" 安眠科技 · 核心算法层单元测试")
    print("=" * 50)

    n_pass = n_fail = 0
    stats = []
    for m in MODULES:
        exe = os.path.join(BUILD, "test_" + m)
        log = os.path.join(BUILD, m + ".log")
        print("\n>>> %s" % m)
        # 超时保护：schedule 模块曾因日期换算笔误死循环，没有超时会让整套挂住
        try:
            p = run([exe], timeout=TIMEOUT_S)
            body = (p.stdout or "") + (p.stderr or "")
        except subprocess.TimeoutExpired as e:
            body = ((e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes)
                    else (e.stdout or ""))
            body += "\n__TIMEOUT__\n"
        with open(log, "w", encoding="utf-8") as f:
            f.write(body)

        if "__TIMEOUT__" in body:
            print("    TIMEOUT — 超过 %ds，疑似死循环；日志：%s" % (TIMEOUT_S, log))
            n_fail += 1
            continue

        last = None
        for line in body.splitlines():
            if line.startswith("MY_RESULT: "):
                last = line.strip()
        if last is None:
            print("    NO_RESULT — 未打印 MY_RESULT 行（程序异常退出？）；日志：%s" % log)
            n_fail += 1
            continue

        mo = re.match(r"^MY_RESULT: pass=(\d+) fail=(\d+)$", last)
        if not mo:
            print("    NO_RESULT — MY_RESULT 行格式不符：%s" % last)
            n_fail += 1
            continue

        pn, fn = int(mo.group(1)), int(mo.group(2))
        stats.append((m, pn, fn))
        if fn == 0:
            print("    " + last)
            n_pass += 1
        else:
            print("    FAILED — 完整日志：%s" % log)
            print("    " + last)
            for line in body.splitlines():
                if "[FAIL]" in line:
                    print("    " + line)
            n_fail += 1

    print("\n" + "=" * 50)
    print(" 套件： %d 通过 / %d 失败" % (n_pass, n_fail))
    print("=" * 50)

    if show_report:
        print("\n断言统计（逐套件，取自各程序的 MY_RESULT 权威行）：")
        tp = tf = 0
        for m, pn, fn in stats:
            print("  %-14s %3d PASS / %3d FAIL" % (m, pn, fn))
            tp += pn
            tf += fn
        print("  " + "-" * 30)
        print("  %-14s %3d PASS / %3d FAIL" % ("合计", tp, tf))

    return 1 if n_fail else 0


def build_demo():
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "night_demo")
    srcs = [os.path.join(ROOT, "demo", "night_demo.c")] + [core_src(m) for m in MODULES]
    if not compile_to(exe, srcs, ["-I" + os.path.join(ROOT, "demo")]):
        return None
    return exe


def build_app():
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "mianyu_app")
    # 注意这里【不含】ui/breathe_lvgl.c 和 ui/watch_ui.c：它们 #include
    # <lvgl/lvgl.h>，只在 openvela 工程里编译。PC 侧界面接口走 hal/sim 的空实现
    # （见 mianyu_hal_sim.c 里"手表界面"那段），所以主循环照样能整晚跑完。
    srcs = ([core_src(m) for m in MODULES]
            + [os.path.join(APPDIR, "mianyu_app_main.c"),
               os.path.join(APPDIR, "hal", "sim", "mianyu_hal_sim.c"),
               # PC 侧的进程入口：真机上这活儿由 nuttx_add_application 代劳，
               # 见 hal/sim/pc_main.c 头注释。
               os.path.join(APPDIR, "hal", "sim", "pc_main.c")])
    if not compile_to(exe, srcs, APP_INCLUDES):
        return None
    return exe


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "test"
    if mode == "clean":
        shutil.rmtree(BUILD, ignore_errors=True)
        print("已清理 build/")
        return 0
    if mode == "test":
        return do_test(False)
    if mode == "report":
        return do_test(True)
    if mode == "demo":
        exe = build_demo()
        if not exe:
            return 1
        print("=" * 50)
        print(" 安眠科技 · 整晚闭环演示")
        print("=" * 50)
        # 不捕获输出：演示要跑一整晚（压缩过），让它直接流到终端，
        # 用户可以实时看到进度，也不用先攒在内存里。
        return subprocess.run([exe]).returncode
    if mode == "app":
        exe = build_app()
        if not exe:
            return 1
        print("=" * 50)
        print(" 安眠科技 · 真机主循环（HAL 接入版，PC 模拟器）")
        print("=" * 50)
        return subprocess.run([exe]).returncode
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
