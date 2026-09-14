#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 bsp_audio_test.c / bsp_voice_link.c 推到云机并接进构建。

上传走 base64 + stdin 喂给 ssh（避免 80KB 命令行参数被打断）。
远端做两件事：解出两个 .c，然后跑 remote_patch_vlink.py 改构建配置。
"""
import base64
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = "/root/vela/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src"
SSH_WRAP = os.path.join(os.environ.get("TEMP", "/tmp"), "xm-ssh.sh")

FILES = [
    (os.path.join(HERE, "board", "bsp_audio_test.c"), "bsp_audio_test.c"),
    (os.path.join(HERE, "board", "bsp_voice_link.c"), "bsp_voice_link.c"),
    (os.path.join(HERE, "remote_patch_vlink.py"), "patch_vlink.py"),
]


def main():
    out = ["set -e", "cd %s" % SRC]

    for local, name in FILES:
        with open(local, "rb") as f:
            raw = f.read()
        b64 = base64.b64encode(raw).decode("ascii")
        # 40 字符一行，避免某些 shell/heredoc 实现的行长限制
        wrapped = "\n".join(b64[i:i + 76] for i in range(0, len(b64), 76))
        out.append("echo '=== 上传 %s (%d B) ==='" % (name, len(raw)))
        out.append("cat > /tmp/up_%s.b64 <<'B64EOF'" % name)
        out.append(wrapped)
        out.append("B64EOF")
        if name.endswith(".c"):
            out.append("base64 -d /tmp/up_%s.b64 > %s && wc -c %s" % (name, name, name))
        else:
            out.append("base64 -d /tmp/up_%s.b64 > /tmp/%s && wc -c /tmp/%s"
                       % (name, name, name))

    out.append("python3 /tmp/patch_vlink.py")
    script = "\n".join(out) + "\n"

    p = subprocess.run(["sh", SSH_WRAP], input=script.encode("utf-8"),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(p.stdout.decode("utf-8", "replace"))
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
