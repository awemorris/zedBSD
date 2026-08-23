#!/usr/bin/env python3
"""Enforce the final zero-pending gate for the SUSv4/XSI audit CSV."""

import argparse
import csv
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    with args.csv.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))

    errors = []
    if len(rows) != 134:
        errors.append(f"catalog has {len(rows)} rows, expected 134")
    symbols = [row["symbol"] for row in rows]
    if len(set(symbols)) != len(symbols):
        errors.append("catalog contains duplicate symbols")
    if [int(row["order"]) for row in rows] != list(range(1, len(rows) + 1)):
        errors.append("order column is not contiguous")

    for row in rows:
        symbol = row["symbol"]
        if row["implementation_status"] != "implemented":
            errors.append(f"{symbol}: {row['implementation_status']}")
        if not row["header_evidence"] or not row["definition_evidence"]:
            errors.append(f"{symbol}: incomplete declaration/definition evidence")
        if not row["test_evidence"]:
            errors.append(f"{symbol}: missing test evidence")
        if row["review_status"] != "reviewed":
            errors.append(f"{symbol}: review is {row['review_status']}")
        if not row["review_findings"]:
            errors.append(f"{symbol}: review disposition is empty")
        if row["fix_status"] not in {"fixed", "not-needed"}:
            errors.append(f"{symbol}: fix is {row['fix_status']}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        raise SystemExit(1)
    fixed = sum(row["fix_status"] == "fixed" for row in rows)
    print(f"SUSv4/XSI review gate: PASS (134 implemented/reviewed, {fixed} fixes)")


if __name__ == "__main__":
    main()
