#!/usr/bin/env python3
"""Public GPT initialization; blank/existing media, immutable non-table bytes."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct
import sys
import uuid
import zlib

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]
SECTORS = 1048576
DISK_GUID = "78190000-aaaa-4aaa-8aaa-bbbbbbbbbbbb"
ESP_GUID = "78190000-1111-4111-8111-222222222222"
ROOT_GUID = "78190000-3333-4333-8333-444444444444"
ESP_TYPE = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
ROOT_TYPE = "516e7cb6-6ecf-11d6-8ff8-00022d09712b"
ROOT_START = 264192
ROOT_COUNT = SECTORS - 33 - ROOT_START
SPEC = f"{DISK_GUID} 2048 262144 {ESP_TYPE} {ESP_GUID} ESP {ROOT_START} {ROOT_COUNT} {ROOT_TYPE} {ROOT_GUID} root"


def body_digest(path):
    digest = hashlib.sha256()
    remaining = path.stat().st_size - (34 + 33) * 512
    with path.open("rb") as stream:
        stream.seek(34 * 512)
        while remaining:
            data = stream.read(min(remaining, 1048576))
            assert data
            digest.update(data)
            remaining -= len(data)
    return digest.hexdigest()


def inspect(path):
    with path.open("rb") as stream:
        def read(lba, size):
            stream.seek(lba * 512)
            data = stream.read(size)
            assert len(data) == size
            return data
        mbr = read(0, 512)
        assert mbr[510:] == b"\x55\xaa" and mbr[450] == 0xee
        assert struct.unpack_from("<II", mbr, 454) == (1, SECTORS - 1)
        tables = []
        for lba in (1, SECTORS - 1):
            header = bytearray(read(lba, 512))
            assert header[:8] == b"EFI PART"
            revision, length, crc, reserved = struct.unpack_from("<IIII", header, 8)
            assert (revision, length, reserved) == (0x10000, 92, 0)
            struct.pack_into("<I", header, 16, 0)
            assert zlib.crc32(header[:92]) == crc
            assert struct.unpack_from("<QQQQ", header, 24) == (lba, SECTORS - 1 if lba == 1 else 1, 34, SECTORS - 34)
            assert str(uuid.UUID(bytes_le=bytes(header[56:72]))) == DISK_GUID
            table_lba, slots, stride, crc = struct.unpack_from("<QIII", header, 72)
            assert (table_lba, slots, stride) == (2 if lba == 1 else SECTORS - 33, 128, 128)
            entries = read(table_lba, 16384)
            assert zlib.crc32(entries) == crc and entries[256:] == bytes(16384 - 256)
            for index, (kind, guid, start, count, name) in enumerate([
                    (ESP_TYPE, ESP_GUID, 2048, 262144, "ESP"),
                    (ROOT_TYPE, ROOT_GUID, ROOT_START, ROOT_COUNT, "root")]):
                entry = entries[index * 128:(index + 1) * 128]
                assert str(uuid.UUID(bytes_le=entry[:16])) == kind
                assert str(uuid.UUID(bytes_le=entry[16:32])) == guid
                assert struct.unpack_from("<QQQ", entry, 32) == (start, start + count - 1, 0)
                assert entry[56:].decode("utf-16-le").rstrip("\0") == name
            tables.append(entries)
        assert tables[0] == tables[1]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    mode = sys.argv[2]
    assert mode in ("blank", "existing")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    if mode == "existing":
        F["create_nvme"](disk, sectors=SECTORS)
    else:
        with disk.open("xb") as stream:
            stream.truncate(SECTORS * 512)
    result.update(mode=mode, target_before=F["digest"](disk), body_before=body_digest(disk), result="FAIL diskpart init")
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        records = guest.run("/sbin/diskpart --machine list")
        parent = re.search(r"^device\s+nvme0n1\s+(\d+)\s+0\s+0\s+512\s+1048576\s+0$", records, re.M)
        assert parent, records
        registration = parent[1]
        if mode == "existing":
            guest.run("mkdir /run/init-busy")
            guest.run("mount -t fat -o ro nvme0n1p1 /run/init-busy")
            guest.run("/sbin/diskpart init nvme0n1 " + SPEC, "No partition metadata written", status=1)
            guest.run("umount /run/init-busy")
        guest.run("/sbin/diskpart init sda " + SPEC, "No partition metadata written", status=1)
        for response, status in [("NO", 1), ("ERASE nvme0n1:" + registration, 0)]:
            if mode == "blank" and status == 0:
                empty_start = len(guest.text())
                guest.send("/sbin/diskpart init nvme0n1 " + DISK_GUID + "; echo storage-result-$?")
                guest.wait("to initialize:", empty_start, 120)
                guest.send(response)
                guest.command_status(empty_start, 0)
                guest.run("/sbin/diskpart --machine show nvme0n1", r"table\s+gpt\s+0\s+0")
                result["empty_GPT"] = "PASS"
            start = len(guest.text())
            guest.send("/sbin/diskpart init nvme0n1 " + SPEC + "; echo storage-result-$?")
            guest.wait("to initialize:", start, 120)
            guest.send(response)
            text = guest.command_status(start, status)
            if status:
                assert "No partition metadata written" in text
                assert F["digest"](disk) == result["target_before"]
                result["cancel_unchanged"] = True
            else:
                assert "GPT initialized, flushed, verified" in text
        records = guest.run("/sbin/diskpart --machine list")
        for slot, start, count in [(1, 2048, 262144), (2, ROOT_START, ROOT_COUNT)]:
            assert re.search(rf"^device\s+nvme0n1p{slot}\s+\d+\s+{registration}\s+4\s+512\s+{count}\s+{start}$", records, re.M), records
        guest.run("/sbin/diskpart --machine show nvme0n1", r"table\s+gpt\s+0\s+2")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS diskpart init " + mode
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["target_after"] = F["digest"](disk)
        result["body_after"] = body_digest(disk)
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if result["body_before"] != result["body_after"] or result["production_sha256"] != result["production_after"]:
            result["result"] = "FAIL unrelated bytes changed"
        try:
            if result["result"].startswith("PASS"):
                inspect(disk)
                result["independent_GPT"] = "PASS"
        except BaseException as error:
            result["result"] = "FAIL independent GPT inspection"
            result["inspection_failure"] = str(error)
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS diskpart init " + mode, result
    print(result["result"])


if __name__ == "__main__":
    main()
