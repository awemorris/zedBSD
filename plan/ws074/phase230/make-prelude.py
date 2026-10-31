#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Makes userland/desktop/libbrowser/bind/prelude-source.inc (from plan/ws074/phase230/prelude.js) and
prelude-net.inc (from plan/ws074/phase232/net.js), each a C string literal."""
import os

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))


def convert(source_path, output_name, variable):
    source = open(os.path.join(root, source_path), encoding="utf-8").read()
    # A line "// @include path" (relative to the repository) is replaced by that file's text.
    out = []
    for line in source.split("\n"):
        if line.startswith("// @include "):
            out.append(open(os.path.join(root, line[len("// @include "):].strip()), encoding="utf-8").read().rstrip("\n"))
        else:
            out.append(line)
    source = "\n".join(out)
    lines = []
    for line in source.split("\n"):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        lines.append('\t"%s\\n"' % escaped)
    text = "/* Made by plan/ws074/phase230/make-prelude.py from %s; edit that and run the script. */\n" % source_path
    text += "static const char %s[] =\n" % variable + "\n".join(lines) + ";\n"
    open(os.path.join(root, "userland/desktop/libbrowser/bind", output_name), "w", encoding="utf-8").write(text)


convert("plan/ws074/phase230/prelude.js", "prelude-source.inc", "prelude_source")
convert("plan/ws074/phase232/net.js", "prelude-net.inc", "prelude_net_source")
# The host build's dependency tracking sees only .c files, so prelude.c is touched to make it build again.
os.utime(os.path.join(root, "userland/desktop/libbrowser/bind/prelude.c"))
