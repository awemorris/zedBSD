#!/usr/bin/env python3
"""Checks the /dev/gpu UAPI reference (docs/reference/gpu.md, gpu-*.md)
against the public headers, mechanically.

Run from anywhere with python3 -I:

  python3 -I plan/ws009/tests/check-gpu-uapi-doc.py [--root R] [--base SHA] [--final [FILE...]]
  python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --examples
  python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --emit struct gpu_info
  python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --emit constants NAME...
  python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --emit ioctls

The last line is "check-gpu-uapi-doc: PASS ..." or "check-gpu-uapi-doc: FAIL ...".

What the documents must look like (the rules the checks read):

* Files: docs/reference/gpu.md and every docs/reference/gpu-*.md.
  Each has a line starting "Status: experimental".
* ioctl index (gpu.md): a table whose header row begins
  | Request | Nr | Direction | Argument | Bytes | Open mode | Capability | Described in |
  One row per request of the headers, each exactly once.  Request, Argument
  in backticks; Direction is `_IOW`, `_IOWR`, `_IOR` or `_IO`; Bytes is
  sizeof(argument).  Open mode is R, W, RW or none.  Capability is none or
  backticked GPU_CAP_* names joined by " and " / " or ".  Described in is a
  link to the gpu*.md file holding the request's section.
* Request section: a level-3 heading "### `GPU_NAME`", exactly one per
  request over all files.  In final mode its body has a line starting
  "Sources:" with a link into include/uapi/, a link into src/ and either a
  link into userland/tests/ or the words "no source-tree test"; and a line
  "Errors:" followed by list items "- `EXXX`: ...".  Every section has exactly
  one line "Requires: open mode X; capability Y." (X and Y as in the index;
  PENDING until written); once both are written, the section and the index
  row must agree.
* Constant tables: header row begins | Name | Value |.  Name is a
  backticked macro; Value is decimal, 0x hex or UINT32_MAX and must equal the
  header's value.  Every required macro (see REQUIRED_* below) appears in at
  least one constant table.
* Structure tables: a level-4 heading "#### `struct gpu_name`" followed (before
  the next heading) by a table whose header row begins
  | Offset | Bytes | Type | Field | Direction | Meaning |.  The rows are
  exactly the members in order, with offset, size and type as the compiler
  gives them (arrays as type[N]).  Layouts must be equal on LP64 and ILP32.
  Direction in final mode is in, out, in/out or zero.  Every required
  structure has exactly one table.
* Capset tables (gpu-capabilities.md): | Capset constant | Value | ... | with
  names VENDOR_BYTES, VENDOR_MAGIC, VENDOR_<FLAG>, NATIVE_..., taken from the
  consumer (userland/desktop/libvulkan/context.c, VULKAN_*_CAPSET_*), plus
  CAPSET_ID; and | Offset | Bytes | Payload field | ... | with the words the
  consumer reads (vulkan_load_word(capset.data + N)).
* Names: every GPU_*, HW_GPU* and struct gpu_* the documents mention exists in
  the headers (gpu-op.h enum members included); every backticked errno name
  exists in include/uapi/errno.h.
* Links: every relative link resolves; none points into plan/.  No
  development-management references (WS/Phase/Queue/Bug numbers, plan/).
* Placeholders: "<!-- spec:" comments and the word PENDING.  Counted always;
  in final mode they fail the named files (all files when none is named).
* Skeleton (--base SHA): the headings of levels 1 to 4 of every file are
  those of the file at SHA, in order; a writer adds level-5 headings only.
* Examples (--examples): every ```c block of gpu-examples.md whose first line
  is "/* example: NAME */" compiles with the target clang, -std=c11 -Wall
  -Wextra -Werror -fsyntax-only, for each GPU target whose sysroot exists.
"""

import os
import re
import subprocess
import sys

