#!/usr/bin/env python3
"""Import the Issue 8 utility index without vendoring standards text."""

import csv
from html.parser import HTMLParser
from pathlib import Path
import re
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
BASE = "https://pubs.opengroup.org/onlinepubs/9799919799/"
INDEX = BASE + "idx/utilities.html"
OUTPUT = ROOT / "tests" / "posix-2024-utilities.csv"
FIELDS = [
    "order", "utility", "option_codes", "requirement", "issue8_change",
    "kind", "status", "implementation_evidence", "test_evidence",
    "standard_url", "notes",
]
XSI_TARGET = {"XSI"}
BUILTINS = {
    "alias", "bg", "cd", "command", "echo", "env", "false", "fc",
    "fg", "getopts", "jobs", "printf", "pwd", "read", "test", "true",
    "type", "umask", "unalias", "wait",
}
NEW_ISSUE8 = {
    "c17", "gettext", "msgfmt", "ngettext", "readlink", "realpath",
    "timeout", "xgettext",
}


def download(url):
    request = Request(url, headers={"User-Agent": "zedBSD audit importer"})
    with urlopen(request, timeout=30) as response:
        return response.read().decode("utf-8", "replace")


class SynopsisParser(HTMLParser):
    def __init__(self, utility):
        super().__init__()
        self.utility = utility
        self.depth = 0
        self.depths = []

    def handle_starttag(self, tag, attributes):
        if tag != "img":
            return
        alt = dict(attributes).get("alt")
        if alt == "[Option Start]":
            self.depth += 1
        elif alt == "[Option End]" and self.depth != 0:
            self.depth -= 1

    def handle_data(self, data):
        if re.search(r"(?<![A-Za-z0-9_])" + re.escape(self.utility) +
                     r"(?![A-Za-z0-9_])", data):
            self.depths.append(self.depth)


def indexed_utilities(index):
    return re.findall(
        r'href="\.\./utilities/([^"/#]+\.html)"[^>]*>([^<]+)</a>',
        index,
    )


def synopsis_details(page, utility):
    begin = page.find(">SYNOPSIS</h4>")
    end = page.find(">DESCRIPTION</h4>", begin)
    if begin < 0 or end < 0:
        raise RuntimeError(f"cannot locate {utility} synopsis")
    synopsis = page[begin:end]
    parser = SynopsisParser(utility)
    parser.feed(synopsis)
    if not parser.depths:
        raise RuntimeError(f"cannot classify {utility} synopsis")
    codes = sorted(set(re.findall(r"open_code\('([^']+)'\)", synopsis)))
    return codes, 0 in parser.depths


def evidence(utility):
    source = ROOT / "userland" / "base" / utility / "main.c"
    if source.is_file():
        return "external", str(source.relative_to(ROOT))
    if utility in BUILTINS:
        return "shell-builtin", "userland/base/sh/main.c"
    if utility == "sh":
        return "shell", "userland/base/sh/main.c"
    return "external", ""


def main():
    utilities = indexed_utilities(download(INDEX))
    rows = []
    for order, (filename, utility) in enumerate(utilities, 1):
        url = BASE + "utilities/" + filename
        codes, has_base_form = synopsis_details(download(url), utility)
        kind, source = evidence(utility)
        targeted = has_base_form or bool(set(codes) & XSI_TARGET)
        if targeted:
            requirement = "required" if has_base_form else "enabled-option"
            status = "implemented-unreviewed" if source else "missing"
        else:
            requirement = "disabled-option"
            status = "option-disabled"
        test = ""
        if source and kind == "external":
            test = "tests/test-userland-commands-host.sh"
        elif source:
            test = "tests/sh-lexer-host-test.c"
        rows.append({
            "order": str(order),
            "utility": utility,
            "option_codes": "|".join(codes),
            "requirement": requirement,
            "issue8_change": "new" if utility in NEW_ISSUE8 else "unchanged",
            "kind": kind,
            "status": status,
            "implementation_evidence": source,
            "test_evidence": test,
            "standard_url": url,
            "notes": "imported from the Issue 8 synopsis",
        })
        print(f"{order:3d}/{len(utilities)} {utility}: {requirement} {status}")
    with OUTPUT.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} rows to {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
