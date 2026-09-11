#!/usr/bin/env python3
"""Run and account for all 50 q086 stories, retaining every command and log."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--native", action="store_true", help="run fresh two-boot xHCI USB acceptance")
args = parser.parse_args()
output = args.output.resolve()
output.relative_to(REPO / "plan/ws018/temp")
output.mkdir(parents=True, exist_ok=False)
observed = set()
commands = []

def run(name, command, env=None):
    print(name, flush=True)
    commands.append({"name": name, "argv": command, "env": env or {}})
    (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    with (output / (name + ".log")).open("w") as log:
        result = subprocess.run(command, cwd=REPO, stdout=log,
            stderr=subprocess.STDOUT, env={**os.environ, **(env or {})})
    text = (output / (name + ".log")).read_text(errors="replace")
    if result.returncode:
        print(text[-4000:], flush=True)
        raise SystemExit(f"{name}: exit {result.returncode}")
    observed.update(int(x) for x in re.findall(r"^S(\d\d) PASS\b", text, re.M))
    return text

common = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
          "-ffunction-sections", "-fdata-sections", "-Iinclude", "-Iinclude/uapi",
          "-Isrc", "-I.", "-Dtid_t=int32_t", "-DUFS_AUDIT_CURRENT_DRIVER"]
for variant in ["ordinary", "sanitize"]:
    extra = [] if variant == "ordinary" else [
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    environment = {"ASAN_OPTIONS": os.environ.get("ASAN_OPTIONS", "detect_leaks=1"), "UBSAN_OPTIONS": "halt_on_error=1"}
    for label, files, flags in [
        ("owned", ["plan/ws004/tests/storage-owned-buffer-stories.c"], ["-pthread"]),
        ("bot", ["plan/ws004/tests/storage-bot-stories.c"], []),
        ("fat", ["plan/ws018/tests/storage-fat-stories.c",
                 "src/drivers/fs/fat.c"], ["-DZEDBSD_USER_ABI_LP64", "-Ilibc/include"]),
        ("ufs", ["plan/ws018/tests/storage-ufs-stories.c",
                 "src/kern/quota.c"],
                 ["-DZEDBSD_USER_ABI_LP64", "-Ilibc/include", "-pthread"])]:
        binary = output / f"{label}-{variant}"
        objects = []
        if label == "ufs":
            # ASan global registration retains the entire unrelated VFS
            # operation table; match the maintained UFS audit runner.
            if variant == "sanitize":
                flags = [*flags, "--param", "asan-globals=0"]
            thread = output / f"thread-{variant}.o"
            run(f"thread-{variant}-compile", ["cc","-std=c11","-O1","-g","-pthread", *extra,
                "-c","plan/ws018/tests/mount-thread-host.c","-o",str(thread)])
            objects.append(str(thread))
        run(f"{label}-{variant}-compile", [*common, *flags, *extra, *files, *objects,
            "src/kern/io.c",
            "-Wl,--gc-sections", "-o", str(binary)])
        run(f"{label}-{variant}", ["timeout","60s",str(binary)], environment)

run("syscall-cells", ["python3","plan/ws018/tests/run-storage-syscall-stories.py"])
run("claim", ["sh","plan/ws016/tests/run-backing-claim-test.sh"])
run("images", ["python3","plan/ws018/tests/storage-image-stories.py"])
for variant in ["ordinary","sanitize"]:
    text = run("wifi-" + variant, ["sh","plan/ws005/tests/run-wifi-stories.sh"],
               {"STORY_VARIANT": variant,
                "STORY_OUTPUT": str(REPO / "plan/ws005/temp" / (output.name + "-wifi"))})
    assert set(range(1,31)).issubset(set(map(int, re.findall(r"^story (\d+) PASS\b", text, re.M))))
observed.add(50)
if args.native:
    run("native-usb", ["python3","plan/ws018/tests/run-storage-native.py",
                       str(output / "native-usb"), "--usb"])
result = {f"S{i:02d}": "PASS" if i in observed else "UNRUN" for i in range(1,51)}
(output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
sources = subprocess.check_output(["git","ls-files","--cached","--others","--exclude-standard","src","include","tools/build"], cwd=REPO, text=True).splitlines()
with (output / "source.sha256").open("w") as log:
    for name in sorted(set(sources)):
        path = REPO / name
        if path.is_file():
            log.write(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + name + "\n")
(output / "source-deleted.json").write_text(json.dumps(sorted(
    name for name in set(sources) if not (REPO / name).exists()), indent=2) + "\n")
missing = sorted(set(range(1,51)) - observed)
print(f"q086: {50-len(missing)}/50 PASS; unrun={missing}", flush=True)
if missing:
    raise SystemExit(1)
