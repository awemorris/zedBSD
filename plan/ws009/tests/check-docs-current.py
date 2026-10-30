#!/usr/bin/env python3
"""Checks the product documents against the current source tree.

Run with python3 -I from anywhere:

  python3 -I plan/ws009/tests/check-docs-current.py [REPOSITORY_ROOT]

REPOSITORY_ROOT defaults to ".".  Every value that a document must agree with
is read from the source (usage strings, headers, Makefile, version files,
the menu); the script holds no copy of old document text.  All failures are
collected before the script ends.  Each one is a line

  FAIL <file>: <reason>

and the last line is "check-docs-current: PASS (<what was counted>)" or
"check-docs-current: FAIL (<n> problems)".  The exit status is 0 or 1.

Checked:

* WLAN (docs/reference/managed-wlan.md): every `net wifi ...` form of the
  usage text in userland/base/net/main.c (the words up to the first option)
  appears; every `net wifi <verb>` the document writes is a verb of the usage;
  `set-key` does not appear; every managed-WLAN state of
  userland/base/networkd/managed-wlan-state.h appears in backquotes (spelled
  as networkd/main.c spells it); the file size limit, when the document
  gives one, equals WIFI_CONF_FILE_MAX of userland/base/net/wifi-conf.h.
* evdev (docs/reference/evdev.md): the two retired "not implemented" /
  "will reserve" sentences are absent; the legacy console event ioctls are
  said to have been removed; the BeUI consumer names the Noct version of
  userland/base/noct/version.mk.
* build (docs/howto/build-from-source.md): every variant of
  ZEDBSD_VARIANTS_amd64_pcat in the Makefile has a table row that starts with
  its backquoted name; every amd64 variant label of tools/menuconfig.py is in
  backquotes; the Noct tag, archive size and SHA-256, and the version as
  v<version>; the LLVM version and cache tag; the sysroot directories and
  the Target Noct row.
* Status: every .md under docs/ except docs/agent/ and the release texts
  docs/release/zedbsd-*.md has a line starting "Status: " in its first 12
  lines; none of them contains ".internal/".
* Index: every one of those documents is reachable from docs/README.md
  through relative Markdown links.
"""

import ast
import os
import re
import sys

problems = []
counts = {}


def fail(path, reason):
    problems.append("FAIL %s: %s" % (path, reason))
    print(problems[-1])


def count(name, n=1):
    counts[name] = counts.get(name, 0) + n


def read(root, rel):
    """Returns the file's text, or None after reporting that it is missing."""
    try:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        fail(rel, "cannot read: %s" % e.strerror)
        return None


def flat(text):
    """Joins wrapped lines so that a phrase can be found across a break."""
    return re.sub(r"\s+", " ", text)


def need(rel, text, needle, what):
    count("items")
    if needle not in text:
        fail(rel, "missing %s: %s" % (what, needle))


def make_vars(text):
    """Reads `override NAME := value` and `NAME := value` lines."""
    found = {}
    for m in re.finditer(r"^(?:override\s+)?([A-Z0-9_]+)\s*:?=\s*(.*?)\s*$",
                         text, re.M):
        found[m.group(1)] = m.group(2)
    return found


# ---------------------------------------------------------------- WLAN

