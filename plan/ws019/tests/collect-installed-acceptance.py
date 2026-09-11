#!/usr/bin/env python3
"""Verify terminal runtime evidence and rerun the installer fault-model matrix."""
import json
from pathlib import Path
import runpy
import subprocess
import sys

HERE = Path(__file__).resolve().parent
F = runpy.run_path(str(HERE / "run-formatter-qemu.py"))
REPO = F["REPO"]


def main():
    out, install, conflict, boot, selection, auxiliary = map(lambda p: Path(p).resolve(), sys.argv[1:])
    for path in (out, install, conflict, boot, selection, auxiliary):
        path.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    runs = [install, conflict, boot, selection, auxiliary]
    records = [json.loads((p / "result.json").read_text()) for p in runs]
    installed, refused, booted, selected, other = records
    assert installed["cases"] == ["cancel", "install", "rerun"]
    assert refused["result"] == "PASS installer conflict continuation"
    assert refused["accepted_run"] == str(install)
    assert booted["result"].startswith("PASS installed boot/persistence;")
    assert booted["cases"] == ["initial boot/write/halt", "cold boot/reboot/persistence/halt"]
    assert selected["cases"] == ["absent", "duplicate"]
    assert other["result"] == "PASS installed selection" and other["cases"] == ["auxiliary"]
    production = F["digest"](REPO / "build/amd64/hdd-image.img")
    target = F["digest"](install / "gpt.img")
    for record in records:
        assert record["production_sha256_after"] == production
    for record in (installed, refused):
        assert record["protected_before"] == record["protected_after"]
        assert record["payload_before"] == record["payload_after"]
        assert record["vars_before"] == record["vars_after"]
    assert refused["accepted_target_before"] == refused["accepted_target_after"] == target
    for record in (booted, selected, other):
        assert record["source_sha256"] == record["source_sha256_after"] == target
    result = {"result": "FAIL consolidated acceptance", "runtime_records": {}, "model_runs": []}
    for path in runs:
        result["runtime_records"][str(path)] = F["digest"](path / "result.json")
    try:
        for mode in ([], ["-j0"]):
            for name in ("admission", "selection", "discovery", "destination", "transaction", "copy"):
                command = [str(REPO / "build/NoctLang/build-static/noct"), *mode,
                           "--path=userland/base/zedinst", str(HERE / f"installer-{name}.noct")]
                completed = subprocess.run(command, cwd=REPO, text=True, capture_output=True, timeout=120)
                label = name + ("-interpreter" if mode else "-jit")
                (out / (label + ".log")).write_text(completed.stdout + completed.stderr)
                assert completed.returncode == 0 and "PASS" in completed.stdout, label + ": " + completed.stdout + completed.stderr
                result["model_runs"].append(label)
        result["cells"] = {
            "1": "PASS public install/cancel/rerun; protected hashes and operation-scoped NVRAM unchanged",
            "2": "PASS NVMe-only fallback boot, generated PARTUUID, overlay/swap, cold restart and reboot persistence",
            "3": "PASS absent config visible refusal; duplicate warning and deterministic first selection; discovery model refuses ambiguity",
            "4": "PASS actual one-byte conflict refusal plus host admission/publication/copy fault models; not a physical power-cut claim",
            "5": "PASS auxiliary USB FAT ignored as OTHER DISK; two-NVMe kernel limitation retained as BUG-016/p050",
            "6": "PASS generated UFS overlay upper and ZEDSWAP2 activation; p004 source/generation contract copies only loader/kernel/immutable rootfs"
        }
        result["candidate"] = str(install / "gpt.img")
        result["candidate_sha256"] = target
        result["production_sha256"] = production
        result["result"] = "PASS consolidated p005 acceptance"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
