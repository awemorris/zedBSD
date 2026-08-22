#!/usr/bin/env python3
"""Build the zedBSD kernel-facing POSIX.1-2008/2017 API audit table."""

import argparse
import csv
import html
from html.parser import HTMLParser
from pathlib import Path
import re
import subprocess


# Margin legends whose functionality is optional for POSIX conformance.
OPTIONAL_CODES = {
    "ADV", "CPT", "FSC", "IP6", "MC1", "ML", "MLR", "MON", "MSG",
    "MX", "MXX", "PIO", "PS", "RPI", "RPP", "RS", "SHM", "SIO",
    "SPN", "SS", "TCT", "TEF", "TPI", "TPP", "TPS", "TRC", "TRI",
    "TRL", "TSA", "TSH", "TSP", "TSS", "TYM", "XSI", "XSR",
}

# Headers whose interfaces need kernel state or a kernel-backed object.  Pure
# ISO C computation/string APIs and POSIX shell utilities are intentionally
# outside this kernel audit.
KERNEL_HEADERS = {
    "aio.h", "dirent.h", "fcntl.h", "mqueue.h", "poll.h", "pthread.h",
    "sched.h", "semaphore.h", "signal.h", "spawn.h", "sys/mman.h",
    "sys/resource.h", "sys/select.h", "sys/socket.h", "sys/stat.h",
    "sys/statvfs.h", "sys/times.h", "sys/uio.h", "sys/un.h", "sys/wait.h",
    "termios.h", "time.h", "unistd.h",
}

FIELDNAMES = [
    "order", "category", "header", "symbol", "kind", "requirement",
    "option_codes", "implementation_status", "header_evidence",
    "libc_source", "syscall", "kernel_source", "test_evidence",
    "implementation_notes", "review_status", "review_findings", "fix_status",
    "standard_url",
]


def header_from_href(href):
    name = href.rsplit("/", 1)[-1].removesuffix(".html")
    for prefix in ("arpa_", "net_", "netinet_", "sys_"):
        if name.startswith(prefix):
            return name.replace("_", "/", 1)
    return name


class SynopsisParser(HTMLParser):
    def __init__(self, symbol):
        super().__init__(convert_charrefs=True)
        self.symbol = symbol
        self.pending_codes = []
        self.code_stack = []
        self.headers = []
        self.matches = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == "a":
            href = values.get("href", "")
            match = re.search(r"open_code\('([^']+)'\)", href)
            if match:
                self.pending_codes = match.group(1).split()
            elif "../basedefs/" in href:
                header_name = header_from_href(href)
                if header_name not in self.headers:
                    self.headers.append(header_name)
        elif tag == "img":
            src = values.get("src", "")
            if "opt-start.gif" in src:
                self.code_stack.append(tuple(self.pending_codes))
                self.pending_codes = []
            elif "opt-end.gif" in src and self.code_stack:
                self.code_stack.pop()

    def handle_data(self, data):
        if re.search(r"(?<![A-Za-z0-9_])" + re.escape(self.symbol) +
                     r"(?![A-Za-z0-9_])", data):
            codes = sorted({code for level in self.code_stack for code in level})
            self.matches.append((tuple(self.headers), codes))


def read_index(posix_root):
    text = (posix_root / "idx/functions.html").read_text(
        encoding="utf-8", errors="replace")
    pattern = re.compile(
        r'<li><a href="\.\./functions/([^"]+)"[^>]*><i>([^<]+)</i>'
        r'(\(\))?</a></li>', re.S)
    return [(page, html.unescape(symbol), "function" if parens else "data")
            for page, symbol, parens in pattern.findall(text)]


def synopsis_info(posix_root, page, symbol):
    path = posix_root / "functions" / page
    text = path.read_text(encoding="iso-8859-1", errors="replace")
    match = re.search(
        r'<blockquote class="synopsis">(.*?)</blockquote>', text, re.S)
    if not match:
        return "", [], text
    parser = SynopsisParser(symbol)
    parser.feed(match.group(1))
    if not parser.matches:
        return "", [], text
    # The index contains one entry per public name.  Prefer an occurrence with
    # a header and the fewest enclosing option legends.
    headers, codes = sorted(
        parser.matches, key=lambda item: (not item[0], len(item[1])))[0]
    header_name = headers[0] if headers else ""
    return header_name, codes, text


