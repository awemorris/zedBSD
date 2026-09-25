#!/usr/bin/env python3
# ws035-p035: the ABI values that move from the C library headers to include/uapi.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Record and compare the ABI values of the headers that move to include/uapi.

usage: python3 plan/ws035/tests/uapi-value-ledger.py --record OUT.json [--tree DIR]
       python3 plan/ws035/tests/uapi-value-ledger.py --check BASE.json [--out NEW.json]

--tree reads the headers (include/libc, include) of another tree, for
example an export of the commit before the move (git archive HEAD).

--record (run on the tree before the move) reads the C library headers of
plan/ws035/kcrt-design.md section 8 as a zedBSD program sees them
(-nostdinc -Iinclude/libc -Iinclude, the zedBSD target compiler) and records,
for both user ABIs (x86_64-unknown-zedbsd with KERN_USER_ABI_LP64 and
i386-unknown-zedbsd):

  - every object-like macro of those headers that is an integer constant
    expression: its value, its size and whether its type is signed;
  - every ioctl request number defined in include/uapi with _IO/_IOR/_IOW/_IOWR;
  - sizeof of the moved types, and sizeof/offsetof of the moved structures.

The values are measured by the compiler, not by reading the text: each entry is
one line of a generated translation unit whose constants are placed in a
section of the object, which is then read back.  An entry that is not a
constant (a string, a type, a pointer) fails to compile and is dropped from the
list at record time.

--check (run after the move) measures the same list twice:
  libc  the same view as --record; every entry must be unchanged.
  uapi  the include/uapi headers alone, the way the kernel reads them
        (-nostdlibinc -Iinclude); every entry the uapi headers define must
        have the value --record measured, and every entry of the kernel's
        list (REQUIRED_IN_UAPI) must be defined there.
