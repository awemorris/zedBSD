#!/usr/bin/env python3
"""WS025 repeated baseline on a disposable xHCI USB root image.
Derived from the maintained q086 native runner; source image is hash protected.
"""
import re
import json
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("storage_qemu",
    REPO / "plan/ws019/tests/run-storage-qemu.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
boot_spec = importlib.util.spec_from_file_location("memory_boot", Path(__file__).with_name("run-memory-boot.py"))
boot = importlib.util.module_from_spec(boot_spec)
boot_spec.loader.exec_module(boot)

def main():
    output = Path(sys.argv[1]).resolve()
    firmware, memory = sys.argv[2], sys.argv[3]
    output.relative_to(REPO / "plan/ws025/temp")
    output.mkdir(parents=True, exist_ok=False)
    source = REPO / "build/amd64/hdd-image.img"
    original = base.digest(source)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    str(source), str(output / "boot.img")], check=True)
    with source.open("rb") as stream:
        stream.seek(512)
        header = stream.read(512)
        assert header[:8] == b"EFI PART"
        lba, slots, width = struct.unpack_from("<QII", header, 72)
        assert 0 < slots <= 4096 and 128 <= width <= 4096
        stream.seek(lba * 512)
        entries = stream.read(slots * width)
    candidates = []
    for i in range(slots):
        entry = entries[i * width:(i + 1) * width]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        assert 34 <= first <= last < source.stat().st_size // 512
        if subprocess.run(["mdir", "-i", f"{source}@@{first * 512}",
                           "::/rootfs.img"], capture_output=True).returncode == 0:
            candidates.append(first)
    assert len(candidates) == 1
    root = REPO / "build/arch-images/amd64-ws025-high.ufs"
    subprocess.run(["mcopy", "-o", "-i",
        f"{output / 'boot.img'}@@{candidates[0] * 512}", str(root), "::/rootfs.img"], check=True)
    guest = boot.MemoryGuest(output, firmware, memory)
    try:
        guest.login()
        at = len(guest.text())
        guest.send("storage-exit high-memory")
        result, _ = guest.wait(r"root@[^\s]*:[^\n]*\$ ?$", at, 240)
        assert "HIGH GUEST PASS" in result and "storage-result-0" in result, result[-2000:]
        assert "HIGH GUEST FAIL" not in result
        maps = re.findall(r"HIGH MAP space=(\S+) va=(\S+) pa=(\d+) size=(\d+) attr=(\d+) result=(-?\d+)", result)
        assert maps and all(int(row[5]) == 0 for row in maps)
        last_map = {}
        last_free = {}
        for position, line in enumerate(result.splitlines()):
            match = re.search(r"HIGH MAP .* pa=(\d+).* result=0$", line)
            if match:
                last_map[int(match[1])] = position
            match = re.search(r"HIGH FREE pa=(\d+)$", line)
            if match:
                last_free[int(match[1])] = position
        assert last_map and all(last_free.get(pa, -1) > position for pa, position in last_map.items()), (last_map, last_free)
        spaces = {row[0] for row in maps}
        assert len(spaces) >= 2
        assert "HIGH UNMAP" in result
        if int(memory) >= 8192:
            assert all(int(row[2]) >= 2**32 for row in maps), maps
            assert "HIGH PROBE PASS min=4294967296" in guest.text()
        if int(memory) >= 2048:
            assert "HIGH PROBE PASS min=1073741824" in guest.text()
        at = len(guest.text())
        guest.send("sysctl hw.memory.stats")
        stats = guest.prompt(at)
        (output / "memory.txt").write_text(stats)
        summary = {"firmware": firmware, "memory_mib": int(memory),
                   "mapped_physical_min": min(int(row[2]) for row in maps),
                   "mapped_physical_max": max(int(row[2]) for row in maps),
                   "retired_physical_pages": len(last_map), "map_events": len(maps), "spaces": len(spaces), "cow_rounds": 4}
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary), flush=True)
    finally:
        if guest.proc.poll() is None:
            guest.proc.stdin.write("quit\n")
            guest.proc.stdin.flush()
            try:
                guest.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                guest.proc.kill()
                guest.proc.wait()
        guest.commands.close()
        guest.monitor.close()
        assert base.digest(source) == original
    assert base.digest(source) == original
    (output / "source.sha256").write_text(original + "  build/amd64/hdd-image.img\n")
    print("WS025 native high-memory PASS")

if __name__ == "__main__":
    main()
