#!/usr/bin/env python3
# ws035-p001: file-level move map and reference inventory for the WS035 refactor.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Regenerate the refactor move map and the references to every moved file.

usage: python3 plan/ws035/tests/refactor-refs.py [--out DIR] [--map-md FILE]

Moves (p002 -> p003 -> p023 -> p004, see plan/ws035/refactor-map.md):
  p002  include/drivers/*                  -> include/drivers/<src/drivers layout>/*
  p003  libc/* (not include/libc)          -> src/libc/*
  p023  include/libc/*                     -> include/*
  p004  include/kern/*, include/kern/{rpi4,sun4u}/boot.h -> include/kern/*

The inventory is computed on the current tree (git ls-files).  After each
refactor Phase lands, run it again: the rules still select the files of the
Phases that have not moved yet, and a moved file no longer appears.

Outputs (in --out, default plan/ws035/refactor-refs/):
  moves.tsv         phase, old path, new path, note
  collisions.tsv    phase, kind, path, detail
  refs.tsv          phase, kind, file, line, target(old), change, text
  hal-lines.tsv     the refs.tsv rows in src/hal/ or include/hal/
  summary.json      counts per phase / kind / target, HAL counts
and the generated tables between the markers of --map-md.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

# p002: include/drivers/<file> -> include/drivers/<dir under src/drivers>/<file>.
# The directory is that of the implementing source (named in the note).  The
# basename is kept (a mechanical move) unless a third element names the new
# basename.
P002 = {
    "disklabel.h": ("disklabel", "src/drivers/disklabel/*.c"),
    "dma.h": ("generic", "src/drivers/generic/dma.c"),
    "dp8390.h": ("ethernet", "src/drivers/ethernet/dp8390.c"),
    "gpu-display.h": ("gpu", "src/drivers/gpu/gpu.c"),
    "gpu-fence.h": ("gpu", "src/drivers/gpu/gpu-fence.c"),
    "gpu-scanout.h": ("gpu", "src/drivers/gpu/gpu.c"),
    "gpu-share.h": ("gpu", "src/drivers/gpu/gpu.c"),
    "gpu.h": ("gpu", "src/drivers/gpu/gpu.c"),
    # ws035-p002 (user, 2026-09-23): the PCI registration headers follow the
    # PCI driver naming rule; these two are renamed as well as moved.
    "i915.h": ("pci", "src/drivers/gpu/i915/ (PCI registration)", "pci-i915.h"),
    "venus.h": ("pci", "src/drivers/gpu/venus/venus.c (PCI registration)",
                "pci-venus.h"),
    "graphics/pc98.h": ("platform/pc98/graphics",
                        "src/drivers/platform/pc98/graphics/pc98-graphics.c"),
    "graphics/pcat.h": ("platform/pcat/graphics",
                        "src/drivers/platform/pcat/graphics/pcat-graphics.c"),
    "hid/hid-report.h": ("usb", "src/drivers/usb/usb-hid.c (only kernel user)"),
    "hid/pc98-busmouse.h": ("platform/pc98",
                            "src/drivers/platform/pc98/pc98-busmouse.c"),
    "hid/ps2-8042.h": ("platform/pcat", "src/drivers/platform/pcat/ps2-8042.c"),
    "pc98-lgy98.h": ("platform/pc98", "src/drivers/platform/pc98/pc98-lgy98.c"),
    "pcat-ide.h": ("platform/pcat", "src/drivers/platform/pcat/pcat-ide.c"),
    "pcat-ne2000.h": ("isa", "src/drivers/isa/ne2000.c"),
    "pci-ehci.h": ("pci", "src/drivers/pci/pci-ehci.c"),
    "pci-intel-ax211.h": ("wifi/intel-ax211",
                          "src/drivers/wifi/intel-ax211/intel-ax211.c"),
    "pci-nvme-protocol.h": ("pci", "src/drivers/pci/pci-nvme.c"),
    "pci-nvme.h": ("pci", "src/drivers/pci/pci-nvme.c"),
    "pci-pcat.h": ("pci", "src/drivers/pci/pci-pcat.c"),
    "pci-uhci.h": ("pci", "src/drivers/pci/pci-uhci.c"),
    "pci-xhci-capability.h": ("pci", "src/drivers/pci/pci-xhci.c"),
    "pci-xhci-control.h": ("pci", "src/drivers/pci/pci-xhci.c"),
    "pci-xhci-lifecycle.h": ("pci", "src/drivers/pci/pci-xhci.c"),
    "pci-xhci.h": ("pci", "src/drivers/pci/pci-xhci.c"),
    "pci.h": ("pci", "src/drivers/pci/pci.c"),
    "usb-cdc-ecm.h": ("usb", "src/drivers/usb/usb-cdc-ecm.c"),
    "usb-cdc-ncm.h": ("usb", "src/drivers/usb/usb-cdc-ncm.c"),
    "usb-hid.h": ("usb", "src/drivers/usb/usb-hid.c"),
    "usb-rtl8822bu.h": ("usb", "src/drivers/usb/usb-rtl8822bu.c"),
    "usb-storage-bot.h": ("usb", "src/drivers/usb/usb-storage.c"),
    "usb-storage-scsi.h": ("usb", "src/drivers/usb/usb-storage.c"),
    "usb-storage.h": ("usb", "src/drivers/usb/usb-storage.c"),
    "usb-uas.h": ("usb", "src/drivers/usb/usb-uas*.c"),
    "usb.h": ("usb", "src/drivers/usb/usb.c"),
}

P004 = {
    "include/kern/boot.h": "include/kern/boot.h",
    "include/kern/boot.h": "include/kern/boot.h",
    "include/kern/boot.h": "include/kern/pc98-handoff.h",
    "include/kern/boot.h": "include/kern/boot.h",
    "include/kern/boot.h": "include/kern/boot.h",
    "include/kern/boot.h": "include/kern/boot.h",
}

PHASE_ORDER = ["p002", "p003", "p023", "p004"]

# -I roots in the order the kernel/user builds search them today.  The
# resolver only needs to know which spelling reaches which file.
ROOTS = ["include", "src", ".", "include/libc"]

INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|include_next|import)\s*([<"])([^>"]+)[>"]')
TOKEN_RE = re.compile(r"[A-Za-z0-9_./$(){}@%*+-]+")


def git_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, check=True,
                         capture_output=True).stdout
    return [p for p in out.decode().split("\0") if p]


def p002_new_path(rel):
    rule = P002[rel]
    base = rule[2] if len(rule) > 2 else rel.split("/")[-1]
    return f"include/drivers/{rule[0]}/{base}"


def build_moves(files):
    """Return (pending, done).  A move is done when its old path is gone and
    its new path is tracked; the stale-reference scan then looks for the old
    spelling (see stale_refs)."""
    fileset = set(files)
    moves = []  # (phase, old, new, note)
    done = []
    p002_new = {p002_new_path(rel): rel for rel in P002}
    for f in files:
        if f.startswith("include/drivers/"):
            rel = f[len("include/drivers/"):]
            if rel in P002:
                moves.append(("p002", f, p002_new_path(rel), P002[rel][1]))
            elif f in p002_new:
                pass
            else:
                raise SystemExit(f"p002: no rule for {f}")
        elif f.startswith("include/libc/"):
            moves.append(("p023", f, "include/" + f[len("include/libc/"):], ""))
        elif f.startswith("libc/"):
            moves.append(("p003", f, "src/" + f, ""))
        elif f in P004:
            moves.append(("p004", f, P004[f], ""))
    for rel in sorted(P002):
        old, new = "include/drivers/" + rel, p002_new_path(rel)
        if old in fileset:
            continue
        if new not in fileset:
            raise SystemExit(f"p002: neither {old} nor {new} is tracked")
        done.append(("p002", old, new, P002[rel][1]))
    for old, new in sorted(P004.items()):
        if old not in fileset and new in fileset:
            done.append(("p004", old, new, ""))
    return moves, done


def include_dirs_from_build(files):
    """Every literal -I directory named by a build file (no make variables)."""
    dirs = set()
    for f in files:
        if not is_build_file(f):
            continue
        try:
            text = (REPO / f).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in re.finditer(r"(?:^|(?<=[\s\"'=]))(?:-I\s*|-isystem\s+|-iquote\s*|-idirafter\s+)"
                             r"([A-Za-z0-9_./-]+)", text, re.M):
            d = m.group(1).rstrip("/")
            if d and not d.startswith("/") and "$" not in d:
                dirs.add(d)
    return sorted(dirs)


def is_build_file(f):
    base = os.path.basename(f)
    return (base == "Makefile" or base.endswith(".mk") or base.endswith(".cmake")
            or base == "CMakeLists.txt")


def classify(f):
    base = os.path.basename(f)
    if f.startswith("plan/") and "/tests/" in f:
        return "plan-tests"
    if f.startswith("plan/"):
        return "plan-records"
    if f == "toolchain/llvm/sysroot.mk":
        return "sysroot"
    if is_build_file(f):
        return "build"
    if base.endswith(".noct"):
        return "noct"
    if "generated" in base:
        return "noct-generated"
    if base.endswith((".c", ".h", ".S", ".s", ".inc", ".cc", ".cpp")):
        return "source-text"
    if base.endswith(".md") or f.startswith("docs/"):
        return "docs"
    if base.endswith((".py", ".sh")) or f.startswith(".github/"):
        return "scripts"
    return "other"


def is_hal(f):
    return f.startswith("src/hal/") or f.startswith("include/hal/")


def resolve_include(including, delim, name, fileset):
    """Return (target, how) where how is 'rel' or a root, or (None, None)."""
    if delim == '"':
        cand = os.path.normpath(os.path.join(os.path.dirname(including), name))
        if cand in fileset:
            return cand, "rel"
    for root in ROOTS:
        cand = os.path.normpath(os.path.join(root, name))
        if cand in fileset:
            return cand, root
    return None, None


def new_location(path, newmap):
    return newmap.get(path, path)


def new_spelling(including, delim, name, target, newmap, newfiles, how):
    """The spelling after the move, in the same style: unchanged when it still
    reaches the moved file from the (moved) including file; a directory-
    relative include stays directory-relative; a root-relative one stays
    relative to the same -I root (the first root that holds the file when
    the old root no longer does, e.g. include/libc -> include)."""
    tnew = new_location(target, newmap)
    inew = new_location(including, newmap)
    after, _how = resolve_include(inew, delim, name, newfiles)
    if after == tnew:
        return name
    if how == "rel":
        return os.path.relpath(tnew, os.path.dirname(inew) or ".")
    if how == ".":
        return tnew
    if tnew.startswith(how + "/"):
        return tnew[len(how) + 1:]
    for root in ROOTS:
        if root != "." and tnew.startswith(root + "/"):
            return tnew[len(root) + 1:]
    return tnew


PREFIX_OK = re.compile(
    r"(-I|-isystem|-iquote)?"
    r"(\./|(\.\./)+|\$[({][A-Za-z_0-9]+[)}]/((obj|kern64|kernel|user|user32|user64|dynamic)/)*"
    # ws035-p002: shell variables ($repo/, $root/) and string concatenation
    # (repo + "/include/...") name the repository root as well
    r"|\$[A-Za-z_][A-Za-z_0-9]*/|/"
    r"|.*/zedBSD[^/]*/)?")


