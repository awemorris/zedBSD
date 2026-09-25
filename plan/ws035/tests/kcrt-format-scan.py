#!/usr/bin/env python3
# ws035-p034: every kernel format stays inside the kcrt printf subset.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Scan the format strings the kernel hands to the kcrt formatting engine.

usage: python3 plan/ws035/tests/kcrt-format-scan.py [--json PATH]

The files are the kernel scope of kcrt-rewrite.py.  The engine sinks are
kern_logf (format argument 0), kern_snprintf and kern_vsnprintf (2), and every
function-like macro whose body passes one of its parameters (or
__VA_ARGS__) as the format of a sink, found to a fixed point.  The
format-check sinks drv_i915_vbt_fmtcheck and drv_i915_lcd_fmtcheck only type
check at compile time and never reach the engine (I915_VBT_LOG, I915_DP_LOG
pass the text unformatted); their macros are scanned and reported apart,
for information.

For each call the string literals of the format argument are joined and
every conversion specification is parsed.  The subset is: conversions
d i u x X c s p %, flags 0 and # (# only on x and X), a decimal width, and
lengths hh h l ll z on d i u x X.  Exit status 1 when an engine format uses
anything else; a format that is not a literal is listed.
"""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("kcrt_rewrite", REPO / "plan/ws035/tests/kcrt-rewrite.py")
rewrite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rewrite)

ENGINE = {"kern_logf": 0, "kern_snprintf": 2, "kern_vsnprintf": 2}
CHECK_ONLY = {"drv_i915_vbt_fmtcheck": 0, "drv_i915_lcd_fmtcheck": 0}
SPEC = re.compile(r"%([-+ #0]*)(\*|\d+)?(\.(\*|\d*))?(hh|h|ll|l|z|j|t|L|q)?(.?)", re.S)
STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
IDENT = re.compile(r"[A-Za-z_]\w*")


def strip_comments(text):
    """The text with every comment blanked and every literal kept."""
    result = []
    i = 0
    n = len(text)
    state = "code"
    while i < n:
        c = text[i]
        if state == "code":
            if text.startswith("/*", i):
                state = "block"
                result.append("  ")
                i += 2
                continue
            if text.startswith("//", i):
                state = "line"
                result.append("  ")
                i += 2
                continue
            if c in "\"'":
                state = c
            result.append(c)
            i += 1
            continue
        if state == "block":
            if text.startswith("*/", i):
                state = "code"
                result.append("  ")
                i += 2
                continue
            result.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "code"
                result.append("\n")
            else:
                result.append(" ")
            i += 1
            continue
        # inside a literal
        if c == "\\" and i + 1 < n:
            result.append(text[i:i + 2])
            i += 2
            continue
        if c == state or c == "\n":
            state = "code"
        result.append(c)
        i += 1
    return "".join(result)


def arguments(text, open_index):
    """The top-level arguments of the call whose '(' is at open_index, and the end index."""
    depth = 0
    args = []
    start = open_index + 1
    i = open_index
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i].strip())
                return args, i
        elif c == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    return None, n


def classify(spec_match):
    flags, width, precision, _, length, conversion = spec_match.groups()
    problems = []
    if conversion == "%":
        if flags or width or precision or length:
            problems.append("modified %%")
        return problems
    if conversion == "":
        return ["format ends after %"]
    if conversion not in "diuxXcsp":
        problems.append("conversion %s" % conversion)
    for flag in flags:
        if flag in "-+ ":
            problems.append("flag '%s'" % flag)
        if flag == "#" and conversion not in "xX":
            problems.append("# on %s" % conversion)
    if width == "*":
        problems.append("* width")
    if precision:
        problems.append("precision")
    if length:
        if length not in ("hh", "h", "l", "ll", "z"):
            problems.append("length %s" % length)
        elif conversion not in "diuxX":
            problems.append("length %s on %s" % (length, conversion))
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", default="plan/ws035/phase034/format-scan.json")
    args = parser.parse_args()

    files = rewrite.scope()
    texts = {rel: strip_comments((REPO / rel).read_text(encoding="utf-8")) for rel in files}

    # The function-like macros of the scope, with their continuation lines joined.
    macros = {}
    for rel, text in texts.items():
        joined = text.replace("\\\n", " ")
        for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^)]*)\)(.*)$", joined, re.M):
            params = [p.strip() for p in m.group(2).split(",") if p.strip()]
            macros.setdefault(m.group(1), []).append((params, m.group(3), rel))

    # The object-like macros that stand for a string literal (DRM_MODE_FMT).
    strings = {}
    for rel, text in texts.items():
        joined = text.replace("\\\n", " ")
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+((?:"(?:[^"\\\n]|\\.)*"[ \t]*)+)$', joined, re.M):
            strings.setdefault(m.group(1), "".join(STRING.findall(m.group(2))))

    sinks = {name: ("engine", index) for name, index in ENGINE.items()}
    sinks.update({name: ("check-only", index) for name, index in CHECK_ONLY.items()})
    changed = True
    while changed:
        changed = False
        for name, definitions in sorted(macros.items()):
            if name in sinks:
                continue
            for params, body, rel in definitions:
                for sink, (kind, index) in list(sinks.items()):
                    for call in re.finditer(r"\b%s\s*\(" % re.escape(sink), body):
                        call_args, _ = arguments(body, call.end() - 1)
                        if not call_args or index >= len(call_args):
                            continue
                        words = IDENT.findall(STRING.sub(" ", call_args[index]))
                        position = None
                        for word in words:
                            if word == "__VA_ARGS__" and params and params[-1] == "...":
                                position = len(params) - 1
                            elif word in params:
                                position = params.index(word)
                        if position is not None and name not in sinks:
                            sinks[name] = (kind, position)
                            changed = True

    calls = {"engine": 0, "check-only": 0}
    conversions = {"engine": {}, "check-only": {}}
    outside = {"engine": [], "check-only": []}
    nonliteral = []
    for rel in files:
        text = texts[rel]
        define_spans = []
        for m in re.finditer(r"^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", text, re.M):
            define_spans.append((m.start(), m.end()))
        for sink, (kind, index) in sorted(sinks.items()):
            for call in re.finditer(r"(?<![\w.>])%s\s*\(" % re.escape(sink), text):
                inside_define = any(s <= call.start() < e for s, e in define_spans)
                call_args, _ = arguments(text, call.end() - 1)
                line = text.count("\n", 0, call.start()) + 1
                if call_args is None or index >= len(call_args):
                    continue
                argument = call_args[index]

                # A declaration or definition of the sink is not a call.
                if re.search(r"\bconst\b|\bchar\s*\*", argument):
                    continue

                # A string macro is replaced by the literal it stands for.
                for word in IDENT.findall(STRING.sub(" ", argument)):
                    if word in strings:
                        argument = re.sub(r"\b%s\b" % word, '"%s"' % strings[word], argument)
                literals = STRING.findall(argument)
                rest = IDENT.findall(STRING.sub(" ", argument))
                if not literals or rest:
                    # A macro body forwards its parameter; its uses are scanned.
                    if inside_define:
                        continue
                    nonliteral.append({"file": rel, "line": line, "sink": sink, "kind": kind, "format": argument[:120]})
                    continue
                calls[kind] += 1
                fmt = "".join(literals)
                for spec_match in SPEC.finditer(fmt):
                    token = spec_match.group(0)
                    conversions[kind][token] = conversions[kind].get(token, 0) + 1
                    problems = classify(spec_match)
                    if problems:
                        outside[kind].append({"file": rel, "line": line, "sink": sink, "spec": token, "problems": problems})

    report = {
        "tool": "plan/ws035/tests/kcrt-format-scan.py",
        "scope_files": len(files),
        "sinks": {name: {"kind": kind, "format_argument": index} for name, (kind, index) in sorted(sinks.items())},
        "calls_scanned": calls,
        "conversion_counts": {kind: dict(sorted(v.items())) for kind, v in conversions.items()},
        "engine_outside_subset": outside["engine"],
        "check_only_outside_subset": outside["check-only"],
        "nonliteral_formats": nonliteral,
    }
    out = REPO / args.json
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print("kcrt-format-scan: engine calls %d, outside subset %d; check-only calls %d, outside subset %d; non-literal %d"
          % (calls["engine"], len(outside["engine"]), calls["check-only"], len(outside["check-only"]), len(nonliteral)))
    for item in outside["engine"]:
        print("  engine: %s:%d %s %s %s" % (item["file"], item["line"], item["sink"], item["spec"], item["problems"]))
    for item in nonliteral:
        print("  non-literal: %s:%d %s(%s)" % (item["file"], item["line"], item["sink"], item["format"]))
    return 1 if outside["engine"] else 0


if __name__ == "__main__":
    sys.exit(main())
