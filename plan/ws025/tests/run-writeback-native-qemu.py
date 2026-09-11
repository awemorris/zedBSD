#!/usr/bin/env python3
"""WS025 file-cache acceptance on a disposable xHCI USB root image.
Derived from the maintained q086 native runner; source image is hash protected.
"""
import argparse
import hashlib
import json
import os
import re
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools/build"))
from ufs_format import create
spec = importlib.util.spec_from_file_location("storage_qemu",
    REPO / "plan/ws019/tests/run-storage-qemu.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

class USBGuest(base.Guest):
    def text(self):
        text = super().text()
        if re.search(r"^(?:fatal:|panic:|kernel panic)", text, re.M | re.I):
            raise RuntimeError("guest fatal error: " + text[-1200:])
        return text

    def __init__(self, output, expect_imod=None):
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
                "-drive", f"if=none,id=wb,format=raw,file={output / 'writeback.img'}",
                "-device", ("nvme,drive=wb,serial=WS025WRITEBACK" if "--nvme" in sys.argv else "usb-storage,bus=xhci.0,drive=wb" + (",removable=on" if "--media-recovery" in sys.argv else "")),
                "-nic", "none", "-display", "none", "-serial", "none",
                "-debugcon", f"file:{self.log}", "-monitor", "stdio"]
        if expect_imod is not None:
            args += ["-qmp", f"unix:{output / 'imod.sock'},server=on,wait=off"]
        self.commands.write(repr(args) + "\n")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
                                     stderr=subprocess.STDOUT, text=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--image", type=Path, default=REPO / "build/amd64/hdd-image.img")
    parser.add_argument("--root-image", type=Path,
                        default=REPO / "build/arch-images/amd64-ws025-writeback.ufs")
    parser.add_argument("--expect-imod", type=int, choices=range(65536), metavar="0..65535")
    options, _ = parser.parse_known_args()
    source = options.image.resolve()
    root = options.root_image.resolve()
    if not source.is_file() or not root.is_file():
        parser.error("source and workload root images must exist")
    output = options.output.resolve()
    output.relative_to(REPO / "plan/ws025/temp")
    output.mkdir(parents=True, exist_ok=False)
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
    read_root = None
    if "--readahead" in sys.argv:
        read_root = output / "read-source"
        read_root.mkdir()
        payload = bytes(i % 251 for i in range(1024 * 1024))
        for name in ("read-sequential", "read-random"):
            (read_root / name).write_bytes(payload)
    profile = "journal-snapshot" if "--journal" in sys.argv else "ordinary"
    if "--media-recovery" in sys.argv:
        assert "--nvme" not in sys.argv and "--shutdown" not in sys.argv
        if read_root is None:
            read_root = output / "media-root"
            read_root.mkdir()
        (read_root / "media-marker").write_text("WS025 ORIGINAL MEDIUM\n")
        replacement_root = output / "replacement-root"
        replacement_root.mkdir()
        (replacement_root / "media-marker").write_text("WS025 REPLACEMENT MEDIUM\n")
        (output / "replacement.img").write_bytes(create(64 * 1024 * 1024, root=replacement_root, profile=profile))
    (output / "writeback.img").write_bytes(create(64 * 1024 * 1024, root=read_root, profile=profile))
    (output / "filesystem-profile.json").write_text(json.dumps({"profile": profile}) + "\n")
    capture = None
    if options.expect_imod is not None:
        imod_spec = importlib.util.spec_from_file_location("ws025_imod_capture",
            Path(__file__).with_name("run-imod-qemu.py"))
        imod = importlib.util.module_from_spec(imod_spec)
        imod_spec.loader.exec_module(imod)
        capture = imod.capture
    evidence = {"source": str(source), "source_sha256": original,
                "root_image": str(root), "root_sha256": base.digest(root),
                "expected_imod": options.expect_imod, "completed": False}
    guest = USBGuest(output, options.expect_imod)
    try:
        guest.login()
        if capture is not None:
            evidence["imod_before"] = capture(output / "imod.sock", options.expect_imod)
        commands = [
            "mkdir /ws025-wb",
            "mount -t ufs /dev/sdb /ws025-wb",
            "sysctl vfs.writeback.stats",
            "sysctl vfs.writeback.control=/ws025-wb:on",
            "writeback-native",
            "umount /ws025-wb",
            "mount -t ufs /dev/sdb /ws025-wb",
            "writeback-native verify",
            "writeback-native unmount",
            "mount -t ufs /dev/sdb /ws025-wb",
            "writeback-native verify",
            "umount /ws025-wb",
            "sysctl vfs.writeback.stats",
        ]
        if "--readahead" in sys.argv:
            commands += ["mount -t ufs /dev/sdb /ws025-wb", "readahead-native prepare",
                         "umount /ws025-wb", "mount -t ufs /dev/sdb /ws025-wb",
                         ("readahead-native sequential-baseline" if "--readahead-baseline" in sys.argv else "readahead-native sequential"), "umount /ws025-wb",
                         "mount -t ufs /dev/sdb /ws025-wb", "readahead-native random",
                         "umount /ws025-wb", "sysctl vfs.readahead.stats"]
        if "--shutdown" in sys.argv:
            commands += ["mount -t ufs /dev/sdb /ws025-wb", "writeback-native shutdown"]
        results = {}
        costs = {}
        for command in commands:
            at = len(guest.text())
            if "--nvme" in sys.argv:
                command = command.replace("/dev/sdb", "/dev/nvme0n1")
            # Host QEMU user+system CPU includes vCPUs/device emulation, plus
            # command typing/waiting. It is not guest process CPU accounting.
            def cpu_ticks():
                fields = Path(f"/proc/{guest.proc.pid}/stat").read_text().rsplit(")", 1)[1].split()
                return int(fields[11]) + int(fields[12])
            cpu_before = cpu_ticks()
            wall_before = time.monotonic_ns()
            guest.send("storage-exit " + command)
            result, _ = guest.wait(r"root@[^\s]*:[^\n]*\$ ?$", at, 240)
            costs[command] = dict(wall_ns=time.monotonic_ns() - wall_before,
                qemu_cpu_ns=(cpu_ticks() - cpu_before) * 1000000000 // os.sysconf("SC_CLK_TCK"))
            (output / "command-costs.json").write_text(json.dumps(costs, indent=2) + "\n")
            print(result, flush=True)
            assert "storage-result-0" in result, result[-4000:]
            assert "WRITEBACK FAIL" not in result
            assert "READ FAIL" not in result
            if command.startswith("readahead-native "):
                assert ("READ PREPARE PASS" if command.endswith("prepare") else "READ MEASURE PASS") in result
            if command == "writeback-native":
                assert "WRITEBACK NATIVE PASS" in result
                assert "WRITEBACK BATCH" in result and "WRITEBACK AGE" in result
            if command == "writeback-native verify":
                assert "WRITEBACK VERIFY PASS" in result
            if command == "writeback-native unmount":
                assert "WRITEBACK UNMOUNT PASS" in result
            results[command] = "PASS"
        if "--media-recovery" in sys.argv:
            # Change equal-capacity media behind the same configured USB device.
            at = len(guest.text())
            guest.proc.stdin.write(f"eject -f wb\nchange wb {output / 'replacement.img'} raw\n")
            guest.proc.stdin.flush()
            guest.wait(r"usb-storage: sdb blocks=", at, 60)
            guest.run("mount -t ufs /dev/sdb /ws025-wb")
            guest.run("cat /ws025-wb/media-marker", expected="WS025 REPLACEMENT MEDIUM")
            guest.run("cat /ws025-wb/media-marker", expected="WS025 REPLACEMENT MEDIUM")
            results["idle same-capacity medium replacement"] = "PASS"
            at = len(guest.text())
            guest.proc.stdin.write(f"eject -f wb\nchange wb {output / 'writeback.img'} raw\n")
            guest.proc.stdin.flush()
            guest.run("sleep 3")
            stale = guest.run("cat /ws025-wb/media-marker", status=1)
            assert "WS025 REPLACEMENT MEDIUM" not in stale
            assert "WS025 ORIGINAL MEDIUM" not in stale
            assert "usb-storage: sdb blocks=" not in guest.text()[at:]
            results["mounted old cache rejected without rebinding"] = "PASS"
        if "--shutdown" in sys.argv:
            at = len(guest.text())
            guest.send("poweroff")
            result, _ = guest.wait(r"WRITEBACK SHUTDOWN HALT PASS", at, 180)
            print(result, flush=True)
            assert "WRITEBACK SHUTDOWN STORAGE CLEAN" in result
            assert "final system action failed" not in result
            results["shutdown storage/device boundary"] = "PASS"
        if capture is not None and "--shutdown" not in sys.argv:
            evidence["imod_after"] = capture(output / "imod.sock", options.expect_imod)
        evidence["completed"] = True
    finally:
        guest.stop()
        evidence["source_unchanged"] = base.digest(source) == original
        (output / "campaign.json").write_text(json.dumps(evidence, indent=2) + "\n")
    if "--shutdown" in sys.argv:
        checker_spec = importlib.util.spec_from_file_location("ufs_checker", REPO / "tools/build/check-ufs-image.py")
        checker = importlib.util.module_from_spec(checker_spec)
        checker_spec.loader.exec_module(checker)
        fs = checker.UFS((output / "writeback.img").read_bytes(), runtime=True)
        content = fs.read_file(fs.lookup("/content"))
        assert content[3:67] == bytes([0xcd]) * 64
        assert content[8193:8195] == bytes([0x9a, 0x5b])
        results["shutdown disk contents"] = "PASS"
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    assert base.digest(source) == original
    (output / "source.sha256").write_text(original + "  " + str(source) + "\n")
    print("WS025 native writeback PASS")

if __name__ == "__main__":
    main()