Exits non-zero on any difference.
"""

import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
TREE = REPO
CLANG = REPO / "build/llvm/bin/clang"
OBJCOPY = REPO / "build/llvm/bin/llvm-objcopy"

ABIS = {
    "lp64": ["--target=x86_64-unknown-zedbsd", "-DKERN_USER_ABI_LP64"],
    "ilp32": ["--target=i386-unknown-zedbsd", "-m32"],
}

# The C library headers whose ABI definitions move (design section 8).
LIBC_HEADERS = [
    "errno.h", "sys/ioctl.h", "time.h", "sys/time.h", "sys/stat.h", "fcntl.h",
    "limits.h", "unistd.h", "stdio.h", "sys/mman.h", "sys/wait.h",
    "sys/statvfs.h", "sys/resource.h", "sys/mount.h", "sys/un.h", "sys/types.h",
]

# Macros of those headers that stay in the C library by design and are not
# ABI shared with the kernel; they are still measured in the libc view.
UAPI_HEADERS = [
    "uapi/errno.h", "uapi/ioctl.h", "uapi/time.h", "uapi/stat.h", "uapi/fcntl.h",
    "uapi/limits.h", "uapi/unistd.h", "uapi/mman.h", "uapi/wait.h",
    "uapi/statvfs.h", "uapi/resource.h", "uapi/mount.h", "uapi/un.h",
    "uapi/types.h",
]

TYPES = [
    "ssize_t", "off_t", "blkcnt_t", "blksize_t", "dev_t", "ino_t", "mode_t",
    "nlink_t", "uid_t", "gid_t", "pid_t", "id_t", "tid_t", "useconds_t",
    "suseconds_t", "reclen_t", "time_t", "clockid_t", "timer_t", "fsblkcnt_t",
    "fsfilcnt_t", "rlim_t", "idtype_t",
]

STRUCTS = {
    "struct timespec": ["tv_sec", "tv_nsec"],
    "struct itimerspec": ["it_interval", "it_value"],
    "struct timeval": ["tv_sec", "tv_usec"],
    "struct itimerval": ["it_interval", "it_value"],
    "struct stat": ["st_dev", "st_ino", "st_mode", "st_nlink", "st_uid", "st_gid",
                    "st_rdev", "st_size", "st_atim", "st_mtim", "st_ctim",
                    "st_blksize", "st_blocks"],
    "struct statvfs": ["f_bsize", "f_frsize", "f_blocks", "f_bfree", "f_bavail",
                       "f_files", "f_ffree", "f_favail", "f_fsid", "f_flag",
                       "f_namemax"],
    "struct rlimit": ["rlim_cur", "rlim_max"],
    "struct rusage": ["ru_utime", "ru_stime", "ru_maxrss", "ru_nivcsw"],
    "struct mount_args": ["size", "version", "fspec"],
    "struct sockaddr_un": ["sun_family", "sun_path"],
}

# Enumerators of those headers (not visible to #ifdef).
ENUMERATORS = ["P_ALL", "P_PID", "P_PGID"]

# Function-like macros measured at fixed arguments (the wait status decoders).
CALLS = [
    ("WIFEXITED", "0x0100"), ("WEXITSTATUS", "0x2a00"), ("WIFSIGNALED", "0x0009"),
    ("WTERMSIG", "0x0009"), ("WIFSTOPPED", "0x137f"), ("WSTOPSIG", "0x137f"),
    ("WIFCONTINUED", "0xffff"),
    ("S_ISREG", "0100644"), ("S_ISDIR", "0040755"), ("S_ISBLK", "0060600"),
    ("S_ISCHR", "0020600"), ("S_ISLNK", "0120777"), ("S_ISSOCK", "0140777"),
    ("S_ISFIFO", "0010600"),
]

# What the kernel takes from these headers (uapi-move-ledger.json) and must
# therefore find in include/uapi after the move.
REQUIRED_IN_UAPI = [
    "E2BIG", "EACCES", "EINVAL", "ENOTSUP", "EWOULDBLOCK", "EPROTO",
    "O_RDONLY", "O_CLOFORK", "AT_FDCWD", "F_OFD_SETLKW", "FD_CLOFORK", "F_UNLCK",
    "NAME_MAX", "PATH_MAX", "SSIZE_MAX", "GETENTROPY_MAX", "ARG_MAX", "HOST_NAME_MAX",
    "SEEK_SET", "SEEK_CUR", "SEEK_END", "SEEK_DATA", "SEEK_HOLE",
    "F_OK", "R_OK", "W_OK", "X_OK",
    "MAP_ANONYMOUS", "MAP_FIXED_NOREPLACE", "MS_INVALIDATE", "PROT_EXEC", "MADV_FREE",
    "KERN_MOUNT_ARGS_VERSION", "MNT_NOSUID", "MNT_RDONLY",
    "PRIO_USER", "RUSAGE_CHILDREN", "S_IFMT", "S_IWOTH", "S_ISVTX",
    "ST_LOCAL", "ST_RDONLY", "UNIX_PATH_MAX", "P_PGID", "WNOWAIT", "WCONTINUED",
    "CLOCK_MONOTONIC", "CLOCK_REALTIME", "TIMER_ABSTIME", "UTIME_NOW", "UTIME_OMIT",
    "ITIMER_PROF", "KERN_IOC_INOUT",
    "sizeof(struct stat)", "sizeof(struct timespec)", "sizeof(struct timeval)",
    "sizeof(struct statvfs)", "sizeof(struct rlimit)", "sizeof(struct rusage)",
    "sizeof(struct mount_args)", "sizeof(struct sockaddr_un)",
    "sizeof(struct itimerspec)", "sizeof(struct itimerval)",
    "sizeof(off_t)", "sizeof(ssize_t)", "sizeof(time_t)", "sizeof(tid_t)",
    "sizeof(reclen_t)", "sizeof(idtype_t)", "P_ALL", "P_PID",
    "WIFEXITED(0x0100)", "S_ISDIR(0040755)",
]

# include/uapi headers that describe one architecture only and refuse the other.
UAPI_SKIP = {"reg.h"}

DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(\(?)", re.M)
IOCTL = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+_IO[RW]*\s*\(", re.M)


def macro_names():
    names = []
    for header in LIBC_HEADERS:
        text = (TREE / "include/libc" / header).read_text(encoding="utf-8")
        for name, paren in DEFINE.findall(text):
            if paren or name.startswith(("LIBC_", "KERN_STDLIB")) or name == "errno":
                continue
            names.append(name)
    return sorted(set(names))


def ioctl_names():
    names = []
    for path in sorted((TREE / "include/uapi").rglob("*.h")):
        names += IOCTL.findall(path.read_text(encoding="utf-8"))
    return sorted(set(names))


def entries():
    items = []
    for name in macro_names():
        items.append({"key": name, "expr": name, "macro": name})
    for name in ioctl_names():
        items.append({"key": name, "expr": name, "macro": name, "ioctl": True})
    for name in ENUMERATORS:
        items.append({"key": name, "expr": name})
    for macro, argument in CALLS:
        items.append({"key": "%s(%s)" % (macro, argument), "expr": "%s(%s)" % (macro, argument), "macro": macro})
    for name in TYPES:
        items.append({"key": "sizeof(%s)" % name, "expr": "sizeof(%s)" % name, "type": name})
    for name, fields in STRUCTS.items():
        items.append({"key": "sizeof(%s)" % name, "expr": "sizeof(%s)" % name, "type": name})
        for field in fields:
            items.append({"key": "offsetof(%s, %s)" % (name, field),
                          "expr": "__builtin_offsetof(%s, %s)" % (name, field), "type": name})
    return items


def source(items, headers, guard_macros):
    lines = ["/* generated by plan/ws035/tests/uapi-value-ledger.py */"]
    for header in headers:
        lines.append("#include <%s>" % header)
    lines.append("#define LEDGER_VALUE(x) (unsigned long long)(x)")
    lines.append("#define LEDGER_SIZE(x) (unsigned long long)sizeof((x) + 0)")
    lines.append("#define LEDGER_SIGNED(x) (unsigned long long)((__typeof__((x) + 0))-1 < 0)")
    lines.append('__attribute__((used, section(".uapi_ledger"))) const unsigned long long uapi_value_ledger[] = {')
    first = len(lines) + 1
    for item in items:
        expr = item["expr"]
        if guard_macros and "macro" in item:
            lines.append("#ifdef %s" % item["macro"])
            lines.append("1ULL, LEDGER_VALUE(%s), LEDGER_SIZE(%s), LEDGER_SIGNED(%s)," % (expr, expr, expr))
            lines.append("#else")
            lines.append("0ULL, 0ULL, 0ULL, 0ULL,")
            lines.append("#endif")
        else:
            lines.append("1ULL, LEDGER_VALUE(%s), LEDGER_SIZE(%s), LEDGER_SIGNED(%s)," % (expr, expr, expr))
    lines.append("};")
    return "\n".join(lines) + "\n", first


def compile_view(items, view, abi, work):
    if view == "libc":
        flags = ["-nostdinc", "-Iinclude/libc", "-Iinclude"]
        headers = LIBC_HEADERS + ["uapi/%s" % p.name for p in sorted((TREE / "include/uapi").glob("*.h"))
                                  if p.name not in UAPI_SKIP]
        guard = False
    else:
        flags = ["-nostdlibinc", "-Iinclude"]
        headers = [h for h in UAPI_HEADERS if (TREE / "include" / h).exists()]
        headers += ["uapi/%s" % p.name for p in sorted((TREE / "include/uapi").glob("*.h"))
                    if "uapi/%s" % p.name not in headers and p.name not in UAPI_SKIP]
        guard = True
    text, _ = source(items, headers, guard)
    c_file = work / ("%s-%s.c" % (view, abi))
    o_file = work / ("%s-%s.o" % (view, abi))
    b_file = work / ("%s-%s.bin" % (view, abi))
    c_file.write_text(text)
    cmd = [str(CLANG)] + ABIS[abi] + flags + ["-std=c11", "-ffreestanding", "-c", str(c_file), "-o", str(o_file)]
    r = subprocess.run(cmd, cwd=TREE, capture_output=True, text=True, timeout=120)
    return r, cmd, o_file, b_file, text


def failing_lines(stderr, c_file):
    bad = set()
    for m in re.finditer(re.escape(str(c_file)) + r":(\d+):\d+: error:", stderr):
        bad.add(int(m.group(1)))
    return bad


def measure(items, view, abi, work, prune):
    """Returns ({key: [value, size, signed] or None}, kept items)."""
    for _ in range(20):
        r, cmd, o_file, b_file, text = compile_view(items, view, abi, work)
        if r.returncode == 0:
            break
        if not prune:
            raise SystemExit("%s/%s: compile failed:\n%s\n%s" % (view, abi, " ".join(cmd), r.stderr[:4000]))
        bad = failing_lines(r.stderr, work / ("%s-%s.c" % (view, abi)))
        if not bad:
            raise SystemExit("%s/%s: compile failed without a line:\n%s" % (view, abi, r.stderr[:4000]))
        lines = text.split("\n")
        drop = set()
        for number in bad:
            line = lines[number - 1]
            for item in items:
                if "LEDGER_VALUE(%s)" % item["expr"] in line:
                    drop.add(item["key"])
        if not drop:
            raise SystemExit("%s/%s: cannot map errors to entries:\n%s" % (view, abi, r.stderr[:4000]))
        items = [i for i in items if i["key"] not in drop]
    else:
        raise SystemExit("%s/%s: did not converge" % (view, abi))
    subprocess.run([str(OBJCOPY), "-O", "binary", "--only-section=.uapi_ledger", str(o_file), str(b_file)],
                   check=True, timeout=60)
    data = b_file.read_bytes()
    values = struct.unpack("<%dQ" % (len(data) // 8), data)
    result = {}
    for index, item in enumerate(items):
        present, value, size, signed = values[index * 4:index * 4 + 4]
        result[item["key"]] = [value, size, signed] if present else None
    return result, items


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--record")
    parser.add_argument("--check")
    parser.add_argument("--out")
    parser.add_argument("--tree")
    args = parser.parse_args()
    global TREE
    if args.tree:
        TREE = Path(args.tree).resolve()
    work = Path(tempfile.mkdtemp(prefix="uapi-ledger-"))
    try:
        if args.record:
            ledger = {"tool": "plan/ws035/tests/uapi-value-ledger.py", "abis": {}}
            items = None
            for abi in ABIS:
                values, kept = measure(entries() if items is None else items, "libc", abi, work, True)
                items = kept
                ledger["abis"][abi] = values
            # an entry must be measurable in both ABIs
            common = set(ledger["abis"]["lp64"]) & set(ledger["abis"]["ilp32"])
            for abi in ABIS:
                ledger["abis"][abi] = {k: v for k, v in ledger["abis"][abi].items() if k in common}
            ledger["entries"] = [i for i in items if i["key"] in common]
            Path(args.record).write_text(json.dumps(ledger, indent=1) + "\n")
            print("uapi value ledger recorded: %d entries x %d ABIs" % (len(ledger["entries"]), len(ABIS)))
            return 0
        base = json.loads(Path(args.check).read_text())
        items = base["entries"]
        status = 0
        report = {"libc": {}, "uapi": {}}
        for abi in ABIS:
            expected = base["abis"][abi]
            libc, _ = measure(items, "libc", abi, work, False)
            uapi, _ = measure(items, "uapi", abi, work, False)
            report["libc"][abi] = libc
            report["uapi"][abi] = uapi
            libc_diff = [k for k in expected if libc.get(k) != expected[k]]
            uapi_diff = [k for k in expected if uapi.get(k) is not None and uapi[k] != expected[k]]
            missing = [k for k in REQUIRED_IN_UAPI if uapi.get(k) is None]
            defined = sum(1 for k in expected if uapi.get(k) is not None)
            for k in libc_diff:
                print("%s libc changed: %s %s -> %s" % (abi, k, expected[k], libc.get(k)))
            for k in uapi_diff:
                print("%s uapi differs: %s %s != %s" % (abi, k, uapi[k], expected[k]))
            for k in missing:
                print("%s uapi missing: %s" % (abi, k))
            print("%s: entries=%d libc_changed=%d uapi_defined=%d uapi_differs=%d required_missing=%d"
                  % (abi, len(expected), len(libc_diff), defined, len(uapi_diff), len(missing)))
            if libc_diff or uapi_diff or missing:
                status = 1
        if args.out:
            Path(args.out).write_text(json.dumps(report, indent=1) + "\n")
        print("uapi value ledger check:", "PASS" if status == 0 else "FAIL")
        return status
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
