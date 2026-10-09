#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Reads the POSIX.1-2024 header pages into the list of names each header must define (ws001-p045).

    python3 -I extract.py PAGES-DIRECTORY OUTPUT.json

PAGES-DIRECTORY holds the pages of https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/ (aio.h.html ...), as
fetched by fetch.sh.  Only each page's DESCRIPTION is read.  The names, by kind:

  types       a bold name ending in _t ("shall define the ... types"), or a bold name in a data types list
  structs     a bold name followed by "structure" or "union", with the members its <pre> lists
  constants   a <dt> that is not bold (symbolic constants and object-like macros); "{NAME}" limits are kept apart
  macros      a <dt> or <pre> name followed by "(" that is not a prototype (function-like macros)
  functions   a prototype in a <pre> (a declaration ending in ";" with a parameter list)
  variables   an "extern" declaration without a parameter list
  others      a table's first column that is neither upper case nor a _t type (type-generic and atomic names)

Each name carries the option codes shaded around it (CX, XSI, ADV, OB, ...), from the [CODE] mark and the
opt-start/opt-end images; an empty list is the base.  The extraction is a reading of the pages' markup, not of the
standard's text: check.py's table is reviewed by a person, and a name it cannot have read right is corrected by hand
in OVERRIDES below.
"""

import html
import json
import re
import sys
from pathlib import Path

# Names the markup gives wrongly, by header: (kind, name) to drop.
OVERRIDES = {
    "signal.h.html": [("constants", "Any")],
    # Named in the prose of the limits, the widths or the regoff_t range, not defined by these headers.
    "inttypes.h.html": [("variables", "return")],
    "fcntl.h.html": [("structs", "termios")],
    "limits.h.html": [("structs", "iovec"), ("types", "sigset_t"), ("types", "ssize_t")],
    "regex.h.html": [("types", "ssize_t")],
    "unistd.h.html": [("types", name) for name in ("blksize_t", "cc_t", "long", "mode_t", "nfds_t", "ptrdiff_t", "speed_t",
                                                    "suseconds_t", "tcflag_t", "wchar_t", "wint_t")],
    "stdarg.h.html": [("variables", "argno"), ("variables", "array"), ("functions", "execl"), ("functions", "execv"), ("functions", "while")],
    "stdint.h.html": [("types", "_t"), ("types", "uint24_t"), ("types", "ptrdiff_t"), ("types", "sig_atomic_t"),
                      ("types", "size_t"), ("types", "wchar_t"), ("types", "wint_t")],
}

# Names the markup splits (an italic N inside a bold type), by header: (kind, name) to add.
ADDITIONS = {
    "stdint.h.html": [("types", f"{prefix}{width}_t") for prefix in ("int", "uint", "int_least", "uint_least", "int_fast", "uint_fast")
                      for width in (8, 16, 32, 64)],
}

# Words the markup sets in bold that are not names the header defines (a note's heading, a keyword in the prose).
NOISE = {"Note", "Identifier", "register", "typedef", "int", "uint", "int_fast", "int_least", "uint_fast", "uint_least"}

TOKEN = re.compile(
    r"(?P<mark><sup>\[<a href=\"javascript:open_code\('(?P<code>[A-Z0-9|]+)'\)\">[A-Z0-9|]+</a>\]</sup>)"
    r"|(?P<start><img src=\"\.\./images/opt-start\.gif\"[^>]*>)"
    r"|(?P<end><img src=\"\.\./images/opt-end\.gif\"[^>]*>)"
    r"|(?P<dt><dt>(?P<dtbody>.*?)</dt>)"
    r"|(?P<pre><pre>(?P<prebody>.*?)</pre>)"
    r"|(?P<row><tr valign=\"top\">(?P<rowbody>(?:(?!</tr>).)*?<td.*?)</tr>)"
    r"|(?P<heading><h5>(?P<headingbody>.*?)</h5>)"
    r"|(?P<bold><b>(?P<boldbody>[^<]*)</b>(?=(?P<after>(?:(?!</p>|</dt>|</dd>|<pre>)[^.])*)))",
    re.S)
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
KEYWORDS = {"int", "char", "void", "long", "short", "unsigned", "signed", "const", "struct", "union", "volatile",
            "restrict", "double", "float", "enum", "extern", "static", "_Noreturn", "noreturn", "inline", "size_t"}


def text(fragment):
    """The text of a markup fragment: tags dropped, entities read, blanks folded."""
    plain = re.sub(r"<[^>]+>", "", fragment)
    return " ".join(html.unescape(plain).split())


def names_of(fragment):
    """The names a fragment of markup stands for: an italic N is each of the widths 8, 16, 32 and 64."""
    if "<i>N</i>" not in fragment:
        return [text(fragment)]
    return [text(fragment.replace("<i>N</i>", str(width))) for width in (8, 16, 32, 64)]


def declaration_name(declaration):
    """The name a C declaration declares (the function, or the member/variable)."""
    pointer = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)?\s*\(", declaration)
    returns_pointer = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\(", declaration)
    if returns_pointer:
        return returns_pointer.group(1), True
    call = re.search(r"([A-Za-z_]\w*)\s*\(", declaration)
    if call and not pointer:
        return call.group(1), True
    if pointer:
        return pointer.group(1), False
    words = IDENT.findall(re.sub(r"\[[^\]]*\]", "", declaration))
    words = [word for word in words if word not in KEYWORDS]
    if not words:
        return None, False
    return words[-1], False


def read_page(path):
    """The names of one header page."""
    page = path.read_text(encoding="utf-8", errors="replace")
    start = page.find(">DESCRIPTION</h4>")
    end = page.find(">APPLICATION USAGE</h4>", start)
    if end < 0:
        end = page.find("<hr>", start)
    body = page[start:end]
    found = {"types": {}, "structs": {}, "constants": {}, "limits": {}, "macros": {}, "functions": {}, "variables": {}, "others": {}}
    options = []
    pending = None
    last_struct = None
    may_omit = False

    def add(kind, name, extra=None):
        if not name or name in found[kind] or name in NOISE:
            return
        entry = {"options": sorted(set(options))}
        if extra:
            entry.update(extra)
        found[kind][name] = entry

    for match in TOKEN.finditer(body):
        if match.group("mark"):
            pending = match.group("code")
            continue
        if match.group("start"):
            options.append(pending or "?")
            pending = None
            continue
        if match.group("end"):
            if options:
                options.pop()
            continue
        if match.group("heading"):
            # <limits.h>: the values that may be left out (possibly indeterminate, per path, increasable at run time).
            heading = text(match.group("headingbody"))
            may_omit = heading.startswith(("Runtime Invariant Values (Possibly Indeterminate)", "Pathname Variable Values", "Runtime Increasable Values"))
            continue
        if match.group("dt"):
            item = match.group("dtbody")
            if "<i>N</i>" in item and "<b>" not in item:
                for words in names_of(item):
                    name = IDENT.search(words)
                    if name:
                        add("limits" if words.startswith("{") else "constants", name.group(0), {"may_omit": False})
                continue
            words = text(item)
            if "<b>" in item:
                # A data types list: the bold name is a type.
                name = IDENT.match(words)
                if name:
                    add("types", name.group(0))
                continue
            if words.startswith("[") and words.endswith("]"):
                # <errno.h>'s [NAME].
                words = words[1:-1]
            if words.startswith("{"):
                name = IDENT.search(words)
                if name:
                    add("limits", name.group(0), {"may_omit": may_omit})
                continue
            name = IDENT.match(words)
            if not name or len(name.group(0)) == 1:
                continue
            if words[name.end():].lstrip().startswith("("):
                add("macros", name.group(0))
            else:
                add("constants", name.group(0))
            continue
        if match.group("row"):
            # A table's row: its first column, or every column when each holds one name (<inttypes.h>).  Upper-case
            # names are constants, names ending in _t types, the rest (type-generic and atomic names) are listed for a
            # person to classify.
            cells = re.findall(r"<td[^>]*>\s*<p class=\"tent\">(.*?)</p>", match.group("rowbody"), re.S)
            if not cells:
                continue
            every = all(IDENT.fullmatch(text(cell)) for cell in cells)
            for cell in cells if every else cells[:1]:
                for words in names_of(cell):
                    name = IDENT.fullmatch(words.split(" ")[0]) if words else None
                    if not name or len(name.group(0)) == 1:
                        continue
                    if name.group(0).endswith("_t"):
                        add("types", name.group(0))
                    elif name.group(0)[0].isupper():
                        add("constants", name.group(0))
                    else:
                        add("others", name.group(0))
            continue
        if match.group("pre"):
            block = match.group("prebody")
            segments = [text(segment) for segment in re.findall(r"<tt>(.*?)</tt>", block, re.S)]
            joined = " ".join(segment for segment in segments if segment)
            if ";" in joined:
                for declaration in joined.split(";"):
                    declaration = declaration.strip()
                    if not declaration:
                        continue
                    if "{" in declaration and "}" not in declaration:
                        # A structure's body cut by its members' ";": not a declaration to read.
                        continue
                    if "{" in declaration:
                        # A type defined in place (<search.h>'s "enum { FIND, ENTER } ACTION"): the type and its constants.
                        inside = declaration[declaration.index("{") + 1:declaration.rindex("}")]
                        for constant in IDENT.findall(inside):
                            add("constants", constant)
                        add("types", IDENT.findall(declaration[declaration.rindex("}") + 1:])[-1])
                        continue
                    name, is_function = declaration_name(declaration)
                    if is_function and declaration.startswith("#define"):
                        continue
                    if is_function:
                        add("functions", name)
                    elif declaration.startswith("extern"):
                        add("variables", name)
                    elif name:
                        add("variables", name)
            elif last_struct is not None:
                for segment in segments:
                    if not segment:
                        continue
                    name, _ = declaration_name(segment)
                    if segment.startswith(("extern ", "const struct ")):
                        # <netinet/in.h>'s in6addr_any and in6addr_loopback: variables listed like members.
                        add("variables", name)
                        continue
                    if name and name not in found["structs"][last_struct]["members"]:
                        found["structs"][last_struct]["members"].append(name)
            continue
        if match.group("bold"):
            name = match.group("boldbody").strip()
            after = match.group("after")
            if not IDENT.fullmatch(name):
                continue
            # The rest of the sentence says what the bold name is: a type, a structure or a union (a type that is a
            # structure, as siginfo_t, is both, and its members are listed after it).
            sentence = text(after)
            word = re.search(r"\b(types?|structures?|unions?)\b", sentence)
            kind = word.group(1).rstrip("s") if word else None
            if name.endswith("_t"):
                if word is None or "type" in sentence or kind == "type":
                    add("types", name)
                if re.search(r"\b(structure|union)\b", sentence):
                    add("structs", name, {"members": [], "union": "union" in sentence, "typedef": True})
                    last_struct = name
                continue
            if kind == "type":
                add("types", name)
            elif kind in ("structure", "union"):
                add("structs", name, {"members": [], "union": kind == "union"})
                last_struct = name
            continue

    for kind, name in OVERRIDES.get(path.name, []):
        found[kind].pop(name, None)
    options = []
    for kind, name in ADDITIONS.get(path.name, []):
        add(kind, name)
    return found


def main():
    """Reads every page and writes the list."""
    pages = Path(sys.argv[1])
    result = {}
    for path in sorted(pages.glob("*.h.html")):
        header = path.name[:-len(".html")].replace("_", "/", 1) if path.name.startswith(("sys_", "arpa_", "net_", "netinet_")) else path.name[:-len(".html")]
        result[header] = read_page(path)
    Path(sys.argv[2]).write_text(json.dumps(result, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    counts = {kind: sum(len(names[kind]) for names in result.values()) for kind in ("types", "structs", "constants", "limits", "macros", "functions", "variables", "others")}
    print(f"{len(result)} headers: " + ", ".join(f"{kind} {count}" for kind, count in counts.items()))


if __name__ == "__main__":
    main()