HEADERS = [
    "include/uapi/gpu.h",
    "include/uapi/gpu-allocation.h",
    "include/uapi/gpu-fence.h",
    "include/uapi/gpu-display.h",
    "include/uapi/gpu-scanout.h",
    "include/uapi/gpu-job.h",
]
OP_HEADER = "include/uapi/gpu-op.h"
SYSCTL_HEADER = "include/uapi/sysctl.h"
ERRNO_HEADER = "include/uapi/errno.h"
CAPSET_CONSUMER = "userland/desktop/libvulkan/context.c"

REQUIRED_OP_DEFINES = ["GPU_OP_PROTOCOL_VERSION", "GPU_OP_OWN_FIRST", "GPU_OP_FLAG_REPLY"]
SYSCTL_DEFINE = re.compile(r"^(GPU_TELEMETRY_\w+|HW_GPU\w*)$")
SYSCTL_STRUCTS = ["gpu_telemetry_header", "gpu_telemetry_entry"]

GPU_TARGETS = [
    ("x86_64-unknown-zedbsd", "amd64"),
    ("aarch64-unknown-zedbsd", "arm64"),
    ("riscv64-unknown-zedbsd", "riscv64"),
]

errors = []


def fail(message):
    errors.append(message)


def read(root, path):
    with open(os.path.join(root, path), encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def parse_value(text):
    text = text.strip()
    if text == "UINT32_MAX":
        return 4294967295
    match = re.fullmatch(r"(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*", text)
    if match is None:
        return None
    return int(match.group(1), 0)


class Headers:
    """The names, values, requests and structures of the GPU headers."""

    def __init__(self, root):
        self.root = root
        self.defines = {}       # name -> value (non-request)
        self.required = []      # names that must appear in a constant table
        self.ioctls = {}        # name -> (nr, direction, struct, header)
        self.structs = {}       # name -> [(type, field, array_text)]
        self.struct_header = {}
        self.required_structs = []
        self.known = set()      # every name a document may mention
        self.errnos = set()
        for path in HEADERS:
            self._parse(path, required=True)
        self._parse(OP_HEADER, required=False)
        self._parse_sysctl()
        self._parse_errno()
        self.layout = self._layout()

    def _define(self, name, raw, path, required):
        request = re.fullmatch(r"(_IOWR|_IOW|_IOR|_IO)\(\s*'G'\s*,\s*(\d+)\s*,\s*struct\s+(\w+)\s*\)", raw)
        self.known.add(name)
        if request:
            self.ioctls[name] = (int(request.group(2)), request.group(1), request.group(3), path)
            return
        value = parse_value(raw)
        if value is None:
            fail("%s: cannot read the value of %s: %s" % (path, name, raw))
            return
        self.defines[name] = value
        if required:
            self.required.append(name)

    def _parse(self, path, required):
        text = read(self.root, path)
        body = strip_comments(text)
        for match in re.finditer(r"^#define[ \t]+(GPU_\w+)[ \t]+([^\n]+?)[ \t]*$", body, flags=re.M):
            name, raw = match.group(1), match.group(2)
            need = required or name in REQUIRED_OP_DEFINES
            self._define(name, raw, path, need)
        for match in re.finditer(r"^enum\s+gpu_\w+\s*\{(.*?)\n\};", body, flags=re.S | re.M):
            for member in re.finditer(r"^\s*(GPU_\w+)\s*(=|,)", match.group(1), flags=re.M):
                self.known.add(member.group(1))
        for match in re.finditer(r"^struct\s+(gpu_\w+)\s*\{(.*?)\n\};", body, flags=re.S | re.M):
            self._struct(match.group(1), match.group(2), path, required)

    def _struct(self, name, body, path, required):
        members = []
        for line in body.split(";"):
            line = " ".join(line.split())
            if not line:
                continue
            member = re.fullmatch(r"(.+?)\s*\b(\w+)\s*(\[[^\]]+\])?", line)
            if member is None:
                fail("%s: cannot read a member of struct %s: %s" % (path, name, line))
                continue
            members.append((member.group(1).strip(), member.group(2), member.group(3)))
        self.structs[name] = members
        self.struct_header[name] = path
        self.known.add("struct " + name)
        if required:
            self.required_structs.append(name)

    def _parse_sysctl(self):
        body = strip_comments(read(self.root, SYSCTL_HEADER))
        for match in re.finditer(r"^#define[ \t]+(\w+)[ \t]+([^\n]+?)[ \t]*$", body, flags=re.M):
            if SYSCTL_DEFINE.match(match.group(1)):
                self._define(match.group(1), match.group(2), SYSCTL_HEADER, True)
        for match in re.finditer(r"^struct\s+(gpu_\w+)\s*\{(.*?)\n\};", body, flags=re.S | re.M):
            if match.group(1) in SYSCTL_STRUCTS:
                self._struct(match.group(1), match.group(2), SYSCTL_HEADER, True)

    def _parse_errno(self):
        body = strip_comments(read(self.root, ERRNO_HEADER))
        for match in re.finditer(r"^#define[ \t]+(E[A-Z0-9]+)\b", body, flags=re.M):
            self.errnos.add(match.group(1))

    def _layout_program(self):
        """A translation unit whose data is every size, offset and count, in order."""
        lines = ["#include <stddef.h>"]
        for path in HEADERS + [SYSCTL_HEADER]:
            lines.append("#include <%s>" % path[len("include/"):])
        lines.append("const unsigned int gpu_doc_layout[] = {")
        order = []
        for name, members in sorted(self.structs.items()):
            lines.append("(unsigned int)sizeof(struct %s)," % name)
            order.append(("S", name, None))
            for _, field, array in members:
                ref = "((struct %s *)0)->%s" % (name, field)
                count = "sizeof(%s) / sizeof(%s[0])" % (ref, ref) if array else "0"
                lines.append("(unsigned int)offsetof(struct %s, %s), (unsigned int)sizeof(%s), (unsigned int)(%s),"
                             % (name, field, ref, count))
                order.append(("M", name, field))
        lines.append("0 };")
        return "\n".join(lines) + "\n", order

    def _layout(self):
        """Compiles the layout unit to assembly on stdout (no files) for LP64 and ILP32."""
        program, order = self._layout_program()
        results = []
        for flags in ([], ["-m32"]):
            command = ["cc"] + flags + ["-std=c11", "-DKERN_UAPI_NATIVE", "-I", os.path.join(self.root, "include"),
                                        "-S", "-o", "-", "-x", "c", "-"]
            built = subprocess.run(command, input=program, capture_output=True, text=True, timeout=120)
            if built.returncode != 0:
                fail("layout unit does not compile (%s): %s" % (" ".join(flags) or "LP64", built.stderr.strip()[:2000]))
                return {}
            body = built.stdout.split("gpu_doc_layout:", 1)
            if len(body) != 2:
                fail("layout unit: no gpu_doc_layout in the assembly")
                return {}
            values = []
            for line in body[1].splitlines()[1:]:
                part = line.split()
                if not part:
                    continue
                if part[0] == ".long":
                    values.append(int(part[1], 0))
                elif part[0] == ".zero":
                    values.extend([0] * (int(part[1]) // 4))
                elif part[0].startswith("."):
                    break
            results.append(values)
        if results[0] != results[1]:
            fail("structure layouts differ between LP64 and ILP32")
        values = results[0]
        layout = {}
        at = 0
        for kind, name, field in order:
            if kind == "S":
                layout[name] = {"size": values[at], "members": []}
                at += 1
            else:
                layout[name]["members"].append((field, values[at], values[at + 1], values[at + 2]))
                at += 3
        return layout

    def rendered_members(self, name):
        rows = []
        for (ctype, field, array), (lfield, offset, size, count) in zip(self.structs[name], self.layout[name]["members"]):
            rendered = ctype + ("[%d]" % count if array else "")
            rows.append((offset, size, rendered, field))
        return rows


def table_rows(lines, start):
    """Returns the cell lists of the table starting at lines[start] and the index after it."""
    rows = []
    index = start
    while index < len(lines) and lines[index].lstrip().startswith("|"):
        cells = [cell.strip() for cell in lines[index].strip().strip("|").split("|")]
        rows.append(cells)
        index += 1
    return rows, index


def unquote(cell):
    cell = cell.strip()
    if len(cell) >= 2 and cell[0] == "`" and cell[-1] == "`":
        return cell[1:-1].strip()
    return cell


class Document:
    def __init__(self, root, path):
        self.path = path
        self.text = read(root, path)
        self.lines = self.text.split("\n")


def collect_tables(doc):
    """Yields (kind, heading, rows, line_number) for each table."""
    heading = None
    index = 0
    in_spec = False
    while index < len(doc.lines):
        line = doc.lines[index]
        if "<!-- spec:" in line:
            in_spec = "-->" not in line
        elif in_spec:
            if "-->" in line:
                in_spec = False
            index += 1
            continue
        if line.startswith("#"):
            heading = line
            index += 1
            continue
        if line.lstrip().startswith("|"):
            rows, after = table_rows(doc.lines, index)
            if len(rows) >= 2:
                head = [unquote(cell) for cell in rows[0]]
                body = [row for row in rows[2:]]
                yield head, heading, body, index + 1
            index = after
            continue
        index += 1


def check_ioctl_index(docs, headers):
    seen = {}
    for doc in docs:
        for head, _, body, number in collect_tables(doc):
            if head[:8] != ["Request", "Nr", "Direction", "Argument", "Bytes", "Open mode", "Capability", "Described in"]:
                continue
            for row in body:
                where = "%s:%d" % (doc.path, number)
                if len(row) < 8:
                    fail("%s: ioctl row has %d cells" % (where, len(row)))
                    continue
                name = unquote(row[0])
                if name in seen:
                    fail("%s: %s listed twice in the ioctl index" % (where, name))
                seen[name] = row
                if name not in headers.ioctls:
                    fail("%s: %s is not a request in the headers" % (where, name))
                    continue
                nr, direction, struct, _ = headers.ioctls[name]
                if row[1] != str(nr):
                    fail("%s: %s Nr %s, header %d" % (where, name, row[1], nr))
                if unquote(row[2]) != direction:
                    fail("%s: %s Direction %s, header %s" % (where, name, row[2], direction))
                if unquote(row[3]) != "struct " + struct:
                    fail("%s: %s Argument %s, header struct %s" % (where, name, row[3], struct))
                size = headers.layout.get(struct, {}).get("size")
                if row[4] != str(size):
                    fail("%s: %s Bytes %s, sizeof %s" % (where, name, row[4], size))
                if row[5] not in ("R", "W", "RW", "none", "PENDING"):
                    fail("%s: %s Open mode %r is not R, W, RW, none" % (where, name, row[5]))
                if row[6] not in ("none", "PENDING"):
                    for cap in re.split(r"\s+(?:and|or)\s+", row[6]):
                        cap = unquote(cap)
                        if not cap.startswith("GPU_CAP_") or cap not in headers.defines:
                            fail("%s: %s Capability %r is not a GPU_CAP_* name" % (where, name, cap))
                link = re.fullmatch(r"\[[^\]]*\]\(([^)#]+)(#[^)]*)?\)", row[7])
                if link is None:
                    fail("%s: %s Described in is not a link: %s" % (where, name, row[7]))
                    continue
                target = os.path.normpath(os.path.join(os.path.dirname(doc.path), link.group(1)))
                holder = [d for d in docs if os.path.normpath(d.path) == target]
                if not holder or ("### `%s`" % name) not in holder[0].lines:
                    fail("%s: %s Described in %s, which has no '### `%s`' heading" % (where, name, link.group(1), name))
    for name in headers.ioctls:
        if name not in seen:
            fail("ioctl index: %s is missing" % name)
    return seen


def ioctl_sections(doc):
    """Yields (name, body_lines, line_number) for each '### `GPU_X`' section."""
    for index, line in enumerate(doc.lines):
        match = re.fullmatch(r"### `(GPU_\w+)`", line)
        if not match:
            continue
        end = index + 1
        while end < len(doc.lines) and not re.match(r"#{1,3} ", doc.lines[end]):
            end += 1
        yield match.group(1), doc.lines[index + 1:end], index + 1


REQUIRES = re.compile(r"Requires: open mode (R|W|RW|none); capability (.+)\.")


def capability_set(text):
    """Normalises a Capability cell or clause: the joiner and the names."""
    text = text.strip()
    if text in ("none", "PENDING"):
        return (text, ())
    joiner = "or" if " or " in text else "and"
    return (joiner, tuple(sorted(unquote(part) for part in re.split(r"\s+(?:and|or)\s+", text))))


def check_sections(docs, headers, final_paths, index_rows):
    count = {}
    stated = {}
    for doc in docs:
        for name, body, number in ioctl_sections(doc):
            where = "%s:%d" % (doc.path, number)
            count[name] = count.get(name, 0) + 1
            if name not in headers.ioctls:
                fail("%s: section %s is not a request" % (where, name))
            requires = [line for line in body if line.startswith("Requires:")]
            if len(requires) != 1:
                fail("%s: %s has %d 'Requires:' lines" % (where, name, len(requires)))
            elif requires[0] != "Requires: PENDING":
                match = REQUIRES.fullmatch(requires[0])
                if match is None:
                    fail("%s: %s 'Requires:' is not 'Requires: open mode X; capability Y.': %s" % (where, name, requires[0]))
                else:
                    stated[name] = (match.group(1), capability_set(match.group(2)), where)
            if doc.path not in final_paths:
                continue
            sources = [line for line in body if line.startswith("Sources:")]
            if not sources:
                fail("%s: %s has no 'Sources:' line" % (where, name))
            else:
                text = " ".join(sources)
                if "include/uapi/" not in text:
                    fail("%s: %s Sources: no header link" % (where, name))
                if "src/" not in text:
                    fail("%s: %s Sources: no implementation link" % (where, name))
                if "userland/tests/" not in text and "no source-tree test" not in text:
                    fail("%s: %s Sources: no test link and no 'no source-tree test'" % (where, name))
            if "Errors:" not in body:
                fail("%s: %s has no 'Errors:' line" % (where, name))
            else:
                at = body.index("Errors:")
                items = [line for line in body[at + 1:] if line.startswith("- `E")]
                if not items:
                    fail("%s: %s 'Errors:' has no '- `EXXX`:' items" % (where, name))
    for name in headers.ioctls:
        if count.get(name, 0) != 1:
            fail("request section '### `%s`' appears %d times" % (name, count.get(name, 0)))
    for name, (mode, caps, where) in stated.items():
        row = index_rows.get(name)
        if row is None or row[5] == "PENDING" or row[6] == "PENDING":
            continue
        if row[5] != mode or capability_set(row[6]) != caps:
            fail("%s: %s Requires: open mode %s, capability %s; index says %s, %s"
                 % (where, name, mode, caps, row[5], row[6]))
    return len(count)


def check_constants(docs, headers):
    present = set()
    rows_checked = 0
    for doc in docs:
        for head, _, body, number in collect_tables(doc):
            if head[:2] != ["Name", "Value"]:
                continue
            for row in body:
                where = "%s:%d" % (doc.path, number)
                name = unquote(row[0])
                if name not in headers.defines:
                    fail("%s: constant %s is not a value macro of the headers" % (where, name))
                    continue
                value = parse_value(unquote(row[1]))
                if value != headers.defines[name]:
                    fail("%s: %s Value %s, header %d" % (where, name, row[1], headers.defines[name]))
                present.add(name)
                rows_checked += 1
    for name in headers.required:
        if name not in present:
            fail("constant tables: %s is missing" % name)
    return rows_checked


def check_structs(docs, headers, final_paths):
    count = {}
    for doc in docs:
        for head, heading, body, number in collect_tables(doc):
            if head[:4] != ["Offset", "Bytes", "Type", "Field"]:
                continue
            where = "%s:%d" % (doc.path, number)
            match = re.fullmatch(r"#### `struct (gpu_\w+)`", heading or "")
            if match is None:
                fail("%s: structure table is not under a '#### `struct gpu_name`' heading" % where)
                continue
            name = match.group(1)
            count[name] = count.get(name, 0) + 1
            if name not in headers.structs:
                fail("%s: struct %s is not in the headers" % (where, name))
                continue
            expect = headers.rendered_members(name)
            if len(body) != len(expect):
                fail("%s: struct %s has %d rows, header %d members" % (where, name, len(body), len(expect)))
            for row, (offset, size, ctype, field) in zip(body, expect):
                got = (row[0], row[1], unquote(row[2]), unquote(row[3]))
                want = (str(offset), str(size), ctype, field)
                if got != want:
                    fail("%s: struct %s row %s, header %s" % (where, name, got, want))
                if doc.path in final_paths and len(row) >= 5 and row[4] not in ("in", "out", "in/out", "zero"):
                    fail("%s: struct %s.%s Direction %r is not in, out, in/out, zero" % (where, name, field, row[4]))
    for name in headers.required_structs:
        if count.get(name, 0) != 1:
            fail("structure table for struct %s appears %d times" % (name, count.get(name, 0)))
    return len(count)


def capset_expectations(root):
    text = read(root, CAPSET_CONSUMER)
    constants = {}
    for match in re.finditer(r"^#define\s+VULKAN_(VENDOR|NATIVE)_CAPSET_(\w+)\s+(\S+)", text, flags=re.M):
        constants["%s_%s" % (match.group(1), match.group(2))] = parse_value(match.group(3))
    match = re.search(r"capset\.capset_id\s*=\s*(\w+)\s*;", text)
    if match:
        constants["CAPSET_ID"] = parse_value(match.group(1))
    words = {}
    for match in re.finditer(r"(\w+)\s*=\s*vulkan_load_word\(capset\.data(?:\s*\+\s*(\d+))?\)", text):
        words[match.group(1)] = int(match.group(2) or 0)
    return constants, words


def check_capset(docs, root):
    constants, words = capset_expectations(root)
    if not constants or not words:
        fail("%s: capset constants or words not found" % CAPSET_CONSUMER)
    seen_constants = {}
    seen_words = {}
    for doc in docs:
        for head, _, body, number in collect_tables(doc):
            where = "%s:%d" % (doc.path, number)
            if head[:2] == ["Capset constant", "Value"]:
                for row in body:
                    seen_constants[unquote(row[0])] = (parse_value(unquote(row[1])), where)
            elif head[:3] == ["Offset", "Bytes", "Payload field"]:
                for row in body:
                    seen_words[unquote(row[2])] = (row[0], row[1], where)
    for name, value in constants.items():
        if name not in seen_constants:
            fail("capset constant %s (%s) is missing" % (name, value))
        elif seen_constants[name][0] != value:
            fail("%s: capset constant %s %s, consumer %s" % (seen_constants[name][1], name, seen_constants[name][0], value))
    for name in seen_constants:
        if name not in constants:
            fail("%s: capset constant %s is not in the consumer" % (seen_constants[name][1], name))
    for name, offset in words.items():
        if name not in seen_words:
            fail("capset payload field %s (offset %d) is missing" % (name, offset))
        elif seen_words[name][0] != str(offset) or seen_words[name][1] != "4":
            fail("%s: payload field %s at %s/%s, consumer %d/4" % (seen_words[name][2], name, seen_words[name][0], seen_words[name][1], offset))
    return len(seen_constants) + len(seen_words)


DEV_INFO = re.compile(r"\b(?:WS|ws)\d{3}\b|\bws\d{3}-p\d+|\bBUG-\d+|\bq\d{3,4}\b|\.internal/")


def check_text(docs, headers, root, final_paths):
    placeholders = 0
    for doc in docs:
        if not re.search(r"^Status: experimental", doc.text, flags=re.M):
            fail("%s: no 'Status: experimental' line" % doc.path)
        for number, line in enumerate(doc.lines, 1):
            where = "%s:%d" % (doc.path, number)
            for token in re.findall(r"(?<![A-Za-z0-9_])((?:GPU|HW_GPU)[A-Z0-9_]*)", line):
                if token.endswith("_"):
                    continue
                if token not in headers.known and token != "GPU":
                    fail("%s: %s is not a name of the headers" % (where, token))
            for token in re.findall(r"\bstruct\s+(gpu_\w+)", line):
                if "struct " + token not in headers.known:
                    fail("%s: struct %s is not in the headers" % (where, token))
            for token in re.findall(r"`(E[A-Z0-9]{2,})`", line):
                if token not in headers.errnos:
                    fail("%s: %s is not an errno name" % (where, token))
            if DEV_INFO.search(line):
                fail("%s: development-management reference: %s" % (where, line.strip()[:120]))
            for target in re.findall(r"\]\(([^)\s]+)\)", line):
                if re.match(r"[a-z]+:", target) or target.startswith("#"):
                    continue
                path = target.split("#")[0]
                if "plan/" in path:
                    fail("%s: link into plan/: %s" % (where, target))
                resolved = os.path.normpath(os.path.join(root, os.path.dirname(doc.path), path))
                if not os.path.exists(resolved):
                    fail("%s: missing link target: %s" % (where, target))
        count = doc.text.count("<!-- spec:") + len(re.findall(r"\bPENDING\b", doc.text))
        placeholders += count
        if doc.path in final_paths and count:
            fail("%s: %d placeholders remain" % (doc.path, count))
    return placeholders


def headings(text):
    return [line for line in text.split("\n") if re.match(r"#{1,6} ", line)]


def check_base(root, docs, base):
    """The skeleton's headings (levels 1 to 4) stay, in order; additions are level 5 or deeper only."""
    for doc in docs:
        shown = subprocess.run(["git", "-C", root, "show", "%s:%s" % (base, doc.path)],
                               capture_output=True, text=True, timeout=60)
        if shown.returncode != 0:
            fail("%s: not in the skeleton at %s" % (doc.path, base))
            continue
        old = headings(shown.stdout)
        new = [line for line in headings(doc.text) if re.match(r"#{1,4} ", line)]
        if old != new:
            missing = [line for line in old if line not in new]
            added = [line for line in new if line not in old]
            fail("%s: headings differ from the skeleton at %s (missing %s, added %s)" % (doc.path, base, missing[:3], added[:3]))


def check_examples(root):
    path = "docs/reference/gpu-examples.md"
    text = read(root, path)
    blocks = re.findall(r"```c\n(/\* example: ([\w-]+) \*/\n.*?)```", text, flags=re.S)
    if not blocks:
        fail("%s: no ```c block starting with '/* example: NAME */'" % path)
        return 0
    build = os.environ.get("GPU_DOC_BUILD")
    if not build:
        build = os.path.join(root, "build")
        if not os.path.exists(os.path.join(build, "llvm/bin/clang")):
            build = "/home/awe/zedBSD-claude1/build"
    clang = os.path.join(build, "llvm/bin/clang")
    compiled = 0
    for source, name in blocks:
        for target, arch in GPU_TARGETS:
            sysroot = os.path.join(build, arch, "sysroot")
            if not os.path.isdir(sysroot):
                print("examples: %s: no sysroot %s, not compiled" % (arch, sysroot))
                continue
            command = [clang, "--target=" + target, "--sysroot=" + sysroot, "-I", os.path.join(root, "include"),
                       "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-x", "c", "-"]
            result = subprocess.run(command, input=source, capture_output=True, text=True, timeout=120)
            if result.returncode != 0:
                fail("example %s (%s): %s" % (name, target, result.stderr.strip()[:2000]))
            else:
                compiled += 1
    return compiled


def emit(headers, args):
    kind = args[0]
    if kind == "struct":
        for name in args[1:]:
            print("#### `struct %s`" % name)
            print()
            print("| Offset | Bytes | Type | Field | Direction | Meaning |")
            print("| ---: | ---: | --- | --- | --- | --- |")
            for offset, size, ctype, field in headers.rendered_members(name):
                print("| %d | %d | `%s` | `%s` | PENDING | PENDING |" % (offset, size, ctype, field))
            print()
    elif kind == "constants":
        print("| Name | Value | Meaning |")
        print("| --- | ---: | --- |")
        for name in args[1:]:
            print("| `%s` | %s | PENDING |" % (name, headers.defines[name]))
    elif kind == "ioctls":
        print("| Request | Nr | Direction | Argument | Bytes | Open mode | Capability | Described in |")
        print("| --- | ---: | --- | --- | ---: | --- | --- | --- |")
        for name, (nr, direction, struct, _) in sorted(headers.ioctls.items(), key=lambda item: item[1][0]):
            print("| `%s` | %d | `%s` | `struct %s` | %d | PENDING | PENDING | PENDING |"
                  % (name, nr, direction, struct, headers.layout[struct]["size"]))
    elif kind == "names":
        for name in sorted(headers.required):
            print(name, headers.defines[name])
        for name in sorted(headers.required_structs):
            print("struct", name, headers.struct_header[name])
    else:
        sys.exit("unknown --emit kind %s" % kind)


def main(argv):
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
    final = None
    examples = False
    emit_args = None
    base = None
    index = 0
    while index < len(argv):
        arg = argv[index]
        if arg == "--root":
            root = argv[index + 1]
            index += 2
        elif arg == "--final":
            final = argv[index + 1:]
            break
        elif arg == "--base":
            base = argv[index + 1]
            index += 2
        elif arg == "--examples":
            examples = True
            index += 1
        elif arg == "--emit":
            emit_args = argv[index + 1:]
            break
        else:
            sys.exit("usage: check-gpu-uapi-doc.py [--root R] [--base SHA] [--examples] [--final [FILE...]] | --emit KIND ARGS")
    headers = Headers(root)
    if emit_args is not None:
        if errors:
            sys.exit("\n".join(errors))
        emit(headers, emit_args)
        return 0
    names = sorted(name for name in os.listdir(os.path.join(root, "docs/reference"))
                   if name == "gpu.md" or (name.startswith("gpu-") and name.endswith(".md")))
    docs = [Document(root, "docs/reference/" + name) for name in names]
    if final is None:
        final_paths = set()
    elif final:
        final_paths = set(os.path.normpath(path) for path in final)
        for path in final_paths:
            if path not in [doc.path for doc in docs]:
                fail("--final %s is not a GPU reference file" % path)
    else:
        final_paths = set(doc.path for doc in docs)
    index_rows = check_ioctl_index(docs, headers)
    ioctls = len(index_rows)
    sections = check_sections(docs, headers, final_paths, index_rows)
    if base:
        check_base(root, docs, base)
    constants = check_constants(docs, headers)
    structs = check_structs(docs, headers, final_paths)
    capset = check_capset(docs, root)
    placeholders = check_text(docs, headers, root, final_paths)
    compiled = check_examples(root) if examples else 0
    summary = ("%d files, %d index rows, %d request sections, %d constant rows, %d structures, %d capset rows, "
               "%d placeholders, final %d files%s"
               % (len(docs), ioctls, sections, constants, structs, capset, placeholders, len(final_paths),
                  ", %d example compiles" % compiled if examples else ""))
    for message in errors:
        print(message)
    if errors:
        print("check-gpu-uapi-doc: FAIL (%d errors; %s)" % (len(errors), summary))
        return 1
    print("check-gpu-uapi-doc: PASS (%s)" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
