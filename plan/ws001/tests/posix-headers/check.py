#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Checks zedBSD's C library headers against the POSIX.1-2024 names (ws001-p045).

    python3 -I plan/ws001/tests/posix-headers/check.py OUTDIR [--target amd64|i386|aarch64]... [--header NAME]...

For each header of posix-2024.json (extract.py) it writes a C file that includes the header alone, with
_XOPEN_SOURCE=800 (POSIX.1-2024 with the XSI option), and refers to every name the standard has it define: a type in
a typedef, a structure in a sizeof and each listed member in a sizeof, a constant and a limit as a macro or else as an
expression, a function-like macro as a macro, a function by its address, a variable by its address.  The file is
compiled with the tree's clang for each target against include/libc (the compiler's own include directory only
behind it, for the headers the C library leaves to the compiler); the errors on the probe's lines are the names
missing, and an error inside a header means the header does not stand alone.

OUTDIR gets the C files, the compiler's output, results.json and table.md (a row per header: what is missing, by the
option it belongs to -- the base and CX, XSI, or another option).  The last line printed is "missing N (base B, XSI X,
options O)".
"""

import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
CLANG = ROOT / "build/llvm/bin/clang"
NAMES = Path(__file__).with_name("posix-2024.json")
TARGETS = {
    "amd64": ["--target=x86_64-unknown-zedbsd", "-DKERN_USER_ABI_LP64"],
    "i386": ["--target=i386-unknown-zedbsd"],
    "aarch64": ["--target=aarch64-unknown-zedbsd", "-DKERN_USER_ABI_AARCH64", "-DKERN_USER_ABI_LP64"],
}


def option_class(options):
    """The column a name's options put it in: base (none, or CX only), XSI, or another option."""
    rest = [option for option in options if option != "CX"]
    if not rest:
        return "base"
    if any("XSI" in option.split("|") for option in rest):
        return "XSI"
    return "option"


def probe(header, names):
    """The C probe of one header and the meaning of each probe line."""
    lines = ["#define _XOPEN_SOURCE 800", f"#include <{header}>"]
    meaning = {}

    def emit(text, what):
        lines.append(text)
        meaning[len(lines)] = what

    index = 0
    for name, entry in sorted(names["types"].items()):
        index += 1
        emit(f"typedef {name} probe_type_{index};", ("type", name, entry))
    for name, entry in sorted(names["structs"].items()):
        spelled = name if entry.get("typedef") else ("union " if entry.get("union") else "struct ") + name
        index += 1
        emit(f"static char probe_struct_{index}[sizeof({spelled})];", ("struct", name, entry))
        for member in entry.get("members", []):
            index += 1
            emit(f"static char probe_member_{index}[sizeof((({spelled} *)0)->{member})];", ("member", f"{name}.{member}", entry))
    for kind in ("constants", "limits"):
        for name, entry in sorted(names[kind].items()):
            index += 1
            lines.append(f"#if !defined({name})")
            emit(f"static int probe_value_{index}(void) {{ return (int)sizeof({name}); }}", (kind[:-1], name, entry))
            lines.append("#endif")
    for name, entry in sorted(names["macros"].items()):
        index += 1
        lines.append(f"#if !defined({name})")
        emit(f"static int probe_macro_{index} = probe_missing_macro_{name};", ("macro", name, entry))
        lines.append("#endif")
    for name, entry in sorted(names["functions"].items()):
        # A function the header defines as a macro too is taken as the macro (the generic and the type-generic
        # ones, va_start, FD_SET, setjmp).
        index += 1
        lines.append(f"#if !defined({name})")
        emit(f"void (*probe_function_{index})(void) = (void (*)(void))({name});", ("function", name, entry))
        lines.append("#endif")
    for name, entry in sorted(names["variables"].items()):
        index += 1
        emit(f"const void *probe_variable_{index} = (const void *)&({name});", ("variable", name, entry))
    return "\n".join(lines) + "\n", meaning


