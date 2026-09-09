#!/usr/bin/env python3
"""Check the public shell's status contract against POSIX command outcomes."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]

with tempfile.TemporaryDirectory(prefix="zedbsd-shell-status-") as directory:
    out = Path(directory)
    shell = out / "sh"
    sources = [ROOT / "userland/base/sh" / (name + ".c") for name in
               ("main", "builtins", "lexer", "expand", "glob", "vars",
                "arithmetic", "alias")]
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-int-conversion", "-I" + str(ROOT),
                    "-I" + str(ROOT / "include/uapi"),
                    "-I" + str(ROOT / "userland/base/libedit"),
                    *map(str, sources),
                    str(ROOT / "userland/base/libedit/readline.c"),
                    "-lm", "-o", str(shell)], check=True)
    blocked = out / "blocked"
    blocked.write_text("exit 0\n")
    blocked.chmod(0o644)
    # A PATH-selected cp must execute, not an obsolete shell-local copy loop.
    selected_cp = out / "cp"
    selected_cp.write_text("#!/bin/sh\nprintf 'external-cp:%s\\n' \"$1\"\nexit 23\n")
    selected_cp.chmod(0o755)
    cases = [
        ("/bin/sh -c 'exit 42'; echo $?", 0, "42\n"),
        ("/bin/sh -c 'exit 42'", 42, ""),
        ("/bin/true | /bin/sh -c 'exit 7'", 7, ""),
        ("/bin/sh -c 'exit 7' | /bin/cat", 0, ""),
        ("missing-zedbsd-status-command", 127, ""),
        (str(blocked), 126, ""),
        ("/bin/sh -c 'exit 42'; exit", 42, ""),
        ("/bin/false && /bin/true; echo $?", 0, "1\n"),
        ("/bin/true || /bin/false; echo $?", 0, "0\n"),
        ("eval \"/bin/sh -c 'exit 42'\"", 42, ""),
        ("command /bin/sh -c 'exit 7'", 7, ""),
        ("exec missing-zedbsd-status-command", 127, ""),
        ("/bin/sh -c 'kill -TERM $$'; echo $?", 0, "143\n"),
        ("/bin/sh -c 'exit 42'; /bin/true; echo $?", 0, "0\n"),
        ("/bin/cat < /missing-zedbsd-status-path", 1, ""),
        ("/bin/true & echo $?", 0, None),
        (f"PATH={out} cp --report-file=proof", 23, "external-cp:--report-file=proof\n"),
    ]
    for command, code, output in cases:
        result = subprocess.run([str(shell), "-c", command],
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == code, (command, result)
        assert output is None or result.stdout == output, (command, result)
    scripts = [
        ("/bin/sh -c 'exit 42'\n\n# keep status\necho $?\n/bin/sh -c 'exit 7'\n", 7, "42\n"),
        ("/bin/false\necho continued\n", 0, "continued\n"),
        ("/bin/false\nexit\n", 1, ""),
        ("echo 'unterminated\necho must-not-run\n", 2, ""),
    ]
    for text, code, output in scripts:
        script = out / "script"
        script.write_text(text)
        result = subprocess.run([str(shell), str(script)],
                                text=True, capture_output=True, timeout=10)
        assert (result.returncode, result.stdout) == (code, output), result
    print(json.dumps({"result": "PASS", "cases": len(cases) + len(scripts)}))
