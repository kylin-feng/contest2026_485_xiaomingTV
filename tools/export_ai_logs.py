#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 AI Coding 工具的会话流水导出成 logs/raw/ 下的逐日可读记录。

===================== 这是干什么的 =====================

`logs/raw/` 是给大赛核验「AI Coding 日志」那一项用的原始流水。09-08~09-15
那六天是用当时的工具（WorkBuddy/CodeBuddy）导的；09-16 之后换成 Kimi Code，
它的会话存在 `~/.kimi-code/sessions/<workspace>/<session>/agents/*/wire.jsonl`，
**默认只在本机暂存、不会自动进仓**，必须主动导出 —— 就是本脚本。

===================== 只导本作品的会话 =====================

本机同一个工作区下还躺着别的项目的会话（westlake-3d、剪映、snipaste…）。
它们和本作品无关，导进来只会稀释日志。所以这里按**首个用户指令里的关键词**
筛选（默认 小鸣TV / 安眠科技 / openvela / contest2026 / mianyu / 眠语），
也可以用 `--session <id>` 显式指定。

===================== 口径 =====================

与 logs/raw/README.md 里那六天保持一致：
  · 用户指令：原文整段，单条超 8000 字截断
  · 助手回复：原文，单条超 4000 字截断并标 `...(截断)`
  · 模型思考：只留每条前 200 字（不是全文）
  · 工具调用：工具名 + 关键参数摘要；Write/Edit 只留路径和一小段预览，不贴整篇代码
  · 工具结果：单条最多 1000 字；base64 / 二进制 / 超长整块换成占位说明
  · 脱敏：口令/密钥**连片段一起**换掉（见下面 _secret_patterns 的说明）
  · 清洗：NUL 与其余控制字符一律剥掉、行尾统一 LF（否则文件会被当成二进制，
    Read 之类工具直接拒读，几百 KB 日志等于白导）

用法：
    python tools/export_ai_logs.py                    # 自动挑本作品的会话，导出到 logs/raw/
    python tools/export_ai_logs.py --list             # 只列出本机有哪些会话，不导出
    python tools/export_ai_logs.py --session <id> …   # 显式指定会话
