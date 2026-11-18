#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Turn a pile of compatibility reports into a work list.

    tools/compat-report.sh <corpus>/*.el   # produces the reports
    tools/missing.py <report-dir>          # produces this

Reads the reports a scan wrote (docs/design.md 9.1), collects every name
that came up missing, asks a running GNU Emacs what each one *is*
(tools/classify.el), and sorts the answers into the buckets that decide
who implements them.

The buckets, and why the line falls where it does:

  evaluator      A special form. Nothing outside the interpreter can
                 provide it.
  core           A C primitive of Emacs that touches editor state --
                 buffer text, point, markers, windows, files, processes,
                 search. These are what napi.def is for; writing them in
                 Lisp on top of nothing is not possible.
  core-or-elisp  A C primitive that is a pure function over lists,
                 strings and numbers. Policy decision (2026-08-10): if
                 Emacs implements it in C, remacs treats it as core, so
                 this bucket is folded into core for planning. The
                 split is still reported for reference.
  elisp          A macro or a Lisp function. Emacs writes it in Lisp and
                 so can remacs -- the prelude, or a library.
  out-of-scope   A C primitive belonging to a subsystem docs/design.md 1
                 declares a non-goal: coding systems, faces, images, the
                 window system.
  not-in-emacs   Emacs does not have this name either. It belongs to the
                 package that mentioned it, and is not a gap in remacs.

The classification is not a judgement call: "C primitive or Lisp" comes
from subrp plus subr-native-elisp-p, and the subsystem from the C source
file name Emacs reports for it. Both are runtime observations.

Attributing a name by its C file is a first cut, not a verdict. A file
is mostly one subsystem but not entirely -- ding and sleep-for live in
dispnew.c beside the redisplay machinery remacs has no use for. The
"review" bucket collects whatever falls outside the known files; the
misfiled remainder is small enough to read.
"""

import collections
import os
import re
import subprocess
import sys

SECTIONS = [
    "unsupported reader syntax",
    "missing special forms and macros",
    "missing functions",
    "unbound variables",
    "missing functions in code that did not run",
]
# Sections that name functions; the others are counted but not classified.
FUNCTION_SECTIONS = [
    "missing special forms and macros",
    "missing functions",
    "missing functions in code that did not run",
]

# Emacs C files, by what they mean for remacs.
CORE = {
    "src/editfns.c", "src/buffer.c", "src/insdel.c", "src/marker.c",
    "src/search.c", "src/window.c", "src/fileio.c", "src/dired.c",
    "src/process.c", "src/callproc.c", "src/keyboard.c", "src/keymap.c",
    "src/minibuf.c", "src/casefiddle.c", "src/indent.c", "src/textprop.c",
    "src/undo.c", "src/cmds.c", "src/syntax.c", "src/macros.c",
    "src/term.c", "src/terminal.c", "src/filelock.c", "src/casetab.c",
    "src/category.c", "src/lread.c", "src/print.c", "src/callint.c",
    # Character width is core here: redisplay already computes East
    # Asian Width to lay out a line (docs/design.md 6, 11).
    "src/character.c",
}
PURE = {
    "src/fns.c", "src/data.c", "src/alloc.c", "src/eval.c",
    "src/floatfns.c", "src/timefns.c", "src/chartab.c", "src/bignum.c",
    "src/sort.c", "src/charset.c",
}
OUT = {
    "src/coding.c", "src/fontset.c", "src/xfaces.c", "src/xdisp.c",
    "src/image.c", "src/xterm.c", "src/xfns.c", "src/xselect.c",
    "src/xmenu.c", "src/gtkutil.c", "src/font.c", "src/ftfont.c",
    "src/composite.c", "src/emacs-module.c", "src/dbusbind.c",
    "src/gnutls.c", "src/xwidget.c", "src/sound.c", "src/xrdb.c",
    "src/xsmfns.c", "src/frame.c", "src/dispnew.c", "src/menu.c",
    "src/scroll.c", "src/xgselect.c", "src/inotify.c", "src/json.c",
    "src/treesit.c", "src/pdumper.c", "src/sqlite.c", "src/comp.c",
}


def read_reports(directory):
    """name -> (uses, files) per section."""
    uses = {s: collections.Counter() for s in SECTIONS}
    files = {s: collections.Counter() for s in SECTIONS}
    n = 0
    for fn in sorted(os.listdir(directory)):
        if not fn.endswith(".txt"):
            continue
        n += 1
        section = None
        with open(os.path.join(directory, fn), encoding="utf-8",
                  errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                m = re.match(r"^(.*?) \((\d+)\)$", line)
                if m and m.group(1) in SECTIONS:
                    section = m.group(1)
                    continue
                if line.startswith("forms that did not complete"):
                    section = None
                    continue
                if section and line.startswith("  "):
                    m = re.match(r"^  (.+?)\s+(\d+)$", line)
                    if m:
                        uses[section][m.group(1)] += int(m.group(2))
                        files[section][m.group(1)] += 1
                        continue
                section = None
    return uses, files, n


def classify(names, repo_root):
    """Ask Emacs what each name is. Returns name -> row dict."""
    script = os.path.join(repo_root, "tools", "classify.el")
    proc = subprocess.run(
        ["emacs", "-Q", "--batch", "-l", script, "-f", "classify-run"],
        input="\n".join(names) + "\n",
        capture_output=True, text=True)
    out = {}
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        name, kind, csrc, arity, args, doc = parts[:6]
        out[name] = dict(kind=kind, csrc=csrc, arity=arity, args=args,
                         doc=doc)
    return out


def bucket(info):
    kind = info["kind"]
    if kind == "unbound":
        return "not-in-emacs"
    if kind == "special":
        return "evaluator"
    if kind in ("macro", "lisp"):
        return "elisp"
    csrc = info["csrc"]
    if csrc in CORE:
        return "core"
    if csrc in PURE:
        return "core-or-elisp"
    if csrc in OUT:
        return "out-of-scope"
    return "review"


ORDER = ["evaluator", "core", "core-or-elisp", "elisp", "review",
         "out-of-scope", "not-in-emacs"]

# ---------------------------------------------------------------------
# The platform dimension.
#
# A core function is only implementable when the Noct VM offers the OS
# access it needs. The VM's surface today (surveyed from the api-*.c
# registration tables): FileUtil (text read/write, existence, size,
# directory listing, cwd, home), Process (spawn/read/write/kill/wait),
# System (env, runCommand/shell, pcall, import), Term, Regex, Thread,
# and the String/Array/Dict/Packed/Math intrinsics.  What is absent is
# named below; docs/noct-gaps.md is the prose version of this table.
#
# A function not named here and not in a NEEDS_CSRC subsystem is
# implementable now: buffer text, windows, keymaps, text properties,
# syntax tables, the reader and printer are all editor state, and the
# regex layer (Emacs-syntax translation + match data) already runs on
# Regex.*.
# ---------------------------------------------------------------------

# Whole subsystems that are one missing platform API.
NEEDS_CSRC = {
    "src/timefns.c": "time",
}

NEEDS = {
    # Wall-clock time; nothing in the VM tells the time of day.
    "time": {
        "file-newer-than-file-p", "set-file-times", "visited-file-modtime",
        "verify-visited-file-modtime", "set-visited-file-modtime",
    },
    # stat(2)/access(2): metadata beyond FileUtil.getFileSize.
    "file-stat": {
        "file-attributes", "directory-files-and-attributes",
        "file-directory-p", "file-regular-p", "file-symlink-p",
        "file-modes", "default-file-modes", "set-default-file-modes",
        "file-executable-p", "file-writable-p", "file-readable-p",
        "file-accessible-directory-p", "access-file",
        "file-acl", "set-file-acl", "file-selinux-context",
        "set-file-selinux-context",
    },
    # rename/unlink/copy/mkdir/symlink/chmod.
    "file-manage": {
        "copy-file", "rename-file", "add-name-to-file",
        "make-symbolic-link", "set-file-modes",
        "make-temp-file-internal", "unix-sync",
    },
    # File locking (lockf/flock).
    "file-lock": {
        "file-locked-p", "lock-buffer", "unlock-buffer",
    },
    # Sockets.
    "net": {
        "make-network-process", "format-network-address",
        "network-interface-list", "network-lookup-address-info",
        "set-network-process-option",
    },
    # Arbitrary signals to children (Process.kill only terminates).
    "proc-signal": {
        "signal-process", "interrupt-process", "stop-process",
        "continue-process", "quit-process",
    },
    # Process identity/inspection beyond the Process handle.
    "proc-misc": {
        "process-id", "process-tty-name", "set-process-window-size",
        "list-system-processes", "process-attributes", "num-processors",
    },
    # /etc/passwd group enumeration.
    "os-users": {
        "system-users", "system-groups",
    },
}

NEEDS_BY_NAME = {}
for _gap, _names in NEEDS.items():
    for _n in _names:
        NEEDS_BY_NAME[_n] = _gap


def platform_of(name, info):
    if info["csrc"] in NEEDS_CSRC:
        return NEEDS_CSRC[info["csrc"]]
    return NEEDS_BY_NAME.get(name, "")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: tools/missing.py <report-dir> [--tsv out.tsv]")
    directory = sys.argv[1]
    tsv_path = None
    if "--tsv" in sys.argv:
        tsv_path = sys.argv[sys.argv.index("--tsv") + 1]
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    uses, files, nfiles = read_reports(directory)

    # One entry per name, with the strongest evidence it appeared under.
    merged_files = collections.Counter()
    merged_uses = collections.Counter()
    for s in FUNCTION_SECTIONS:
        for name, c in files[s].items():
            merged_files[name] = max(merged_files[name], c)
            merged_uses[name] = max(merged_uses[name], uses[s][name])

    names = sorted(merged_files)
    print("reports: %d   distinct names: %d" % (nfiles, len(names)),
          file=sys.stderr)
    info = classify(names, repo_root)

    rows = []
    for name in names:
        i = info.get(name, dict(kind="unknown", csrc="", arity="", args="",
                                doc=""))
        b = bucket(i)
        # Policy: a C primitive is core, pure function or not.
        if b == "core-or-elisp":
            b = "core"
        i["platform"] = platform_of(name, i) if b == "core" else ""
        rows.append((b, merged_files[name], merged_uses[name], name, i))
    rows.sort(key=lambda r: (ORDER.index(r[0]) if r[0] in ORDER else 99,
                             -r[1], -r[2], r[3]))

    counts = collections.Counter(r[0] for r in rows)
    print("\n=== buckets ===")
    for b in ORDER:
        if counts[b]:
            print("  %-14s %5d names" % (b, counts[b]))

    gaps = collections.Counter(i["platform"] for _b, _f, _u, _n, i in rows
                               if _b == "core" and i["platform"])
    now = sum(1 for _b, _f, _u, _n, i in rows
              if _b == "core" and not i["platform"])
    print("\n=== core by platform support ===")
    print("  %-14s %5d names" % ("ready-now", now))
    for g, c in gaps.most_common():
        print("  needs:%-8s %5d names" % (g, c))

    for b in ORDER:
        sel = [r for r in rows if r[0] == b]
        if not sel or b in ("not-in-emacs", "out-of-scope"):
            continue
        print("\n=== %s (%d) ===" % (b, len(sel)))
        for _, nf, nu, name, i in sel[:40]:
            print("  %-30s %3d files  %-14s %-9s %s"
                  % (name, nf, i["csrc"].replace("src/", ""), i["arity"],
                     i["args"][:44]))
        if len(sel) > 40:
            print("  ... and %d more" % (len(sel) - 40))

    if tsv_path:
        with open(tsv_path, "w", encoding="utf-8") as f:
            f.write("bucket\tplatform\tfiles\tuses\tname\tkind\tcsrc\tarity\targs\tdoc\n")
            for b, nf, nu, name, i in rows:
                f.write("%s\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n"
                        % (b, i.get("platform", ""), nf, nu, name, i["kind"],
                           i["csrc"], i["arity"], i["args"], i["doc"]))
        print("\nwrote %s" % tsv_path, file=sys.stderr)


if __name__ == "__main__":
    main()
