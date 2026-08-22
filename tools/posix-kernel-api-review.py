#!/usr/bin/env python3
"""Attach the completed semantic review and correction disposition to CSV."""

import argparse
import csv
from pathlib import Path


FINDINGS = {
    "aio_suspend": "A timeout with no completed request returned immediately instead of waiting.",
    "lio_listio": "List submission incorrectly emitted each aiocb notification as well as the list notification.",
    "execvp": "ENOEXEC from a PATH candidate did not invoke the shell fallback required by exec*p semantics.",
    "execlp": "ENOEXEC from a PATH candidate did not invoke the shell fallback required by exec*p semantics.",
    "fexecve": "The initial descriptor execution path could dereference a pseudo-file with no inode.",
    "pthread_mutex_init": "Extending pthread_mutex_t left the static initializer one field short under -Werror.",
    "clock": "The first implementation reported monotonic uptime, not CPU time consumed by the process.",
    "strftime": "The initial formatter omitted the mandatory ISO week conversions %g, %G, and %V.",
    "strftime_l": "The initial formatter omitted the mandatory ISO week conversions %g, %G, and %V.",
    "times": "The first implementation reported elapsed uptime as user CPU and omitted waited-child CPU time.",
    "tzset": "The initial parser handled fixed offsets only and ignored POSIX daylight transition rules.",
}

NEW_TESTED = {
    "aio_error", "aio_read", "aio_return", "aio_write", "alphasort",
    "fexecve", "getopt", "gmtime", "gmtime_r", "pthread_attr_getschedparam",
    "scandir", "sched_yield", "sockatmark", "strftime", "tcgetsid",
    "tcsendbreak", "times",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "path", type=Path, nargs="?",
        default=Path("plan/posix-required-kernel-api.csv"))
    path = parser.parse_args().path
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames
        rows = list(reader)
    for row in rows:
        symbol = row["symbol"]
        row["review_status"] = "reviewed"
        if symbol in FINDINGS:
            row["review_findings"] = FINDINGS[symbol]
            row["fix_status"] = "fixed"
        else:
            row["review_findings"] = ""
            row["fix_status"] = "not-needed"
        if symbol in NEW_TESTED:
            row["test_evidence"] = "userland/base/tests/posix-r2-remaining.c"
        elif not row["test_evidence"]:
            for candidate in (
                    Path("userland/base/tests/posix-r2.c"),
                    Path("userland/base/tests/posix-r2-remaining.c")):
                if candidate.exists() and symbol in candidate.read_text(
                        encoding="utf-8", errors="replace"):
                    row["test_evidence"] = str(candidate)
                    break
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"reviewed {len(rows)} interfaces; {len(FINDINGS)} findings fixed")


if __name__ == "__main__":
    main()