"""
import argparse
import datetime
import glob
import json
import os
import re
import sys

HOME = os.path.expanduser("~")
SESS_ROOT = os.path.join(HOME, ".kimi-code", "sessions")

# 首个用户指令里出现这些词之一，就认为这个会话属于本作品
KEYWORDS = ["小鸣TV", "安眠科技", "openvela", "contest2026", "mianyu", "眠语"]

MAX_USER = 8000
MAX_REPLY = 4000
MAX_THINK = 200
MAX_RESULT = 1000
MAX_ARG = 300

# ---- 脱敏 ----
# **口令不写在代码里。** 这个脚本要提交到公开仓库，把密钥硬编码进"脱敏列表"
# 是自相矛盾的（第一版就是这么写的，被自己的扫描抓出来了）。
# 用环境变量传，逗号分隔：
#     set EXPORT_REDACT_SECRETS=你的口令,别的密钥
#     python tools/export_ai_logs.py
# 没设也不报错 —— 那样只会少脱敏，不会把密钥写进仓。
SECRETS = [x for x in os.environ.get("EXPORT_REDACT_SECRETS", "").split(",") if x]


def _secret_patterns():
    """把一个密钥展开成「它自己 + 所有长度>=3 的子串」，都作为要替换的字面量。

    为什么要连片段一起换：排查问题时我自己会在命令里写「grep 口令的某个片段」
    这种**片段**做核查，导出后这些片段就留在日志里了 —— 单个片段看着不像密钥，
    几段凑起来却能还原。与其事后打地鼠，不如把片段一并清掉。
    按长度从长到短排，保证完整形态先被整体替换掉。
    """
    pats = set()
    for sec in SECRETS:
        for i in range(len(sec)):
            for j in range(i + 3, len(sec) + 1):
                pats.add(sec[i:j])
    return sorted(pats, key=len, reverse=True)


LITERAL_REDACT = _secret_patterns()
REGEX_REDACT = [
    (re.compile(r"(password[=:]\s*)(?!<已脱敏>|<有值>)\S+"), r"\1<已脱敏>"),
    (re.compile(r"\bsk-[A-Za-z0-9_\-]{16,}"), "<已脱敏>"),
    (re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}"), "<已脱敏>"),
    (re.compile(r"(Bearer\s+)(?!<已脱敏>)[A-Za-z0-9._\-]{12,}"), r"\1<已脱敏>"),
]

# NUL + 除 TAB/LF 之外的控制字符，全部删掉。
# 用 translate + ord 数字构造，**不写反斜杠转义**：这个文件被改过两次，
# 每次都是转义层级搞错把真实控制字符写进了源码，反而让脚本自己坏掉。
_STRIP = {c: None for c in list(range(0, 9)) + [11, 12] + list(range(14, 32))}
_CR = chr(13)
_LF = chr(10)


def clean(s):
    """把文本收拾成「人类可读 + git 友好」。

    · 剥掉 NUL 与其余控制字符：工具输出里混进来的二进制（串口原始数据、
      base64 块）会带 NUL，带 NUL 的文件被多数工具当二进制、连 Read 都拒读。
    · 行尾统一成 LF：仓库用 .gitattributes 固定了 LF。
    """
    if not s:
        return s
    s = s.replace(_CR + _LF, _LF).replace(_CR, _LF)
    return s.translate(_STRIP)


def redact(s):
    if not s:
        return s
    for lit in LITERAL_REDACT:
        s = s.replace(lit, "<已脱敏>")
    for pat, rep in REGEX_REDACT:
        s = pat.sub(rep, s)
    return s


def cut(s, n, mark="...(截断)"):
    s = s or ""
    return s if len(s) <= n else s[:n] + mark


def looks_binary(s):
    """粗判 base64 / 二进制整块：长且几乎没有空白和中文。"""
    if len(s) < 2000:
        return False
    sample = s[:4000]
    printable = sum(1 for c in sample if c.isalnum() or c in "+/=_-")
    return printable > len(sample) * 0.95


def fmt_args(name, args):
    args = args or {}
    if name == "Bash":
        return "command: %s" % cut(args.get("command", ""), MAX_ARG)
    if name == "Write":
        return "path: %s | 内容 %d 字，预览: %s" % (
            args.get("path", ""), len(args.get("content", "") or ""),
            cut((args.get("content") or "").replace("\n", " "), 80))
    if name == "Edit":
        return "path: %s | 改 %d 字 -> %d 字" % (
            args.get("path", ""), len(args.get("old_string", "") or ""),
            len(args.get("new_string", "") or ""))
    if name == "Read":
        rng = ""
        if args.get("line_offset"):
            rng = " (line_offset=%s n_lines=%s)" % (args.get("line_offset"),
                                                    args.get("n_lines"))
        return "path: %s%s" % (args.get("path", ""), rng)
    return cut(json.dumps(args, ensure_ascii=False), MAX_ARG)


def fmt_result(res):
    if not isinstance(res, dict):
        return cut(str(res), MAX_RESULT)
    out = res.get("output") or ""
    if looks_binary(out):
        out = "（%d 字，疑似 base64/二进制整块，已省略）" % len(out)
    else:
        out = cut(out, MAX_RESULT)
    if res.get("isError"):
        out = "[错误] " + out
    if res.get("note"):
        out += "\n（注：%s）" % cut(str(res["note"]), 120)
    if res.get("truncated"):
        out += "\n（原始结果被工具截断过）"
    return out


def iter_events(path):
    """产出 (time_ms, kind, payload)。kind: user/reply/think/tool/res"""
    for line in open(path, encoding="utf-8", errors="replace"):
        try:
            r = json.loads(line)
        except Exception:
            continue
        t = r.get("type")
        ms = r.get("time") or r.get("created_at") or 0
        if t == "turn.prompt":
            try:
                txt = r["input"][0].get("text", "")
            except Exception:
                txt = ""
            if txt:
                yield ms, "user", txt
        elif t == "context.append_loop_event":
            e = r.get("event", {})
            et = e.get("type")
            if et == "content.part":
                p = e.get("part", {})
                if p.get("type") == "text" and p.get("text"):
                    yield ms, "reply", p["text"]
                elif p.get("type") == "think" and p.get("think"):
                    yield ms, "think", cut(p["think"], MAX_THINK)
            elif et == "tool.call":
                yield ms, "tool", (e.get("name"), e.get("args"))
            elif et == "tool.result":
                yield ms, "res", e.get("result")


def detect_sessions():
    """扫本机所有会话，返回 [(session_id, dir, first_prompt, t0, t1, n)]。"""
    out = []
    for wd in glob.glob(os.path.join(SESS_ROOT, "*")):
        for sd in (glob.glob(os.path.join(wd, "session_*"))
                   + glob.glob(os.path.join(wd, "ses_*"))):
            main = os.path.join(sd, "agents", "main", "wire.jsonl")
            if not os.path.exists(main):
                continue
            first, t0, t1, n = "", None, None, 0
            for ms, kind, payload in iter_events(main):
                n += 1
                t0 = ms if t0 is None else min(t0, ms)
                t1 = ms if t1 is None else max(t1, ms)
                if kind == "user" and not first:
                    first = payload
            if t0 is None:
                continue
            out.append((os.path.basename(sd), sd, first, t0, t1, n))
    out.sort(key=lambda x: x[3])
    return out


def matches_project(first_prompt):
    return any(k in (first_prompt or "") for k in KEYWORDS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="只列出本机会话，不导出")
    ap.add_argument("--session", action="append", default=[],
                    help="显式指定会话 id（可多次）")
    ap.add_argument("--out", default=None, help="输出目录（默认 <仓根>/logs/raw）")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = args.out or os.path.join(os.path.dirname(here), "logs", "raw")
    os.makedirs(out_dir, exist_ok=True)

    sessions = detect_sessions()
    if args.list:
        print("本机共 %d 个会话：" % len(sessions))
        for sid, sd, first, t0, t1, n in sessions:
            fmt = lambda ms: datetime.datetime.fromtimestamp(ms / 1000).strftime("%m-%d %H:%M")
            mark = "★本作品" if matches_project(first) else "  其它  "
            print("%s %s -> %s  %5d 条  %s" % (mark, fmt(t0), fmt(t1), n, sid))
            print("         %s" % (first or "")[:90].replace("\n", " "))
        return 0

    picked = []
    for sid, sd, first, t0, t1, n in sessions:
        if args.session:
            if sid in args.session:
                picked.append((sid, sd))
        elif matches_project(first):
            picked.append((sid, sd))
    if not picked:
        print("没挑到会话。用 --list 看看本机有哪些。")
        return 2
    print("导出会话：")
    for sid, sd in picked:
        print("  %s" % sid)

    # 汇总主 agent + 子 agent 的事件，按时间排序，再按本地日期分桶
    buckets = {}
    for sid, sd in picked:
        wires = [("main", os.path.join(sd, "agents", "main", "wire.jsonl"))]
        for a in sorted(glob.glob(os.path.join(sd, "agents", "agent-*"))):
            w = os.path.join(a, "wire.jsonl")
            if os.path.exists(w):
                wires.append((os.path.basename(a), w))
        for tag, w in wires:
            for ms, kind, payload in iter_events(w):
                day = datetime.datetime.fromtimestamp(ms / 1000).strftime("%Y-%m-%d")
                buckets.setdefault(day, []).append((ms, tag, kind, payload))

    for day in sorted(buckets):
        evs = sorted(buckets[day], key=lambda x: x[0])
        path = os.path.join(out_dir, "%s.md" % day)
        stat = {}
        for _, _, kind, _ in evs:
            stat[kind] = stat.get(kind, 0) + 1
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("# AI Coding 原始流水 %s（本作品）\n\n" % day)
            f.write("由 `tools/export_ai_logs.py` 从本机 Kimi Code 会话暂存导出，"
                    "口径见 `logs/raw/README.md`。\n\n")
            f.write("| 项 | 数 |\n|---|---|\n")
            for k in sorted(stat):
                f.write("| %s | %d |\n" % (k, stat[k]))
            f.write("| 合计 | %d |\n\n---\n\n" % len(evs))
            last_tag = None
            for ms, tag, kind, payload in evs:
                ts = datetime.datetime.fromtimestamp(ms / 1000).strftime("%H:%M:%S")
                if tag != "main" and tag != last_tag:
                    f.write("\n## 〔子 agent %s〕\n\n" % tag)
                last_tag = tag
                prefix = "[%s]" % ts
                if kind == "user":
                    f.write("\n### %s 用户指令\n\n%s\n\n"
                            % (prefix, clean(cut(redact(payload), MAX_USER))))
                elif kind == "reply":
                    f.write("\n**%s 助手回复**\n\n%s\n\n"
                            % (prefix, clean(cut(redact(payload), MAX_REPLY))))
                elif kind == "think":
                    f.write("\n> %s 思考（节选）: %s\n\n" % (prefix, clean(redact(payload))))
                elif kind == "tool":
                    name, a = payload
                    f.write("\n%s 工具 `%s` — %s\n\n"
                            % (prefix, name, clean(redact(fmt_args(name, a)))))
                elif kind == "res":
                    f.write("```\n%s\n```\n\n" % clean(redact(fmt_result(payload))))
        print("  写出 %s （%d 条）" % (path, len(evs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
