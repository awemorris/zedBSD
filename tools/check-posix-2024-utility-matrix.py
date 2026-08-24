#!/usr/bin/env python3
"""Validate the complete Issue 8 utility inventory and its profile gate."""

import csv
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "tests" / "posix-2024-utilities.csv"
FEATURES = ROOT / "libc" / "include" / "zedbsd" / "features.h"
FIELDS = [
    "order", "utility", "option_codes", "requirement", "issue8_change",
    "kind", "status", "implementation_evidence", "test_evidence",
    "standard_url", "notes",
]
EXPECTED = """
admin alias ar asa at awk basename batch bc bg c17 cal cat cd cflow chgrp
chmod chown cksum cmp comm command compress cp crontab csplit ctags cut cxref
date dd delta df diff dirname du echo ed env ex expand expr false fc fg file
find fold fuser gencat get getconf getopts gettext grep hash head iconv id
ipcrm ipcs jobs join kill lex link ln locale localedef logger logname lp ls m4
mailx make man mesg mkdir mkfifo more msgfmt mv newgrp ngettext nice nl nm
nohup od paste patch pathchk pax pr printf prs ps pwd read readlink realpath
renice rm rmdel rmdir sact sccs sed sh sleep sort split strings strip stty tabs
tail talk tee test time timeout touch tput tr true tsort tty type ulimit umask
unalias uname uncompress unexpand unget uniq unlink uucp uudecode uuencode
uustat uux val vi wait wc what who write xargs xgettext yacc zcat
""".split()
REQUIREMENTS = {"required", "enabled-option", "disabled-option"}
STATUSES = {
    "missing", "implemented-unreviewed", "reviewed", "option-disabled",
}


def fail(message):
    raise SystemExit(f"POSIX.1-2024 utility matrix: {message}")


def advertised_posix2_version():
    text = FEATURES.read_text(encoding="utf-8")
    match = re.search(r"^#define\s+_POSIX2_VERSION\s+(\d+)L", text,
                      re.MULTILINE)
    if match is None:
        fail("cannot read _POSIX2_VERSION")
    return int(match.group(1))


def validate():
    with MATRIX.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            fail(f"unexpected columns: {reader.fieldnames!r}")
        rows = list(reader)
    if [row["utility"] for row in rows] != EXPECTED:
        fail("Issue 8 index rows are missing, duplicated, or reordered")
    complete_profile = advertised_posix2_version() >= 202405
    pending = 0
    reviewed = 0
    for line, row in enumerate(rows, 2):
        if row["order"] != str(line - 1):
            fail(f"line {line}: invalid order")
        if row["requirement"] not in REQUIREMENTS:
            fail(f"line {line}: invalid requirement")
        if row["status"] not in STATUSES:
            fail(f"line {line}: invalid status")
        targeted = row["requirement"] != "disabled-option"
        if row["status"] == "option-disabled" and targeted:
            fail(f"line {line}: targeted utility is marked option-disabled")
        if row["status"] != "option-disabled" and not targeted:
            fail(f"line {line}: disabled option is presented as implemented")
        source = row["implementation_evidence"]
        test = row["test_evidence"]
        if row["status"] in ("implemented-unreviewed", "reviewed"):
            if not source or not (ROOT / source).is_file():
                fail(f"line {line}: invalid implementation evidence")
        if row["status"] == "reviewed":
            if not test or not (ROOT / test).is_file():
                fail(f"line {line}: reviewed utility lacks a test")
            reviewed += 1
        elif targeted:
            pending += 1
        expected_url = (
            "https://pubs.opengroup.org/onlinepubs/9799919799/utilities/" +
            row["utility"] + ".html"
        )
        if row["standard_url"] != expected_url:
            fail(f"line {line}: invalid standard URL")
    if complete_profile and pending != 0:
        fail(f"_POSIX2_VERSION advertises Issue 8 with {pending} pending rows")
    profile = "complete" if complete_profile else "staged (_POSIX2_VERSION=200809L)"
    print("zedBSD POSIX.1-2024 utility matrix: PASS "
          f"({len(rows)} rows, {reviewed} reviewed, {pending} pending; "
          f"{profile})")


if __name__ == "__main__":
    validate()
