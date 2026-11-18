#!/usr/bin/env python3
# Compiles the oracle's compute shaders (oracle/*.comp) and the graphics checks' shaders (graphics/*.vert, *.geom, *.frag)
# to SPIR-V and writes them as C arrays into userland/tests/vkcompute/oracle-shaders.h and graphics-shaders.h, so
# the build needs no shader compiler.  The SPIR-V is
# validated with spirv-val first.  Run it again after changing a shader:
#
#   python3 -I userland/tests/vkcompute/oracle/mkheader.py
#
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(os.path.dirname(HERE), "oracle-shaders.h")
GRAPHICS_OUTPUT = os.path.join(os.path.dirname(HERE), "graphics-shaders.h")
GLSLC = ["glslc", "--target-env=vulkan1.0"]


def version(command):
    """The first line of a tool's --version, for the header's note."""
    text = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    return text.splitlines()[0].strip()


def compile_shader(path):
    """The SPIR-V words of one shader, validated (nothing is written to disk)."""
    binary = subprocess.run(GLSLC + ["-o", "-", path], check=True, capture_output=True).stdout
    subprocess.run(["spirv-val", "--target-env", "vulkan1.0", "-"], input=binary, check=True)
    return list(struct.unpack("<%dI" % (len(binary) // 4), binary))


def write_header(output, guard, what, sources, prefix):
    """Writes the SPIR-V of the sources (paths under the vkcompute directory) as C arrays into output."""
    lines = [
        "/*",
        " * zedBSD",
        " * Copyright (C) 2026 Awe Morris",
        " *",
        " * SPDX-License-Identifier: Zlib",
        " */",
        "",
        "/*",
        " * %s as SPIR-V, written by oracle/mkheader.py from" % what,
        " * the sources named below with \"glslc --target-env=vulkan1.0\" (%s)," % version(["glslc", "--version"]),
        " * so the build needs no shader compiler.  Do not edit; run the script again.",
        " */",
        "",
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
        "#include <stdint.h>",
    ]
    for source in sources:
        base = os.path.basename(source)
        name = prefix + base.replace(".comp", "").replace(".", "_").replace("-", "_")
        words = compile_shader(os.path.join(os.path.dirname(HERE), source))
        lines.append("")
        lines.append("/* %s */" % source)
        lines.append("static const uint32_t %s[] = {" % name)
        for start in range(0, len(words), 4):
            row = ", ".join("0x%08xU" % word for word in words[start:start + 4])
            lines.append("\t" + row + ",")
        lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(output, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print("mkheader: %d shaders -> %s" % (len(sources), output))


def main():
    oracle = sorted("oracle/" + name for name in os.listdir(HERE) if name.endswith(".comp"))
    write_header(OUTPUT, "VKCOMPUTE_ORACLE_SHADERS_H", "The oracle's compute shaders", oracle, "vkcompute_oracle_")
    graphics_dir = os.path.join(os.path.dirname(HERE), "graphics")
    graphics = sorted("graphics/" + name for name in os.listdir(graphics_dir)
                      if name.endswith(".vert") or name.endswith(".frag") or name.endswith(".geom"))
    write_header(GRAPHICS_OUTPUT, "VKCOMPUTE_GRAPHICS_SHADERS_H", "The graphics checks' shaders", graphics,
                 "vkcompute_graphics_")
    return 0


if __name__ == "__main__":
    sys.exit(main())
