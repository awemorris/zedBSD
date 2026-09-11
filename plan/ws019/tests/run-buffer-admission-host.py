#!/usr/bin/env python3
"""Reuse the current-code buffer fixture with concurrent clean admission pressure."""
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(REPO / "plan/ws019/temp")
source = (REPO / "plan/ws025/tests/run-buffer-run-host.py").read_text()
source = source.replace("repo/'plan/ws025/temp'", "repo/'plan/ws019/temp'")
source = source.replace("plan/ws025/tests/buffer-run-host.c",
                        "plan/ws019/tests/buffer-admission-host.c")
source = source.replace("src/kern/io-stats.c", "src/kern/io.c")
scope = dict(__file__=str(REPO / "plan/ws025/tests/run-buffer-run-host.py"), __name__="__main__")
exec(compile(source, "current-buffer-admission-runner", "exec"), scope)
