#!/usr/bin/env python3
"""WS025 repeated baseline on a disposable xHCI USB root image.
Derived from the maintained q086 native runner; source image is hash protected.
"""
import hashlib
import json
import re
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("storage_qemu",
    REPO / "plan/ws019-installation/tests/run-storage-qemu.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

class USBGuest(base.Guest):
    def __init__(self, output):
        self.output = output
        self.log = output / "guest.log"
        self.deadline = time.monotonic() + 600
        self.commands = (output / "commands.log").open("w")
        self.monitor = (output / "qemu.log").open("w")
        args = ["qemu-system-x86_64", "-machine", "q35", "-m", next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--memory=")), "512"), "-smp", "4",
                "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                "-drive", f"if=pflash,format=raw,file={output / 'vars.fd'}",
                "-device", "qemu-xhci,id=xhci",
                "-drive", f"if=none,id=boot,format=raw,file={output / 'boot.img'}",
                "-device", "usb-storage,bus=xhci.0,drive=boot,bootindex=1",
                "-nic", "none", "-display", "none", "-serial", "none",
                "-debugcon", f"file:{self.log}", "-monitor", "stdio"]
        self.commands.write(repr(args) + "\n")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
                                     stderr=subprocess.STDOUT, text=True)

def main():
    output = Path(sys.argv[1]).resolve()
    output.relative_to(REPO / "plan/ws025-io-memory-cache/temp")
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
    root = REPO / "build/arch-images/amd64-ws025.ufs"
    subprocess.run(["mcopy", "-o", "-i",
        f"{output / 'boot.img'}@@{candidates[0] * 512}", str(root), "::/rootfs.img"], check=True)
    # Record the backing-file alignment: 2 KiB FAT clusters can double 4 KiB RMW writes.
    boot_lba = candidates[0]
    with (output / "boot.img").open("rb") as stream:
        stream.seek(boot_lba * 512)
        bpb = stream.read(512)
    assert struct.unpack_from("<H", bpb, 11)[0] == 512
    sectors_per_cluster = bpb[13]
    reserved = struct.unpack_from("<H", bpb, 14)[0]
    fat_sectors = struct.unpack_from("<H", bpb, 22)[0] or struct.unpack_from("<I", bpb, 36)[0]
    root_sectors = (struct.unpack_from("<H", bpb, 17)[0] * 32 + 511) // 512
    data_lba = boot_lba + reserved + bpb[16] * fat_sectors + root_sectors
    if "--align-data" in sys.argv:
        # Relocate only the disposable backing file; consume one cluster to align it.
        image_spec = f"{output / 'boot.img'}@@{boot_lba * 512}"
        chain = subprocess.run(["mshowfat", "-i", image_spec, "::/data.img"],
                               text=True, capture_output=True, check=True).stdout
        first_cluster = int(re.search(r"<(\d+)", chain)[1])
        offset = ((data_lba + (first_cluster - 2) * sectors_per_cluster) * 512) % 4096
        if offset:
            assert sectors_per_cluster * 512 == 2048 and offset == 2048
            subprocess.run(["mcopy", "-i", image_spec, "::/data.img",
                            str(output / "data-source.img")], check=True)
            subprocess.run(["mdel", "-i", image_spec, "::/data.img"], check=True)
            (output / "alignment-pad.bin").write_bytes(bytes(2048))
            subprocess.run(["mcopy", "-i", image_spec, str(output / "alignment-pad.bin"),
                            "::/ws025-pad.bin"], check=True)
            subprocess.run(["mcopy", "-i", image_spec, str(output / "data-source.img"),
                            "::/data.img"], check=True)
    layout = {}
    for name in ("data.img", "rootfs.img"):
        chain = subprocess.run(["mshowfat", "-i", f"{output / 'boot.img'}@@{boot_lba * 512}",
                                "::/" + name], text=True, capture_output=True, check=True).stdout
        cluster = int(re.search(r"<(\d+)", chain)[1])
        first_lba = data_lba + (cluster - 2) * sectors_per_cluster
        layout[name] = {"first_cluster": cluster, "first_lba": first_lba,
                        "offset_in_4k": (first_lba * 512) % 4096,
                        "cluster_bytes": sectors_per_cluster * 512, "chain": chain.strip()}
    (output / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
    if "--align-data" in sys.argv:
        assert layout["data.img"]["offset_in_4k"] == 0, layout["data.img"]
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", output / "vars.fd")
    base.fixture(output / "gpt.img", True)
    base.fixture(output / "mbr.img", False)
    guest = USBGuest(output)
    try:
        guest.login()
        at = len(guest.text())
        guest.send("storage-exit io-baseline")
        result, _ = guest.wait(r"root@[^\s]*:[^\n]*\$ ?$", at, 240)
        assert "BASELINE PASS" in result and "storage-result-0" in result, result[-2000:]
        assert "BASELINE FAIL" not in result
        samples = re.findall(r"^SAMPLE mode=(\d+) n=(\d+) ns=(\d+)(.*)$", result, re.M)
        assert len(samples) == 202
        assert {(int(m), int(n)) for m, n, _, _ in samples} == {
            (m, n) for m in range(2) for n in range(101)}
        (output / "samples.tsv").write_text("\n".join(
            "\t".join(row) for row in samples) + "\n")
        print("\n".join(line for line in result.splitlines()
            if line.startswith(("MEM ", "QUANTILE ", "BASELINE "))), flush=True)
    finally:
        guest.stop()
    assert base.digest(source) == original
    (output / "source.sha256").write_text(original + "  build/amd64/hdd-image.img\n")
    print("WS025 native baseline PASS")

if __name__ == "__main__":
    main()
