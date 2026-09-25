#!/usr/bin/env python3
"""Compile proposal copies with existing independent guest fixture collaborators."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


def run(command, log):
    started = time.monotonic()
    with log.open("w") as stream:
        try:
            process = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=120)
            return {"pass": process.returncode == 0, "exit": process.returncode,
                    "seconds": time.monotonic() - started, "log": str(log)}
        except subprocess.TimeoutExpired:
            return {"pass": False, "timeout": True, "seconds": time.monotonic() - started, "log": str(log)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proposal", type=Path, help="historical proposal copy; defaults to current production checkout")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    args.proposal = (args.proposal or repo).resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    overlay = output / "overlay"
    target = overlay / "src/drivers/gpu/venus"
    target.mkdir(parents=True, exist_ok=True)
    for path in (repo / "src/drivers/gpu/venus").iterdir():
        if path.is_file():
            selected = args.proposal / "src/drivers/gpu/venus" / path.name
            shutil.copyfile(selected if selected.exists() else path, target / path.name)
    names = ["plan/ws014/tests/venus-transport.c", "plan/ws014/tests/venus-transport-peer.inc",
             "plan/ws014/tests/venus-backend.c", "plan/ws014/tests/venus-backend-linked.c",
             "plan/tools/venus-console.c"]
    for name in names:
        destination = overlay / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / name, destination)
    results = []
    for case, name in (("transport", names[0]), ("backend", names[3])):
        for profile in ("ordinary", "sanitized"):
            executable = output / (case + "-" + profile)
            command = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-Wdeclaration-after-statement", "-I" + str(repo / "include"),
                       "-idirafter", str(repo / "libc/include"), str(overlay / name), "-o", str(executable)]
            if profile == "sanitized":
                command += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            result = {"case": case, "profile": profile, "pass": False}
            result["compile"] = run(command, output / (case + "-" + profile + ".compile.log"))
            if result["compile"]["pass"]:
                result["run"] = run([str(executable)], output / (case + "-" + profile + ".run.log"))
                result["pass"] = result["run"]["pass"]
            results.append(result)
            print(json.dumps(result), flush=True)
    hashes = {str(p.relative_to(args.proposal)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (args.proposal / "src/drivers/gpu/venus").iterdir() if p.is_file()}
    report = {"scope": "isolated source-copy verification; mocked PCI/native peers; existing regressions retained",
              "source_sha256": hashes, "results": results, "pass": all(r["pass"] for r in results)}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