def check(outdir, target, header, names):
    """Compiles one header's probe for one target; returns its findings."""
    stem = header.replace("/", "_")
    directory = outdir / target
    directory.mkdir(parents=True, exist_ok=True)
    source, meaning = probe(header, names)
    path = directory / f"{stem}.c"
    path.write_text(source, encoding="utf-8")
    resource = subprocess.run([str(CLANG), "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
    command = [str(CLANG), *TARGETS[target], "-std=c11", "-nostdinc", "-isystem", str(ROOT / "include/libc"),
               "-isystem", str(ROOT / "include"), "-isystem", f"{resource}/include", "-fsyntax-only", "-ferror-limit=0",
               "-w", str(path)]
    result = subprocess.run(command, capture_output=True, text=True)
    (directory / f"{stem}.log").write_text(result.stderr, encoding="utf-8")
    missing = {}
    header_errors = []
    for line in result.stderr.splitlines():
        match = re.match(r"(.*?):(\d+):\d+: (?:fatal )?error: (.*)", line)
        if not match:
            continue
        if Path(match.group(1)).name == path.name:
            what = meaning.get(int(match.group(2)))
            if what is None:
                header_errors.append(match.group(3))
                continue
            kind, name, entry = what
            missing[name] = {"kind": kind, "class": option_class(entry["options"]), "options": entry["options"],
                             "may_omit": bool(entry.get("may_omit")), "error": match.group(3)}
        else:
            header_errors.append(f"{match.group(1)}:{match.group(2)}: {match.group(3)}")
    counted = sum(len(names[kind]) for kind in ("types", "constants", "limits", "macros", "functions", "variables"))
    counted += sum(1 + len(entry.get("members", [])) for entry in names["structs"].values())
    return {"names": counted, "missing": missing, "header_errors": header_errors}


def main():
    """Checks every header for every target and writes the table."""
    parser = argparse.ArgumentParser()
    parser.add_argument("outdir", type=Path)
    parser.add_argument("--target", action="append", choices=sorted(TARGETS))
    parser.add_argument("--header", action="append")
    arguments = parser.parse_args()
    targets = arguments.target or ["amd64", "i386", "aarch64"]
    everything = json.loads(NAMES.read_text(encoding="utf-8"))
    headers = arguments.header or sorted(everything)
    results = {}
    for header in headers:
        results[header] = {target: check(arguments.outdir, target, header, everything[header]) for target in targets}
    (arguments.outdir / "results.json").write_text(json.dumps(results, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")

    rows = ["| header | names | missing (base / XSI / options; omittable limits apart) | header errors | missing names |",
            "| --- | ---: | --- | --- | --- |"]
    totals = {"base": 0, "XSI": 0, "option": 0}
    for header in headers:
        first = results[header][targets[0]]
        union = {}
        errors = []
        for target in targets:
            union.update(results[header][target]["missing"])
            errors += [f"{target}: {error}" for error in results[header][target]["header_errors"][:2]]
        counts = {"base": 0, "XSI": 0, "option": 0}
        omitted = 0
        for name, entry in union.items():
            if entry["may_omit"]:
                omitted += 1
                continue
            counts[entry["class"]] += 1
        for column in totals:
            totals[column] += counts[column]
        listed = ", ".join(f"{name}({union[name]['kind'][0]}{'' if union[name]['class'] == 'base' else ',' + union[name]['class']})"
                           for name in sorted(union) if not union[name]["may_omit"])
        rows.append(f"| `<{header}>` | {first['names']} | {counts['base']} / {counts['XSI']} / {counts['option']}"
                    f"{f'; omittable {omitted}' if omitted else ''} | {'; '.join(errors)[:160]} | {listed[:400]} |")
    (arguments.outdir / "table.md").write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"missing {sum(totals.values())} (base {totals['base']}, XSI {totals['XSI']}, options {totals['option']})")


if __name__ == "__main__":
    main()