def check_wlan(root):
    doc_rel = "docs/reference/managed-wlan.md"
    doc = read(root, doc_rel)
    net = read(root, "userland/base/net/main.c")
    state_h = read(root, "userland/base/networkd/managed-wlan-state.h")
    networkd = read(root, "userland/base/networkd/main.c")
    conf_h = read(root, "userland/base/net/wifi-conf.h")
    if doc is None:
        return
    text = flat(doc)

    if net is not None:
        forms = []
        for m in re.finditer(r'^\s*"  net wifi ([^"]*?)(?:\s{2,}[^"]*)?\\n"',
                             net, re.M):
            words = []
            for w in m.group(1).split():
                if w.startswith("["):
                    break
                words.append(w)
            if words:
                forms.append("net wifi " + " ".join(words))
        forms = sorted(set(forms))
        if not forms:
            fail("userland/base/net/main.c", "no `net wifi` usage lines found")
        verbs = set()
        for form in forms:
            count("wlan-forms")
            verbs.add(form.split()[2])
            need(doc_rel, text, form, "command form of the usage")
        for m in re.finditer(r"\bnet wifi ([a-z][a-z-]*)", text):
            count("items")
            if m.group(1) not in verbs and m.group(1) not in ("startup",):
                fail(doc_rel, "`net wifi %s` is not a verb of the usage"
                     % m.group(1))
    count("items")
    if "set-key" in doc:
        fail(doc_rel, "mentions the removed command set-key")

    if state_h is not None:
        names = {}
        m = re.search(r"enum\s+networkd_managed_wlan_state\s*\{(.*?)\}",
                      state_h, re.S)
        if not m:
            fail("userland/base/networkd/managed-wlan-state.h",
                 "state enum not found")
        else:
            for e in re.findall(r"\b(NETWORKD_WLAN_[A-Z_]+)\b", m.group(1)):
                spelled = e[len("NETWORKD_WLAN_"):].lower().replace("_", "-")
                if networkd is not None:
                    s = re.search(r"state == %s\)\s*return \"([a-z-]+)\"" % e,
                                  networkd)
                    if s:
                        spelled = s.group(1)
                names[e] = spelled
            for e, spelled in sorted(names.items()):
                count("wlan-states")
                need(doc_rel, doc, "`%s`" % spelled,
                     "state %s in backquotes" % e)

    if conf_h is not None:
        m = re.search(r"#define\s+WIFI_CONF_FILE_MAX\s+(\d+)", conf_h)
        if not m:
            fail("userland/base/net/wifi-conf.h", "WIFI_CONF_FILE_MAX not found")
        else:
            limit = int(m.group(1))
            d = re.search(r"limited to ([\d,]+) bytes", text)
            if d:
                count("items")
                if int(d.group(1).replace(",", "")) != limit:
                    fail(doc_rel, "file limit %s differs from "
                         "WIFI_CONF_FILE_MAX %d" % (d.group(1), limit))


# --------------------------------------------------------------- evdev

def check_evdev(root, noct):
    rel = "docs/reference/evdev.md"
    doc = read(root, rel)
    if doc is None:
        return
    text = flat(doc)
    for stale in ("Live USB HID binding is not implemented",
                  "p008 will reserve"):
        count("items")
        if stale in text:
            fail(rel, "contains retired sentence: %s" % stale)
    count("items")
    if not re.search(r"console[^.]*ioctls[^.]*have been removed", text):
        fail(rel, "does not say that the legacy console event ioctls "
             "have been removed")
    version = noct.get("ZEDBSD_NOCT_VERSION")
    if version:
        need(rel, text, "Noct %s BeUI" % version,
             "BeUI consumer version (ZEDBSD_NOCT_VERSION)")


# --------------------------------------------------------------- build

def check_build(root, noct):
    rel = "docs/howto/build-from-source.md"
    doc = read(root, rel)
    makefile = read(root, "Makefile")
    menu = read(root, "tools/menuconfig.py")
    llvm_text = read(root, "toolchain/llvm/version.mk")
    if doc is None:
        return
    text = flat(doc)

    if makefile is not None:
        m = re.search(r"^ZEDBSD_VARIANTS_amd64_pcat\s*:?=\s*(.*)$", makefile, re.M)
        if not m:
            fail("Makefile", "ZEDBSD_VARIANTS_amd64_pcat not found")
        else:
            for v in m.group(1).split():
                count("variants")
                if not re.search(r"^\| `%s`" % re.escape(v), doc, re.M):
                    fail(rel, "no table row starting with `%s` for the "
                         "Makefile variant" % v)

    if menu is not None:
        labels = []
        try:
            for node in ast.parse(menu).body:
                if (isinstance(node, ast.Assign)
                        and any(getattr(t, "id", "") == "BOARD_VARIANTS"
                                for t in node.targets)):
                    table = ast.literal_eval(node.value)
                    labels = [lab for _v, lab in table[("amd64", "pcat")]]
        except (SyntaxError, ValueError, KeyError) as e:
            fail("tools/menuconfig.py", "cannot read BOARD_VARIANTS: %s" % e)
        if not labels:
            fail("tools/menuconfig.py", "no amd64 pcat variant labels")
        for lab in labels:
            count("labels")
            need(rel, doc, "`%s`" % lab, "menu label in backquotes")

    if noct:
        for key, shown in (("ZEDBSD_NOCT_TAG", "Noct tag"),
                           ("ZEDBSD_NOCT_ARCHIVE_SHA256", "Noct archive SHA-256")):
            if key in noct:
                need(rel, doc, noct[key], "%s (%s)" % (shown, key))
        if "ZEDBSD_NOCT_ARCHIVE_SIZE" in noct:
            size = int(noct["ZEDBSD_NOCT_ARCHIVE_SIZE"])
            need(rel, text, "{:,} bytes".format(size),
                 "Noct archive size (ZEDBSD_NOCT_ARCHIVE_SIZE)")
        if "ZEDBSD_NOCT_VERSION" in noct:
            need(rel, text, "`v%s`" % noct["ZEDBSD_NOCT_VERSION"],
                 "Noct release (ZEDBSD_NOCT_VERSION)")
    if llvm_text is not None:
        llvm = make_vars(llvm_text)
        for key in ("ZEDBSD_LLVM_VERSION", "ZEDBSD_LLVM_CACHE_TAG"):
            if key in llvm:
                need(rel, doc, llvm[key], key)
            else:
                fail("toolchain/llvm/version.mk", "%s not found" % key)
    for fixed in ("`build/amd64/sysroot/`", "`build/i386/sysroot/`",
                  "Target Noct"):
        need(rel, doc, fixed, "sysroot/target row")


