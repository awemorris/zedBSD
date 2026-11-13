# ws299: Debian and zedBSD side by side, from the logs of one measure-5330.sh run.
#   python3 -I plan/ws299/tests/compare.py LOGS DEBIAN_NAME ZEDBSD_NAME
# Reads LOGS/DEBIAN_NAME-MODE.txt (bench-debian.sh) and LOGS/ZEDBSD_NAME-MODE.txt (bench-zedbsd.sh), takes each
# step's rate in MB/s (10^6 bytes a second: dd's bytes over its time, /bin/time's real for zedBSD's dd, the line of
# patwrite, randread or directio), and prints one Markdown table of the items the acceptance names, with zedBSD's
# share of Debian's rate.  A missing step is "-".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import glob
import os
import re
import statistics
import sys

DD = re.compile(r"^(\d+) bytes .* copied, ([\d.]+) s")
PATWRITE = re.compile(r"patwrite mib=\d+ write_ms=\d+ sync_ms=\d+ MBps=(\d+)")
RANDREAD = re.compile(r"randread bs=\d+ qd=\d+ reads=\d+ MBps=([\d.]+)")
DIRECTIO = re.compile(r"directio (?:read|write) mib=\d+ bs_kib=\d+ ms=\d+ MBps=([\d.]+)")
STEP = re.compile(r"step (\S+)(?: bytes=(\d+))?")
TIME = re.compile(r"real ([\d.]+)")


def rates(logs, name):
    """Every step of every mode of one system: {step: MB/s}."""
    out = {}
    raw_bytes = 0
    raw_real = 0.0
    for path in sorted(glob.glob(os.path.join(logs, name + "-*.txt"))):
        if path.endswith(("-dmesg.txt", "-summary.md")):
            continue
        mode = os.path.basename(path)[len(name) + 1:-len(".txt")]
        found = {}
        step = None
        size = 0
        with open(path, errors="replace") as stream:
            for line in stream:
                line = line.rstrip("\n")
                match = STEP.match(line)
                if match:
                    step = match.group(1)
                    size = int(match.group(2) or 0)
                    continue
                if step is None:
                    continue
                for pattern in (PATWRITE, RANDREAD, DIRECTIO):
                    match = pattern.match(line)
                    if match:
                        found[step] = float(match.group(1))
                        step = None
                        break
                if step is None:
                    continue
                match = DD.match(line)
                if match and float(match.group(2)) > 0:
                    found.setdefault(step, int(match.group(1)) / float(match.group(2)) / 1e6)
                    step = None
                    continue
                match = TIME.match(line)
                if match:
                    real = float(match.group(1))
                    if re.match(r"raw\d+$|rawsync$", step):
                        raw_bytes += size
                        raw_real += real
                    if real > 0 and size > 0:
                        found[step] = size / real / 1e6
                    step = None
        # Each step by its mode ("read:chunk0"), and by its name alone from the first mode that has it.
        for key, value in found.items():
            out[mode + ":" + key] = value
            out.setdefault(key, value)
    if raw_real > 0:
        out["rawtotal"] = raw_bytes / raw_real / 1e6
    return out


def median_of(table, names):
    values = [table[name] for name in names if name in table]
    return statistics.median(values) if values else None


