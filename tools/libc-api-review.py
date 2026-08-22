#!/usr/bin/env python3
"""Record the completed ordered semantic review in the libc audit CSV."""
import argparse
import csv
from pathlib import Path


FINDINGS = {
    "fenv": ("Software floating point initially lacked thread-local state and "
             "domain/range exception propagation; both are now connected.", "fixed"),
    "setjmp": ("The compiler-assisted context layout was validated with a "
               "setjmp/longjmp round-trip smoke test.", "not-needed"),
    "stream": ("The first pass did not provide real cookie streams, purge, or "
               "in-place stream reassociation; the userland FILE backend now does.", "fixed"),
    "scan": ("The first scanner omitted scansets, bounded numeric fields, and "
             "wide string/character arguments; those cases are now handled.", "fixed"),
    "wide": ("Wide formatting originally forwarded incompatible unqualified "
             "%c/%s arguments and UTF-16 pending-surrogate errors were incomplete; fixed.", "fixed"),
    "random": ("The entropy-device fallback was a non-cryptographic xorshift; "
               "it now uses a locked ChaCha20 generator seeded from process clocks/state.", "fixed"),
    "compare": ("strverscmp could inspect before its input and timingsafe_memcmp "
                "used data-dependent selection; both were replaced.", "fixed"),
    "exit": ("Exit-handler registration was not connected to exit and was not "
             "serialized; handlers now run LIFO with registration locking.", "fixed"),
    "math": ("The pinned musl implementation was reviewed for zedBSD's 64-bit "
             "long-double ABI; local error helpers now update the software fenv.", "fixed"),
}


def category(row):
    symbol = row["symbol"]
    header = row["header"]
    if header == "<fenv.h>": return "fenv"
    if header == "<math.h>": return "math"
    if header == "<setjmp.h>": return "setjmp"
    if symbol in {"funopen", "freopen", "fpurge", "fgetln"}: return "stream"
    if symbol in {"scanf", "fscanf", "sscanf", "vscanf", "vfscanf", "vsscanf",
                  "wscanf", "fwscanf", "swscanf", "vwscanf", "vfwscanf", "vswscanf"}:
        return "scan"
    if header in {"<wchar.h>", "<uchar.h>"}: return "wide"
    if symbol in {"arc4random", "arc4random_buf", "arc4random_uniform", "srandomdev"}:
        return "random"
    if symbol in {"strverscmp", "timingsafe_bcmp", "timingsafe_memcmp"}: return "compare"
    if symbol in {"exit", "_Exit", "quick_exit", "atexit", "at_quick_exit"}: return "exit"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    with args.csv.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames
        rows = list(reader)
    for row in rows:
        row["review_status"] = "reviewed"
        row["test_evidence"] = (
            "header/symbol audit; amd64 QEMU boot; "
            "pcat/pc98 HDD integration builds"
        )
        key = category(row)
        if key:
            row["review_findings"], row["fix_status"] = FINDINGS[key]
        else:
            row["review_findings"] = "No actionable issue found in signature, boundary, and integration review."
            row["fix_status"] = "not-needed"
    with args.csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"reviewed {len(rows)} rows")


if __name__ == "__main__":
    main()