# ------------------------------------------------- status and the index

def product_documents(root):
    docs = []
    base = os.path.join(root, "docs")
    for dirpath, dirnames, files in os.walk(base):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if rel_dir == "docs/agent" or rel_dir.startswith("docs/agent/"):
            continue
        for name in sorted(files):
            if not name.endswith(".md"):
                continue
            rel = rel_dir + "/" + name
            if rel_dir == "docs/release" and name.startswith("zedbsd-"):
                continue
            docs.append(rel)
    return docs


def check_status(root, docs):
    for rel in docs:
        text = read(root, rel)
        if text is None:
            continue
        count("status-documents")
        head = text.split("\n")[:12]
        if not any(line.startswith("Status: ") for line in head):
            fail(rel, "no line starting \"Status: \" in the first 12 lines")
        if ".internal/" in text:
            fail(rel, "contains \".internal/\"")


LINK = re.compile(r"\]\(<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\)")


def links_of(text):
    out = []
    fenced = False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            out.extend(LINK.findall(line))
    return out


def check_index(root, docs):
    start = "docs/README.md"
    seen = {start}
    todo = [start]
    while todo:
        rel = todo.pop()
        try:
            with open(os.path.join(root, rel), encoding="utf-8") as f:
                text = f.read()
        except OSError:
            continue
        for target in links_of(text):
            if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target[0] in "#/":
                continue
            target = target.split("#", 1)[0]
            if not target:
                continue
            path = os.path.normpath(os.path.join(os.path.dirname(rel), target))
            if os.path.isdir(os.path.join(root, path)):
                path = os.path.join(path, "README.md")
            path = path.replace(os.sep, "/")
            if (path.endswith(".md") and path not in seen
                    and os.path.isfile(os.path.join(root, path))):
                seen.add(path)
                todo.append(path)
    for rel in docs:
        count("indexed-documents")
        if rel not in seen:
            fail(rel, "not reachable from docs/README.md by relative links")


def main(argv):
    root = argv[1] if len(argv) > 1 else "."
    if len(argv) > 2:
        print("usage: check-docs-current.py [REPOSITORY_ROOT]")
        return 2
    if not os.path.isdir(os.path.join(root, "docs")):
        print("check-docs-current: FAIL (no docs/ under %s)" % root)
        return 1
    noct_text = read(root, "userland/base/noct/version.mk")
    noct = make_vars(noct_text) if noct_text is not None else {}

    check_wlan(root)
    check_evdev(root, noct)
    check_build(root, noct)
    docs = product_documents(root)
    check_status(root, docs)
    check_index(root, docs)

    if problems:
        print("check-docs-current: FAIL (%d problems)" % len(problems))
        return 1
    print("check-docs-current: PASS (%s)" % ", ".join(
        "%d %s" % (counts[k], k) for k in sorted(counts)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