def path_token_targets(line, moves_by_old, dir_prefixes):
    """(target, phase-key) for every moved file or directory a text line names.

    A name counts when it is the whole path or follows only a repository-root
    form (./, ../, $(VAR)/, -I, a build-output directory); userland/base/libc/
    and bootloader/include/ are other trees and do not count."""
    hits = []
    for m in NAME_RE.finditer(line):
        start = m.start()
        left = start
        while left > 0 and re.match(r"[A-Za-z0-9_./$(){}-]", line[left - 1]):
            left -= 1
        prefix = line[left:start]
        if not PREFIX_OK.fullmatch(prefix):
            continue
        right = m.end()
        while right < len(line) and re.match(r"[A-Za-z0-9_./*%+-]", line[right]):
            right += 1
        name = line[start:right].rstrip(".,:;")
        if name in moves_by_old:
            hits.append(name)
            continue
        for d in dir_prefixes:
            if name == d or name.startswith(d + "/") or name.startswith(d + "*") \
                    or name.startswith(d + "%"):
                if d == "libc" and not name.startswith("libc/"):
                    break
                hits.append(d + "/")
                break
    return hits


NAME_RE = re.compile(r"(libc/|include/(drivers|boot|kern/rpi4|kern/sun4u)\b)")


STALE_EXEMPT_FILES = {
    # the move table itself names every old path
    "plan/ws035/tests/refactor-refs.py",
    # the dated handoff sections are records (the p002 match is the q314 p002
    # history, line "include/drivers/i915.h: drv_i915_pci_driver_register()")
    "AGENTS.md",
}


