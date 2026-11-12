#!/usr/bin/env python3
# ws333: reads the processor's registers on the 5330's Debian (through /dev/cpu/N/msr, as root, after
# "modprobe msr"), to set beside what zedBSD reports for the same machine.  Only reads.
#   msr-debian.py info              the package C-state control, the limit reasons, the power limit, per CPU
#   msr-debian.py idle SECONDS      the package's and CPU 0's C-state residency and the package power
#   msr-debian.py load SECONDS      each CPU's delivered MHz and busy share, the limit reasons, the package power
# Registers (Intel SDM Vol. 4 rev. 093): CEH MSR_PLATFORM_INFO [15:8] (Table 2-29); E2H MSR_PKG_CST_CONFIG_CONTROL
# (Table 2-35); 10H IA32_TIME_STAMP_COUNTER, E7H IA32_MPERF, E8H IA32_APERF (Table 2-2); 606H, 610H, 611H
# MSR_RAPL_POWER_UNIT, MSR_PKG_POWER_LIMIT, MSR_PKG_ENERGY_STATUS (Table 2-20); 60DH, 3F8H, 3F9H, 3FAH the package
# C2, C3, C6, C7 residency, 3FCH, 3FDH, 3FEH the core C3, C6, C7 residency (Table 2-20); 630H, 631H, 632H the package
# C8, C9, C10 residency (Table 2-31, not listed for this model: read when the register answers); 64FH
# MSR_CORE_PERF_LIMIT_REASONS (Table 2-39); 1B1H IA32_PACKAGE_THERM_STATUS (Table 2-2).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import glob
import os
import struct
import sys
import time

PACKAGE = [("PC2", 0x60D), ("PC3", 0x3F8), ("PC6", 0x3F9), ("PC7", 0x3FA), ("PC8", 0x630), ("PC9", 0x631),
           ("PC10", 0x632)]
CORE = [("C3", 0x3FC), ("C6", 0x3FD), ("C7", 0x3FE)]
REASONS = ["PROCHOT", "THERMAL", "", "", "RSR", "RATL", "VR_THERMALERT", "VR_TDC", "OTHER", "", "PL1", "PL2",
           "MAX_TURBO", "TURBO_ATTEN"]


def cpus():
    return sorted(int(p.rsplit("/", 1)[1][3:]) for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*"))


def rd(cpu, msr):
    try:
        fd = os.open("/dev/cpu/%d/msr" % cpu, os.O_RDONLY)
    except OSError:
        return None
    try:
        return struct.unpack("<Q", os.pread(fd, 8, msr))[0]
    except OSError:
        return None
    finally:
        os.close(fd)


def reasons(value):
    if value is None:
        return "-"
    now = [n for i, n in enumerate(REASONS) if n and value >> i & 1]
    log = [n for i, n in enumerate(REASONS) if n and value >> (i + 16) & 1]
    return "0x%08x now=%s log=%s" % (value & 0xffffffff, ",".join(now) or "-", ",".join(log) or "-")


def base_mhz():
    value = rd(0, 0xCE)
    return None if value is None else (value >> 8 & 0xff) * 100


def energy_unit():
    value = rd(0, 0x606)
    return None if value is None else 1.0 / (1 << (value >> 8 & 0x1f))


def info():
    print("base_mhz %s" % base_mhz())
    limit = rd(0, 0x610)
    unit = rd(0, 0x606)
    if limit is not None and unit is not None:
        watt = 1.0 / (1 << (unit & 0xf))
        print("pkg_power_limit 0x%016x pl1=%.2f W enabled=%d pl2=%.2f W enabled=%d lock=%d" % (
            limit, (limit & 0x7fff) * watt, limit >> 15 & 1, (limit >> 32 & 0x7fff) * watt, limit >> 47 & 1,
            limit >> 63 & 1))
    print("package_therm_status %s" % ("-" if rd(0, 0x1B1) is None else "0x%016x" % rd(0, 0x1B1)))
    print("limit_reasons %s" % reasons(rd(0, 0x64F)))
    for cpu in cpus():
        value = rd(cpu, 0xE2)
        if value is None:
            print("cpu %d cst_config -" % cpu)
            continue
        print("cpu %d cst_config 0x%016x limit=%d lock=%d c3_demotion=%d c1_demotion=%d c3_undemotion=%d "
              "c1_undemotion=%d" % (cpu, value, value & 0xf, value >> 15 & 1, value >> 25 & 1, value >> 26 & 1,
                                    value >> 27 & 1, value >> 28 & 1))


def idle(seconds):
    names = PACKAGE + CORE
    before = {n: rd(0, m) for n, m in names}
    tsc0 = rd(0, 0x10)
    e0 = rd(0, 0x611)
    time.sleep(seconds)
    after = {n: rd(0, m) for n, m in names}
    tsc1 = rd(0, 0x10)
    e1 = rd(0, 0x611)
    unit = energy_unit()
    if None not in (e0, e1, unit):
        print("package %.3f W" % (((e1 - e0) & 0xffffffff) * unit / seconds))
    for n, _ in names:
        if before[n] is None or after[n] is None or tsc1 is None or tsc0 is None:
            print("state %s -" % n)
            continue
        scope = "cpu0" if n.startswith("C") else "package"
        print("state %s %s percent=%.1f" % (scope, n, 100.0 * (after[n] - before[n]) / (tsc1 - tsc0)))


def load(seconds):
    every = cpus()
    unit = energy_unit()
    base = base_mhz()
    first = {c: (rd(c, 0x10), rd(c, 0xE7), rd(c, 0xE8)) for c in every}
    e0 = rd(0, 0x611)
    time.sleep(seconds)
    last = {c: (rd(c, 0x10), rd(c, 0xE7), rd(c, 0xE8)) for c in every}
    e1 = rd(0, 0x611)
    if None not in (e0, e1, unit):
        print("package %.3f W" % (((e1 - e0) & 0xffffffff) * unit / seconds))
    print("limit_reasons %s" % reasons(rd(0, 0x64F)))
    for c in every:
        if None in first[c] + last[c] or base is None:
            print("cpu %d -" % c)
            continue
        dt, dm, da = (last[c][i] - first[c][i] for i in range(3))
        mhz = base * da // dm if dm > 0 else 0
        busy = 100.0 * dm / dt if dt > 0 else 0.0
        print("cpu %d mhz=%d busy=%.1f" % (c, mhz, busy))


def main():
    if len(sys.argv) < 2:
        print(__doc__ or "usage: msr-debian.py info | idle SECONDS | load SECONDS")
        return 2
    if sys.argv[1] == "info":
        info()
    elif sys.argv[1] == "idle":
        idle(int(sys.argv[2]))
    elif sys.argv[1] == "load":
        load(int(sys.argv[2]))
    else:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
