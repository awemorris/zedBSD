#!/usr/bin/env python3
"""Build the zedBSD SUSv4/XSI system-interface audit table."""

import argparse
import csv
import importlib.util
from pathlib import Path
import re


FIELDS = [
    "order", "category", "header", "symbol", "kind", "option_codes",
    "implementation_status", "header_evidence", "definition_evidence",
    "associated_surface", "test_evidence", "review_status",
    "review_findings", "fix_status", "owner_plan", "standard_url",
]

HEADER_OVERRIDES = {
    "_longjmp": "setjmp.h", "_setjmp": "setjmp.h",
    "_tolower": "ctype.h", "_toupper": "ctype.h",
}

PLAN_APIS = {
    "P01": """_longjmp _setjmp _tolower _toupper toascii basename dirname
        memccpy hcreate hdestroy hsearch insque lfind lsearch remque tdelete
        tfind tsearch twalk""",
    "P02": """j0 j1 jn signgam y0 y1 yn a64l l64a drand48 erand48
        jrand48 lcong48 lrand48 mrand48 nrand48 seed48 srand48 initstate
        random setstate srandom setkey crypt encrypt swab""",
    "P03": """ftw nftw dbm_clearerr dbm_close dbm_delete dbm_error dbm_fetch
        dbm_firstkey dbm_nextkey dbm_open dbm_store realpath tempnam""",
    "P04": """pthread_getconcurrency pthread_setconcurrency killpg sighold
        sigignore siginterrupt sigpause sigrelse sigset daylight getdate
        strptime timezone ulimit gethostid setpgrp utimes ftok""",
    "P05": "fmtmsg closelog openlog setlogmask syslog sync",
    "P10": "getpriority getrusage setpriority nice getitimer setitimer mknod mknodat",
    "P11": "msgctl msgget msgrcv msgsnd",
    "P12": "semctl semget semop",
    "P13": "shmat shmctl shmdt shmget",
}
PLAN_APIS = {plan: set(names.split()) for plan, names in PLAN_APIS.items()}
OWNER = {symbol: plan for plan, names in PLAN_APIS.items() for symbol in names}

ASSOCIATED = {
    "getdate": "getdate_err",
    "hsearch": "ENTRY ACTION",
    "ftw": "FTW_* callback types",
    "nftw": "FTW struct and FTW_* flags",
    "dbm_open": "DBM datum DBM_INSERT DBM_REPLACE",
    "fmtmsg": "MM_* constants",
    "syslog": "LOG_* constants and mask macros",
    "getitimer": "struct itimerval ITIMER_*",
    "getrusage": "struct rusage RUSAGE_*",
    "msgctl": "struct msqid_ds IPC_* MSG_*",
    "semctl": "struct semid_ds sembuf commands",
    "shmctl": "struct shmid_ds SHM_* SHMLBA",
}

REVIEW_FIXES = {
    "ftw": "fixed broken-symlink classification and callback isolation",
    "nftw": "fixed broken-symlink classification and callback isolation",
    "hsearch": "fixed invalid ACTION validation",
    "dbm_open": "fixed truncate and failed-flush cleanup paths",
    "dbm_close": "fixed failed-flush descriptor cleanup",
    "realpath": "fixed component-wise symbolic-link resolution and loop limit",
    "nice": "fixed valid -1 result versus errno handling",
    "syslog": "fixed LOG_PID to report the calling process",
    "getitimer": "fixed 32-bit atomic portability and concurrent countdown",
    "setitimer": "fixed 32-bit atomic portability and concurrent update",
    "msgget": "fixed backing queue limits and close-time persistence",
    "msgsnd": "fixed queue-full blocking and serialized queue mutation",
    "msgrcv": "fixed type selection, blocking, truncation, and ENOMSG behavior",
    "msgctl": "fixed removal of queue synchronization state",
    "semget": "fixed named backing persistence across close",
    "semctl": "fixed named backing persistence across close",
    "semop": "fixed named backing persistence across close",
    "shmat": "fixed SHM_RND address alignment",
}


def test_evidence(owner):
    if owner in {"P01", "P02"}:
        return "susv4-libc-host-test; amd64/pcat/pc98 builds"
    if owner in {"P03", "P04", "P05", "P10"}:
        return "SUSV4-XSI QEMU test; ILP32/LP64 header/ABI checks; multiarch builds"
    if owner in {"P11", "P12", "P13"}:
        return "SUSV4-XSI QEMU System V IPC test; multiarch builds"
    return "existing POSIX tests; ILP32/LP64 header checks; multiarch builds"


