#!/usr/bin/env python3
"""Verify exact logical accounting with sparse files and arbitrary filename bytes."""
from pathlib import Path
import os
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix="zedbsd-native-measure-") as temporary:
    base = os.fsencode(temporary)
    source = base + b"/source"
    os.mkdir(source, 0o700)
    names = [b"ordinary", b"white space\nnewline", b"raw-\xff"]
    for index, name in enumerate(names):
        with open(source + b"/" + name, "wb") as stream:
            stream.write(b"x" * (index + 1))
    os.link(source + b"/ordinary", source + b"/hardlink")
    os.symlink(b"/etc/passwd", source + b"/outside-link")
    with open(source + b"/sparse", "wb") as stream:
        stream.truncate(5 * 1024 * 1024)
    paths = [source] + [source + b"/" + name for name in os.listdir(source)]
    expected = f"MEASURE {sum(os.lstat(path).st_size for path in paths)} {len(paths)}"
    for mode in [[], ["-j0"]]:
        workspace = Path(temporary) / ("census-jit" if not mode else "census-interpreter")
        workspace.mkdir(mode=0o700)
        result = subprocess.run([str(REPO / "build/NoctLang/build-static/noct"), *mode,
            "--path=" + str(REPO / "userland/base/zedinst"),
            str(REPO / "plan/ws019-installation/tests/installer-nativemeasure.noct"),
            os.fsdecode(source), str(workspace)], check=True, capture_output=True, text=True)
        assert result.stdout.strip() == expected, (result.stdout, expected)
print("native measurement PASS: sparse bytes, symlinks, hard links and arbitrary filenames; JIT/interpreter")