def stale_exempt(kind, f):
    """References a finished move leaves alone: plan records (past results are
    not rewritten), the unbuilt i915-old/ reference copy, and this tool."""
    return (kind == "plan-records" or "/i915-old/" in f
            or f in STALE_EXEMPT_FILES)


def stale_refs(files, fileset, done):
    """Every line that still names the old path of a finished move."""
    olds = {old: (p, new) for p, old, new, _n in done}
    if not olds:
        return []
    virtual = fileset | set(olds)
    live_dirs = set()
    for f in files:
        d = os.path.dirname(f)
        while d and d not in live_dirs:
            live_dirs.add(d)
            d = os.path.dirname(d)
    gone_dirs = {}
    for old, (p, _new) in olds.items():
        d = os.path.dirname(old)
        if d not in live_dirs:
            gone_dirs[d] = p
    dir_list = sorted(gone_dirs, key=len, reverse=True)
    rows = []
    for f in files:
        try:
            data = (REPO / f).read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        text = data.decode("utf-8", errors="replace")
        kind = classify(f)
        c_like = f.endswith((".c", ".h", ".S", ".s", ".inc", ".cc", ".cpp"))
        for lineno, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line) if c_like else None
            if m:
                delim, name = m.group(1), m.group(2)
                target, how = resolve_include(f, delim, name, virtual)
                if target in olds:
                    new = olds[target][1]
                    if how == "rel":
                        spelled = os.path.relpath(new, os.path.dirname(f) or ".")
                    elif how != "." and new.startswith(how + "/"):
                        spelled = new[len(how) + 1:]
                    else:
                        spelled = new
                    rows.append((olds[target][0], kind, f, lineno, target,
                                 f"stale: {name} -> {spelled}", line.strip()))
                continue
            if not any(o in line for o in olds) and \
                    not any(d in line for d in dir_list):
                continue
            seen = set()
            for hit in path_token_targets(line, olds, dir_list):
                if hit in seen:
                    continue
                seen.add(hit)
                ph = gone_dirs[hit[:-1]] if hit.endswith("/") else olds[hit][0]
                new = "" if hit.endswith("/") else olds[hit][1]
                rows.append((ph, kind, f, lineno, hit, f"stale: path -> {new}",
                             line.strip()[:300]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="plan/ws035/refactor-refs")
    ap.add_argument("--map-md", default="plan/ws035/refactor-map.md")
    args = ap.parse_args()
    os.chdir(REPO)
    # The outputs of this tool are tracked since p001; scanning them would
    # count the previous inventory as references (refs.tsv grows with every
    # run).  They are left out of the scan (ws035-p002).
    own = {os.path.normpath(args.out) + "/", os.path.normpath(args.map_md or "-")}
    files = [f for f in git_files()
             if f not in own and not any(f.startswith(o) for o in own if o.endswith("/"))]
    fileset = set(files)
    moves, finished = build_moves(files)
    newmap = {old: new for _p, old, new, _n in moves}
    phase_of = {old: p for p, old, _new, _n in moves}
    moves_by_old = {old: (p, new) for p, old, new, _n in moves}
    newfiles = {newmap.get(f, f) for f in files}
    # directory-level names a build file may use instead of a file name
    dir_prefixes = {
        "include/drivers": "p002", "include/drivers/graphics": "p002",
        "include/drivers/hid": "p002",
        "include/libc": "p023", "libc/regex": "p003",
        "include/boot": "p004", "include/kern/rpi4": "p004",
        "include/kern/sun4u": "p004",
    }
    # a directory-level name counts only while its Phase still has files to
    # move out of that directory (after p002, include/drivers/ holds the new
    # layout and naming it is not a reference to a moved file)
    pending_dirs = {os.path.dirname(old) for _p, old, _new, _n in moves}
    dir_prefixes = {d: p for d, p in dir_prefixes.items()
                    if any(x == d or x.startswith(d + "/") for x in pending_dirs)}
    # 'libc/' alone (e.g. libc/%.c, libc/*.c) belongs to p003
    dir_prefix_list = sorted(dir_prefixes, key=len, reverse=True) + ["libc"]
    dir_prefixes["libc"] = "p003"

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    # ---- collisions -------------------------------------------------------
    collisions = []
    newset = defaultdict(list)
    for p, old, new, _n in moves:
        newset[new].append(old)
        if new in fileset and new not in newmap:
            collisions.append((p, "exists", new, f"target already tracked (from {old})"))
    for new, olds in newset.items():
        if len(olds) > 1:
            collisions.append(("*", "duplicate", new, " ".join(olds)))
    # a file and a directory of the same name at the target
    dirs_now = {os.path.dirname(f) for f in files}
    for p, old, new, _n in moves:
        if new in dirs_now:
            collisions.append((p, "file-vs-dir", new, "a directory of this name exists"))
    idirs = include_dirs_from_build(files)
    for p, old, new, _n in moves:
        if not new.startswith("include/"):
            continue
        rel = new[len("include/"):]
        for d in idirs:
            if d == "include":
                continue
            cand = os.path.normpath(os.path.join(d, rel))
            if cand in fileset and cand != old and cand not in newmap:
                collisions.append((p, "shadow", new,
                                   f"<{rel}> also exists as {cand} (-I{d}); "
                                   f"-Iinclude is searched first"))
    # p004: kern/boot.h (file) beside kern/boot/ (directory)
    if "include/kern/boot.h" in fileset:
        collisions.append(("p004", "name-beside", "include/kern/",
                           "include/kern/boot.h stays a file beside the new "
                           "directory (legal; see refactor-map.md)"))
    # p023: the top level names of include/libc against include/
    top_new = {m[2].split("/")[1] for m in moves if m[0] == "p023"}
    top_old = {f.split("/")[1] for f in files if f.startswith("include/")
               and not f.startswith("include/drivers/") or f.startswith("include/kern/")}
    for name in sorted(top_new & top_old):
        collisions.append(("p023", "top-level", f"include/{name}",
                           "a libc header name meets an existing include/ entry"))

    # ---- references ---------------------------------------------------------
    refs = []  # phase, kind, file, line, target, change, text
    for f in files:
        path = REPO / f
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        text = data.decode("utf-8", errors="replace")
        kind = classify(f)
        c_like = f.endswith((".c", ".h", ".S", ".s", ".inc", ".cc", ".cpp"))
        for lineno, line in enumerate(text.splitlines(), 1):
            done = set()
            m = INCLUDE_RE.match(line) if c_like else None
            if m:
                delim, name = m.group(1), m.group(2)
                target, how = resolve_include(f, delim, name, fileset)
                if target and target in newmap:
                    spelled = new_spelling(f, delim, name, target, newmap, newfiles, how)
                    change = "keep" if spelled == name else f"{name} -> {spelled}"
                    if change == "keep":
                        _t, how_after = resolve_include(new_location(f, newmap), delim,
                                                        name, newfiles)
                        if how_after != how:
                            change = f"keep-root {how}->{how_after}"
                    refs.append((phase_of[target], "c-include" if kind in
                                 ("source-text", "noct-generated") else kind + "/include",
                                 f, lineno, target, change, line.strip()))
                    done.add(target)
                elif (not target and f in newmap and delim == '"'):
                    pass
                # a moved including file whose relative include stays valid
                if target and f in newmap and target not in newmap and how == "rel":
                    spelled = new_spelling(f, delim, name, target, newmap, newfiles, how)
                    if spelled != name:
                        refs.append((phase_of[f], "c-include-from-moved", f, lineno,
                                     target, f"{name} -> {spelled}", line.strip()))
                continue
            if "libc/" not in line and "include/" not in line:
                continue
            if True:
                for hit in path_token_targets(line, moves_by_old, dir_prefix_list):
                    if hit in done:
                        continue
                    done.add(hit)
                    if hit.endswith("/"):
                        ph = dir_prefixes[hit[:-1]]
                        target = hit
                    else:
                        ph = phase_of[hit]
                        target = hit
                    refs.append((ph, kind, f, lineno, target, "path", line.strip()[:300]))

    refs.sort(key=lambda r: (PHASE_ORDER.index(r[0]), r[1], r[2], r[3]))

    with open(out / "moves.tsv", "w", encoding="utf-8") as fp:
        fp.write("phase\told\tnew\tnote\n")
        for p, old, new, note in sorted(moves, key=lambda m: (PHASE_ORDER.index(m[0]), m[1])):
            fp.write(f"{p}\t{old}\t{new}\t{note}\n")
    with open(out / "collisions.tsv", "w", encoding="utf-8") as fp:
        fp.write("phase\tkind\tpath\tdetail\n")
        for row in collisions:
            fp.write("\t".join(row) + "\n")
    with open(out / "refs.tsv", "w", encoding="utf-8") as fp:
        fp.write("phase\tkind\tfile\tline\ttarget\tchange\ttext\n")
        for r in refs:
            fp.write("\t".join(str(x).replace("\t", " ") for x in r) + "\n")
    hal = [r for r in refs if is_hal(r[2])]
    with open(out / "hal-lines.tsv", "w", encoding="utf-8") as fp:
        fp.write("phase\tkind\tfile\tline\ttarget\tchange\ttext\n")
        for r in hal:
            fp.write("\t".join(str(x).replace("\t", " ") for x in r) + "\n")

    stale = stale_refs(files, fileset, finished)
    stale.sort(key=lambda r: (PHASE_ORDER.index(r[0]), r[1], r[2], r[3]))
    with open(out / "stale.tsv", "w", encoding="utf-8") as fp:
        fp.write("phase\tkind\tfile\tline\ttarget(old)\tchange\ttext\texempt\n")
        for r in stale:
            ex = "exempt" if stale_exempt(r[1], r[2]) else "CHECK"
            fp.write("\t".join(str(x).replace("\t", " ") for x in (*r, ex)) + "\n")

    summary = {"moves": Counter(m[0] for m in moves),
               "moved": Counter(m[0] for m in finished),
               "collisions": [list(c) for c in collisions],
               "include_dirs_checked": idirs,
               "phases": {}}
    for ph in PHASE_ORDER:
        rows = [r for r in refs if r[0] == ph]
        need = [r for r in rows if not r[5].startswith("keep")]
        rooted = [r for r in rows if r[5].startswith("keep-root")]
        summary["phases"][ph] = {
            "refs": len(rows),
            "lines": len({(r[2], r[3]) for r in rows}),
            "lines_needing_edit": len({(r[2], r[3]) for r in need
                                       if r[1] != "plan-records"}),
            "lines_plan_records": len({(r[2], r[3]) for r in rows
                                       if r[1] == "plan-records"}),
            "include_root_changes": dict(Counter(r[5] for r in rooted)),
            "refs_needing_edit": len([r for r in need if r[1] not in ("plan-records",)]),
            "by_kind": dict(Counter(r[1] for r in rows)),
            "by_kind_needing_edit": dict(Counter(r[1] for r in need)),
            "files": len({r[2] for r in rows}),
            "hal_lines": len({(r[2], r[3]) for r in rows if is_hal(r[2])}),
            "hal_lines_needing_edit": len({(r[2], r[3]) for r in need if is_hal(r[2])}),
            "hal_files": sorted({r[2] for r in rows if is_hal(r[2])}),
            # src/drivers/gpu/i915-old/ is a reference copy that is not built
            # and that WS035 does not touch (a decision for the Phase)
            "i915_old_lines_needing_edit": len({(r[2], r[3]) for r in need
                                                if "/i915-old/" in r[2]}),
            "per_target": dict(Counter(r[4] for r in rows)),
            # finished moves: lines still naming an old path
            "moved": len([m for m in finished if m[0] == ph]),
            "stale_lines": len({(r[2], r[3]) for r in stale if r[0] == ph}),
            "stale_lines_checked": len({(r[2], r[3]) for r in stale if r[0] == ph
                                        and not stale_exempt(r[1], r[2])}),
        }
    with open(out / "summary.json", "w", encoding="utf-8") as fp:
        json.dump(summary, fp, indent=1, sort_keys=True, ensure_ascii=False)
        fp.write("\n")

    if args.map_md:
        write_map_tables(Path(args.map_md), moves, refs, collisions, summary, finished)
    print(json.dumps({ph: {k: v for k, v in s.items() if k in
                           ("lines", "lines_needing_edit", "lines_plan_records", "files",
                            "hal_lines", "hal_lines_needing_edit", "include_root_changes",
                            "moved", "stale_lines", "stale_lines_checked")}
                      for ph, s in summary["phases"].items()}, indent=1))
    print(f"collisions: {len(collisions)}")
    return 0


def write_map_tables(md, moves, refs, collisions, summary, done):
    start, end = "<!-- refactor-refs:start -->", "<!-- refactor-refs:end -->"
    ref_count = Counter(r[4] for r in refs if r[1] != "plan-records")
    rec_count = Counter(r[4] for r in refs if r[1] == "plan-records")
    edit_count = Counter(r[4] for r in refs if not r[5].startswith("keep")
                         and r[1] != "plan-records")
    lines = [start, "",
             "（この節は `python3 plan/ws035/tests/refactor-refs.py` が生成する。手で編集しない。）", ""]
    titles = {"p002": "p002: `include/drivers/*` → `src/drivers/` と同じ階層",
              "p003": "p003: `libc/` のsource → `src/libc/`",
              "p023": "p023: `include/libc/*` → `include/*`",
              "p004": "p004: boot定義 → `include/kern/`"}
    for ph in PHASE_ORDER:
        rows = sorted((m for m in moves if m[0] == ph), key=lambda m: m[1])
        s = summary["phases"][ph]
        lines += [f"### {titles[ph]}", "",
                  f"移動 {len(rows)} ファイル。参照 {s['lines']} 行（{s['files']} ファイル、"
                  f"うちplan記録 {s['lines_plan_records']} 行）、"
                  f"編集が要る行 {s['lines_needing_edit']}（plan記録を除く）。"
                  f"HAL配下 {s['hal_lines']} 行（編集が要る行 {s['hal_lines_needing_edit']}）。", "",
                  "| 旧パス | 新パス | 参照行（plan記録を除く） | うち編集 | plan記録 | 根拠 |",
                  "| --- | --- | ---: | ---: | ---: | --- |"]
        for _p, old, new, note in rows:
            lines.append(f"| `{old}` | `{new}` | {ref_count.get(old, 0)} | "
                         f"{edit_count.get(old, 0)} | {rec_count.get(old, 0)} | {note} |")
        moved = sorted((m for m in done if m[0] == ph), key=lambda m: m[1])
        if moved:
            lines += ["", f"移動済み {len(moved)} ファイル。旧パスを指す行 {s['stale_lines']}、"
                      f"うちplan記録・`i915-old/`・このscriptを除く {s['stale_lines_checked']}"
                      "（一覧は `stale.tsv`）。", "",
                      "| 旧パス | 新パス | 根拠 |", "| --- | --- | --- |"]
            for _p, old, new, note in moved:
                lines.append(f"| `{old}` | `{new}` | {note} |")
        dirs = sorted(t for t in ref_count if t.endswith("/") and
                      any(r[0] == ph and r[4] == t for r in refs))
        if dirs:
            lines += ["", "ディレクトリ単位の参照（`-Iinclude/libc`、`libc/%.c` など）:", "",
                      "| 参照先 | 参照行（plan記録を除く） | plan記録 |", "| --- | ---: | ---: |"]
            for d in dirs:
                lines.append(f"| `{d}` | {ref_count.get(d, 0)} | {rec_count.get(d, 0)} |")
        lines.append("")
    lines += ["### 衝突・影の検査", "",
              "| Phase | 種類 | パス | 内容 |", "| --- | --- | --- | --- |"]
    for c in collisions:
        lines.append(f"| {c[0]} | {c[1]} | `{c[2]}` | {c[3]} |")
    if not collisions:
        lines.append("| — | なし | — | — |")
    lines += ["", end]
    text = md.read_text(encoding="utf-8") if md.exists() else ""
    if start in text and end in text:
        head, rest = text.split(start, 1)
        _old, tail = rest.split(end, 1)
        text = head + "\n".join(lines) + tail
    else:
        text = text.rstrip("\n") + "\n\n" + "\n".join(lines) + "\n"
    md.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