def load_posix_audit(repo):
    path = repo / "tools/posix-kernel-api-audit.py"
    spec = importlib.util.spec_from_file_location("posix_audit", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def header_has_symbol(repo, header, symbol):
    path = repo / "libc/include" / header
    if not path.exists():
        return False, ""
    text = path.read_text(encoding="utf-8", errors="replace")
    pattern = r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"(?![A-Za-z0-9_])"
    return re.search(pattern, text) is not None, str(path.relative_to(repo))


def definition_evidence(repo, header, symbol, defined, kind):
    if kind == "data" and symbol in defined:
        return "build symbol"
    if symbol in defined:
        return "build symbol"
    header_path = repo / "libc/include" / header
    if header_path.exists():
        text = header_path.read_text(encoding="utf-8", errors="replace")
        if re.search(r"(?m)^\s*#\s*define\s+" + re.escape(symbol) +
                     r"(?:\s|\()", text):
            return str(header_path.relative_to(repo)) + " (macro)"
    roots = [repo / "libc", repo / "userland/base/libc", repo / "src/softfloat",
             repo / "src/softfloat"]
    pattern = re.compile(r"(?m)^[A-Za-z_][^;{}]*\b" + re.escape(symbol) +
                         r"\s*\([^;]*\)\s*\{")
    data_pattern = re.compile(r"(?m)^(?!\s*(?:extern|static)\b)[A-Za-z_]"
                              r"[^;{}()]*\b" + re.escape(symbol) +
                              r"(?:\s*=|\s*;)")
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*.c"):
            contents = path.read_text(encoding="utf-8", errors="replace")
            if (path.stem == symbol or pattern.search(contents) or
                    (kind == "data" and data_pattern.search(contents))):
                return str(path.relative_to(repo))
    return ""


def category(header):
    if header in {"sys/ipc.h", "sys/msg.h", "sys/sem.h", "sys/shm.h"}:
        return "sysv-ipc"
    if header in {"signal.h", "pthread.h", "sys/resource.h", "sys/time.h",
                  "ulimit.h", "unistd.h"}:
        return "process-system"
    if header in {"ftw.h", "ndbm.h", "sys/stat.h"}:
        return "filesystem"
    if header in {"math.h"}:
        return "math"
    return "libc"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sus-root", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    sus_root = args.sus_root.resolve()
    base = load_posix_audit(repo)
    defined = base.collect_defined_symbols(repo)
    rows = []
    seen = set()
    for page, symbol, kind in base.read_index(sus_root):
        if symbol in seen:
            continue
        header, codes, _ = base.synopsis_info(sus_root, page, symbol)
        if "XSI" not in codes:
            continue
        other_options = (set(codes) & base.OPTIONAL_CODES) - {"XSI"}
        if other_options:
            continue
        header = HEADER_OVERRIDES.get(symbol, header)
        seen.add(symbol)
        declared, header_path = header_has_symbol(repo, header, symbol)
        evidence = definition_evidence(repo, header, symbol, defined, kind)
        implemented = declared and bool(evidence)
        status = "implemented" if implemented else (
            "declared-only" if declared else "missing")
        owner = OWNER.get(symbol, "implemented-baseline")
        finding = REVIEW_FIXES.get(symbol)
        rows.append({
            "category": category(header), "header": f"<{header}>",
            "symbol": symbol, "kind": kind,
            "option_codes": " ".join(codes),
            "implementation_status": status,
            "header_evidence": header_path if declared else "",
            "definition_evidence": evidence,
            "associated_surface": ASSOCIATED.get(symbol, ""),
            "test_evidence": test_evidence(owner), "review_status": "reviewed",
            "review_findings": finding or "none",
            "fix_status": "fixed" if finding else "not-needed",
            "owner_plan": owner,
            "standard_url":
                f"https://pubs.opengroup.org/onlinepubs/9699919799/functions/{page}",
        })
    rows.sort(key=lambda row: (row["header"], row["symbol"].lower()))
    for index, row in enumerate(rows, 1):
        row["order"] = index
    catalog = set(row["symbol"] for row in rows)
    assigned = set(OWNER)
    if len(rows) != 134:
        raise SystemExit(f"expected 134 XSI interfaces, found {len(rows)}")
    if not assigned <= catalog:
        raise SystemExit(f"planned APIs absent from catalog: {sorted(assigned-catalog)}")
    if len(assigned) != 101:
        raise SystemExit(f"expected 101 assigned APIs, found {len(assigned)}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    counts = {}
    for row in rows:
        value = row["implementation_status"]
        counts[value] = counts.get(value, 0) + 1
    print(f"wrote {len(rows)} SUSv4/XSI interfaces to {args.output}")
    for name in sorted(counts):
        print(f"{name}: {counts[name]}")


if __name__ == "__main__":
    main()