def collect_defined_symbols(repo):
    roots = [repo / "build/amd64/user64", repo / "build/amd64"]
    objects = set()
    for root in roots:
        if root.exists():
            for candidate in root.rglob("*.o"):
                relative = candidate.relative_to(repo).as_posix()
                if "/libc/" in relative:
                    objects.add(candidate)
    if not objects:
        return set()
    result = subprocess.run(
        ["nm", "-g", "--defined-only", *map(str, sorted(objects))],
        check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    symbols = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2] in {
                "A", "B", "C", "D", "G", "R", "S", "T", "V", "W"}:
            symbols.add(fields[-1])
    return symbols


def source_evidence(repo, symbol):
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"\s*\(")
    candidates = []
    for base in (repo / "libc", repo / "userland/base/libc"):
        if not base.exists():
            continue
        for path in base.rglob("*.c"):
            try:
                if pattern.search(path.read_text(encoding="utf-8", errors="replace")):
                    candidates.append(str(path.relative_to(repo)))
            except OSError:
                pass
    return "|".join(candidates[:4])


def implementation_state(repo, header_name, symbol, kind, defined):
    header_path = repo / "libc/include" / header_name
    if not header_path.exists():
        return "missing", ""
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(symbol) +
                         r"(?![A-Za-z0-9_])")
    evidence_paths = []
    for base in (repo / "libc/include", repo / "include/uapi"):
        for candidate in base.rglob("*.h"):
            text = candidate.read_text(encoding="utf-8", errors="replace")
            if pattern.search(text):
                evidence_paths.append(str(candidate.relative_to(repo)))
    evidence = "|".join(evidence_paths[:4])
    if not evidence_paths:
        return "missing", evidence
    for evidence_path in evidence_paths:
        text = (repo / evidence_path).read_text(encoding="utf-8", errors="replace")
        if re.search(r"^\s*#\s*define\s+" + re.escape(symbol) + r"\b",
                     text, re.M):
            return "implemented", evidence
    if kind == "data" or symbol in defined:
        return "implemented", evidence
    return "declared-only", evidence


def category_for(header_name):
    if header_name == "pthread.h":
        return "threads"
    if header_name in {"fcntl.h", "sys/stat.h", "sys/statvfs.h", "dirent.h"}:
        return "filesystem"
    if header_name in {"signal.h", "sys/wait.h", "spawn.h", "unistd.h"}:
        return "process"
    if header_name in {"time.h", "sys/times.h", "sys/resource.h"}:
        return "time-resource"
    if header_name in {"poll.h", "sys/select.h"}:
        return "multiplexing"
    if header_name in {"sys/socket.h", "sys/un.h"}:
        return "sockets"
    if header_name == "termios.h":
        return "terminal"
    if header_name in {"aio.h", "sys/mman.h"}:
        return "io-memory"
    return "synchronization"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--posix-root", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    repo = args.repo.resolve()
    posix_root = args.posix_root.resolve()
    defined = collect_defined_symbols(repo)
    rows = []
    seen = set()
    for page, symbol, kind in read_index(posix_root):
        if symbol in seen:
            continue
        header_name, codes, _ = synopsis_info(posix_root, page, symbol)
        if header_name not in KERNEL_HEADERS:
            continue
        optional = sorted(set(codes) & OPTIONAL_CODES)
        if optional:
            continue
        seen.add(symbol)
        status, header_evidence = implementation_state(
            repo, header_name, symbol, kind, defined)
        rows.append({
            "category": category_for(header_name),
            "header": f"<{header_name}>",
            "symbol": symbol,
            "kind": kind,
            "requirement": "required",
            "option_codes": " ".join(codes),
            "implementation_status": status,
            "header_evidence": header_evidence,
            "libc_source": source_evidence(repo, symbol),
            "syscall": "",
            "kernel_source": "",
            "test_evidence": "",
            "implementation_notes": "",
            "review_status": "pending",
            "review_findings": "",
            "fix_status": "pending",
            "standard_url":
                f"https://pubs.opengroup.org/onlinepubs/9699919799/functions/{page}",
        })

    rows.sort(key=lambda row: (row["category"], row["symbol"].lower(), row["symbol"]))
    for index, row in enumerate(rows, 1):
        row["order"] = index
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDNAMES)
        writer.writeheader()
        writer.writerows(rows)
    counts = {}
    for row in rows:
        counts[row["implementation_status"]] = counts.get(
            row["implementation_status"], 0) + 1
    print(f"wrote {len(rows)} required kernel-facing interfaces to {args.output}")
    for status in sorted(counts):
        print(f"{status}: {counts[status]}")


if __name__ == "__main__":
    main()