def main():
    logs, debian_name, zedbsd_name = sys.argv[1], sys.argv[2], sys.argv[3]
    debian = rates(logs, debian_name)
    zedbsd = rates(logs, zedbsd_name)
    chunks = ["chunk%d" % index for index in range(2, 16)]
    debian["chunk2-15"] = median_of(debian, ["b-" + name for name in chunks])
    zedbsd["chunk2-15"] = median_of(zedbsd, ["read:" + name for name in chunks])
    zedbsd["seqcold:chunk0-15"] = median_of(zedbsd, ["seqcold:chunk%d" % index for index in range(16)])
    debian["chunk0-15"] = median_of(debian, ["b-chunk%d" % index for index in range(16)])
    pairs = [
        ("raw の書き p7 4 GiB（fsync まで）", "p7-rawwrite", "p7-rawwrite"),
        ("raw の冷えた読み p7 4 GiB（cache 経由）", "p7-rawread", "p7-rawread"),
        ("file の書き p7 4 GiB（ext4 / UFS、fsync まで）", "p7-fswrite", "p7-fswrite"),
        ("file の冷えた読み p7 4 GiB", "p7-fsread", "p7-fsread"),
        ("direct の書き 1 MiB", "p7-directwrite", "p7-directwrite"),
        ("direct の読み 1 MiB", "p7-directread", "p7-directread"),
        ("direct の読み 4 MiB", "p7-directread4m", "p7-directread4m"),
        ("data 範囲の direct の読み 1 MiB", "data-directread", "data-directread"),
        ("乱読み 4 KiB direct QD1", "r-rand4k-qd1-hmb1", "rand4k-direct-qd1"),
        ("乱読み 4 KiB direct QD32", "r-rand4k-qd32-hmb1", "rand4k-direct-qd32"),
        ("乱読み 4 KiB direct QD32（thread ごとの open）", "r-rand4k-qd32-hmb1", "rand4k-direct-fdeach-qd32"),
        ("乱読み 4 KiB QD1（cache 経由）", "r-rand4k-qd1-cached", "rand4k-qd1"),
        ("乱読み 4 KiB QD32（zedBSD は cache 経由、Debian は direct）", "r-rand4k-qd32-hmb1", "rand4k-qd32"),
        ("（参考）乱読み 4 KiB QD32（両方 cache 経由。Debian は page cache が範囲の多くを持つ）", "r-rand4k-qd32-cached",
         "rand4k-qd32"),
        ("root の file 1 GiB の上書き（ext4 / UFS、sync まで）", "ext4-w1m", "ufsover"),
        ("root の file 1 GiB を新しく（ext4 / UFS、sync まで）", "ext4-w1m", "ufsnew"),
        ("root の file 1 GiB の冷えた読み", "ext4-r1m", "ufsread"),
        ("p6 の raw の書き 1 GiB（zedBSD は 256 MiB の dd 4 回と sync）", "b-w1m", "rawtotal"),
        ("p6 の raw の書き 1 GiB（両方 1 回の dd と sync）", "b-w1m", "rawone:raw1g"),
        ("data 範囲の raw の連続の読み chunk2〜15 の中央値（cache 経由）", "chunk2-15", "chunk2-15"),
        ("read mode（乱読みの後）の bs 64 KiB 128 MiB", "b-bs64k", "read:bs64k"),
        ("同じ、bs 1 MiB 128 MiB", "b-bs1m", "read:bs1m"),
        ("同じ、bs 4 MiB 128 MiB", "b-bs4m", "read:bs4m"),
        ("同じ、chunk0 256 MiB", "b-chunk0", "read:chunk0"),
        ("同じ、chunk1 256 MiB", "b-chunk1", "read:chunk1"),
        ("seqcold（乱読みの前）の bs 64 KiB 128 MiB", "b-bs64k", "seqcold:bs64k"),
        ("同じ、bs 1 MiB 128 MiB", "b-bs1m", "seqcold:bs1m"),
        ("同じ、bs 4 MiB 128 MiB", "b-bs4m", "seqcold:bs4m"),
        ("同じ、chunk0 256 MiB", "b-chunk0", "seqcold:chunk0"),
        ("同じ、chunk1 256 MiB", "b-chunk1", "seqcold:chunk1"),
        ("同じ、chunk0〜15 の中央値", "chunk0-15", "seqcold:chunk0-15"),
        ("residue（乱読み QD32 の後）の 1 GiB（Debian は cache を捨てた後の chunk の中央値）", "chunk0-15", "residue:res-seq"),
    ]
    print("| 項目（5330） | Debian %s MB/s | zedBSD %s MB/s | 比 |" % (debian_name, zedbsd_name))
    print("| --- | --- | --- | --- |")
    for label, debian_step, zedbsd_step in pairs:
        left = debian.get(debian_step)
        right = zedbsd.get(zedbsd_step)
        share = "-"
        if left and right:
            share = "%.0f%%" % (right * 100.0 / left)
        print("| %s | %s | %s | %s |" % (label, "%.1f" % left if left else "-", "%.1f" % right if right else "-",
                                         share))
    for mode in ("read", "seqcold"):
        first = zedbsd.get(mode + ":chunk0")
        last = zedbsd.get(mode + ":chunk15")
        if first and last:
            print("")
            print("zedBSD %s の raw の連続の読みの最初の 256 MiB と最後の 256 MiB の比（受け入れ 1）: %.2f（%.1f / %.1f MB/s）" % (
                mode, min(first, last) / max(first, last), first, last))


main()
