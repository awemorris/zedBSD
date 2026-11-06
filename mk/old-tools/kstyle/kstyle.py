#!/usr/bin/env python3
"""Mechanical zedBSD C style pass for src/kern (pass 1: no comments, no goto).

Transformations (all whitespace/layout or declaration-placement only):
  envelope   modeline + canonical copyright block
  defs       function definition heads: type / name( / one param per line
  protos     one-line forward declarations for every non-inline static function,
             consolidated before the first function definition
  reorder    public definitions before static definitions (only when the
             function area contains nothing but functions and no #if lines)
  case       'case X: stmt;' -> label on its own line
  blank      no blank line directly after an opening brace
  hoist      block-local declarations moved to the leading declaration group;
             the initializer stays in place as an assignment

Every skipped/unsafe site is reported so it can be handled by hand.
"""
import re
import sys

MODELINE = '/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */'
COPYRIGHT = [
    '/*',
    ' * zedBSD',
    ' * Copyright (C) 2026 Awe Morris',
    ' *',
    ' * SPDX-License-Identifier: Zlib',
    ' */',
]

BASE_TYPES = {
    'void', 'char', 'short', 'int', 'long', 'float', 'double', 'signed',
    'unsigned', 'bool', '_Bool', 'va_list', 'FILE',
}
QUALIFIERS = {'static', 'const', 'volatile', 'register', 'extern', '__typeof__'}
IDENT = r'[A-Za-z_][A-Za-z0-9_]*'


def is_type_word(word):
    if word in BASE_TYPES or word in ('struct', 'union', 'enum'):
        return True
    return bool(re.match(IDENT + r'$', word)) and (
        word.endswith('_t') or word.endswith('_fn') or word.endswith('_cb'))


# ---------------------------------------------------------------------------
# Lexical map: for every character, whether it is code (not comment/string).
# ---------------------------------------------------------------------------

def code_mask(text):
    mask = bytearray(len(text))
    i = 0
    n = len(text)
    state = 'code'
    while i < n:
        c = text[i]
        if state == 'code':
            if text.startswith('/*', i):
                state = 'block'
                i += 2
                continue
            if text.startswith('//', i):
                state = 'line'
                i += 2
                continue
            if c == '"':
                state = 'str'
                mask[i] = 1
                i += 1
                continue
            if c == "'":
                state = 'chr'
                mask[i] = 1
                i += 1
                continue
            mask[i] = 1
            i += 1
        elif state == 'block':
            if text.startswith('*/', i):
                state = 'code'
                i += 2
            else:
                i += 1
        elif state == 'line':
            if c == '\n':
                state = 'code'
                mask[i] = 1
            i += 1
        elif state in ('str', 'chr'):
            if c == '\\':
                i += 2
                continue
            if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
                state = 'code'
                mask[i] = 1
            i += 1
    return mask


class Source:
    def __init__(self, text):
        self.lines = text.split('\n')
        self.rebuild()

    def rebuild(self):
        text = '\n'.join(self.lines)
        mask = code_mask(text)
        self.depth_at = []      # brace depth at start of each line
        self.code_line = []     # code-only content of each line (comments blanked)
        self.pp_depth = []      # #if nesting at start of line
        depth = 0
        pp = 0
        pos = 0
        for line in self.lines:
            self.depth_at.append(depth)
            self.pp_depth.append(pp)
            code = []
            for j, ch in enumerate(line):
                if mask[pos + j]:
                    code.append(ch)
                    if ch == '{':
                        depth += 1
                    elif ch == '}':
                        depth -= 1
                else:
                    code.append(' ')
            codeline = ''.join(code)
            self.code_line.append(codeline)
            stripped = line.strip()
            if stripped.startswith('#'):
                d = stripped[1:].strip()
                if d.startswith('if'):
                    pp += 1
                elif d.startswith('endif'):
                    pp -= 1
            pos += len(line) + 1

    def text(self):
        return '\n'.join(self.lines)


# ---------------------------------------------------------------------------
# Function definitions
# ---------------------------------------------------------------------------

class Function:
    def __init__(self, head_start, head_end, body_end, prefix, name, params,
                 suffix, is_static, is_inline):
        self.head_start = head_start  # first line of the head
        self.head_end = head_end      # line index of '{'
        self.body_end = body_end      # line index of closing '}'
        self.prefix = prefix
        self.name = name
        self.params = params          # list of parameter strings
        self.suffix = suffix
        self.is_static = is_static
        self.is_inline = is_inline
        self.comment_start = head_start


def split_params(inner):
    parts = []
    depth = 0
    cur = []
    for ch in inner:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    tail = ''.join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


def is_head_type_line(line):
    s = line.strip()
    if not s or s.startswith('#') or s.startswith('/*') or s.startswith('*') \
            or s.endswith('*/') or s.endswith(';') or s.endswith('}') or '(' in s:
        return False
    first = s.split()[0].rstrip('*')
    return first in QUALIFIERS or is_type_word(first) or first == 'inline' \
        or s.endswith('*') or first.startswith('__attribute__')


def find_functions(src, report):
    funcs = []
    lines = src.lines
    i = 0
    while i < len(lines):
        if lines[i] == '{' and src.depth_at[i] == 0:
            # Head: consecutive non-blank lines above.
            j = i - 1
            while j >= 0 and lines[j].strip() and not lines[j].strip().endswith('*/') \
                    and not lines[j].lstrip().startswith('#') \
                    and not lines[j].rstrip().endswith('}') \
                    and not lines[j].rstrip().endswith(';') \
                    and not lines[j].rstrip().endswith('{'):
                j -= 1
            head_start = j + 1
            head = ' '.join(l.strip() for l in lines[head_start:i])
            head = re.sub(r'\s+', ' ', head).strip()
            # Find body end.
            k = i + 1
            while k < len(lines) and not (lines[k] == '}' and src.depth_at[k] == 1):
                k += 1
            if k >= len(lines):
                report.append('unterminated function body at line %d' % (i + 1))
                i += 1
                continue
            parsed = parse_head(head)
            if parsed is None:
                report.append('unparsed definition head at line %d: %s' % (head_start + 1, head[:60]))
                i = k + 1
                continue
            prefix, name, inner, suffix = parsed
            # The regex is greedy on the name: ensure the '(' after the name
            # matches the ')' before suffix.
            depth = 0
            ok = True
            for ch in inner:
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
                    if depth < 0:
                        ok = False
                        break
            if not ok or depth != 0:
                report.append('unbalanced parameter list at line %d' % (head_start + 1))
                i = k + 1
                continue
            words = prefix.replace('*', ' * ').split()
            is_static = 'static' in words
            is_inline = 'inline' in words or '__inline' in words or '__inline__' in words
            f = Function(head_start, i, k, prefix, name, split_params(inner),
                         suffix, is_static, is_inline)
            # Preceding comment block belongs to the function.
            c = head_start - 1
            if c >= 0 and lines[c].strip().endswith('*/'):
                while c >= 0 and not lines[c].lstrip().startswith('/*'):
                    c -= 1
                if c >= 0:
                    f.comment_start = c
            funcs.append(f)
            i = k + 1
            continue
        i += 1
    return funcs


def parse_head(head):
    """Split 'prefix name(params) suffix' choosing the parameter group that
    ends the head; a leading __attribute__((...)) belongs to the prefix."""
    for m in re.finditer(r'\b(' + IDENT + r')\s*\(', head):
        name = m.group(1)
        if name in ('__attribute__', 'sizeof', 'offsetof'):
            continue
        start = m.end() - 1
        depth = 0
        end = None
        for idx in range(start, len(head)):
            if head[idx] == '(':
                depth += 1
            elif head[idx] == ')':
                depth -= 1
                if depth == 0:
                    end = idx
                    break
        if end is None:
            return None
        suffix = head[end + 1:].strip()
        if suffix and not suffix.startswith('__attribute__'):
            continue
        prefix = head[:m.start()].strip()
        if '(*' in prefix:
            return None
        return prefix, name, head[start + 1:end], suffix
    return None


def format_head(f):
    out = []
    prefix = f.prefix
    if f.suffix:
        prefix = (prefix + ' ' + f.suffix).strip()
    if prefix:
        out.append(prefix)
    if not f.params:
        out.append('%s()' % f.name)
        return out
    out.append('%s(' % f.name)
    for idx, p in enumerate(f.params):
        end = ')' if idx == len(f.params) - 1 else ','
        out.append('\t%s%s' % (p, end))
    return out


def prototype_line(f):
    params = ', '.join(f.params) if f.params else ''
    line = '%s %s(%s)' % (f.prefix, f.name, params)
    if f.suffix:
        line += ' ' + f.suffix
    return line + ';'


# ---------------------------------------------------------------------------
# Passes
# ---------------------------------------------------------------------------

def pass_envelope(src, report):
    lines = src.lines
    # Drop an existing compact one-line copyright or an existing canonical block.
    if lines and lines[0].startswith('/* Copyright (C) 2026 Awe Morris; SPDX'):
        del lines[0]
    elif lines and lines[0] == MODELINE:
        return
    elif lines and lines[0] == '/*' and len(lines) > 5 and lines[1].strip() == '* zedBSD':
        # canonical block without modeline
        pass
    if lines and lines[0] == '/*' and len(lines) > 5 and lines[1].strip() == '* zedBSD':
        end = 0
        while lines[end].strip() != '*/':
            end += 1
        del lines[:end + 1]
    while lines and not lines[0].strip():
        del lines[0]
    src.lines = [MODELINE, ''] + COPYRIGHT + [''] + lines
    src.rebuild()


def pass_defs(src, report):
    funcs = find_functions(src, report)
    for f in reversed(funcs):
        new_head = format_head(f)
        old = src.lines[f.head_start:f.head_end]
        if old != new_head:
            src.lines[f.head_start:f.head_end] = new_head
    src.rebuild()


def collect_static_prototypes(src, funcs, report):
    """Return list of (start, end) line ranges of file-scope static prototypes."""
    names = {f.name for f in funcs if f.is_static and not f.is_inline}
    ranges = []
    lines = src.lines
    i = 0
    while i < len(lines):
        if src.depth_at[i] == 0 and lines[i].startswith('static ') and '(' in src.code_line[i]:
            j = i
            while j < len(lines) and not src.code_line[j].rstrip().endswith(';') \
                    and not src.code_line[j].rstrip().endswith('{'):
                j += 1
            if j < len(lines) and src.code_line[j].rstrip().endswith(';'):
                text = ' '.join(l.strip() for l in lines[i:j + 1])
                m = re.match(r'^static\b.*?\b(' + IDENT + r')\s*\(', text)
                if m and m.group(1) in names and '=' not in text.split('(')[0]:
                    ranges.append((i, j, m.group(1)))
                i = j + 1
                continue
        i += 1
    return ranges


def pass_protos(src, report):
    funcs = find_functions(src, report)
    statics = [f for f in funcs if f.is_static and not f.is_inline]
    if not statics:
        return
    existing = collect_static_prototypes(src, funcs, report)
    existing_names = {name for _, _, name in existing}
    existing_text = {}
    for start, end, name in existing:
        text = ' '.join(l.strip() for l in src.lines[start:end + 1])
        existing_text[name] = re.sub(r'\s+', ' ', text).strip()
    conditional = {f.name for f in funcs if src.pp_depth[f.head_start] > 0}
    # Remove existing prototypes (from bottom up), plus one following blank
    # line when the removal would leave two blank lines.
    for start, end, _ in sorted(existing, reverse=True):
        del src.lines[start:end + 1]
        if start < len(src.lines) and start > 0 and not src.lines[start].strip() \
                and not src.lines[start - 1].strip():
            del src.lines[start]
    src.rebuild()
    funcs = find_functions(src, report)
    statics = [f for f in funcs if f.is_static and not f.is_inline]
    block = []
    for f in statics:
        if f.name in conditional:
            pp = src.pp_depth[f.head_start]
            # Replicate a single-level enclosing #if.
            if pp == 1:
                k = f.head_start
                while k >= 0 and not src.lines[k].lstrip().startswith('#if'):
                    k -= 1
                block.append(src.lines[k].strip())
                block.append(existing_text.get(f.name, prototype_line(f)))
                block.append('#endif')
            else:
                report.append('static %s inside nested #if: no forward declaration' % f.name)
            continue
        block.append(existing_text.get(f.name, prototype_line(f)))
    first = min(funcs, key=lambda f: f.comment_start)
    insert_at = first.comment_start
    # Move file-scope type definitions that follow the first function above
    # the block so prototypes never introduce a conflicting incomplete type.
    moved = []
    i = insert_at
    while i < len(src.lines):
        line = src.lines[i]
        if src.depth_at[i] == 0 and src.pp_depth[i] == 0 and re.match(
                r'^(typedef\b|(struct|union|enum)\s+' + IDENT + r'\s*\{)', line):
            start = i
            while start > 0:
                prev = src.lines[start - 1]
                if prev.strip().endswith('*/'):
                    c = start - 1
                    while c >= 0 and not src.lines[c].lstrip().startswith('/*'):
                        c -= 1
                    if c < 0:
                        break
                    start = c
                elif prev.lstrip().startswith('#define') or not prev.strip():
                    start -= 1
                else:
                    break
            while start < i and not src.lines[start].strip():
                start += 1
            end = i
            while end < len(src.lines) and not (
                    src.code_line[end].rstrip().endswith(';') and
                    (end + 1 >= len(src.lines) or src.depth_at[end + 1] == 0)):
                end += 1
            inside = any(f.head_start <= i <= f.body_end for f in funcs)
            if not inside:
                moved.append(src.lines[start:end + 1])
                del src.lines[start:end + 1]
                if start < len(src.lines) and not src.lines[start].strip() and start > 0 and not src.lines[start - 1].strip():
                    del src.lines[start]
                report.append('moved file-scope type definition from line %d above the forward declarations' % (i + 1))
                src.rebuild()
                funcs = find_functions(src, [])
                i = start
                continue
        i += 1
    prefix_lines = []
    for chunk in moved:
        prefix_lines.extend(chunk + [''])
    # Keep one blank line before and after the block.
    body = prefix_lines + block + ['']
    if insert_at > 0 and src.lines[insert_at - 1].strip():
        body = [''] + body
    src.lines[insert_at:insert_at] = body
    src.rebuild()


def pass_reorder(src, report):
    funcs = find_functions(src, report)
    if len(funcs) < 2:
        return
    lo = min(f.comment_start for f in funcs)
    hi = max(f.body_end for f in funcs)
    # Everything between lo and hi must be functions, blank lines, or the
    # functions' own comment blocks.
    covered = [False] * (hi - lo + 1)
    for f in funcs:
        for k in range(f.comment_start, f.body_end + 1):
            covered[k - lo] = True
    for k in range(lo, hi + 1):
        if not covered[k - lo] and src.lines[k].strip():
            report.append('reorder skipped: non-function content at line %d' % (k + 1))
            return
        if src.lines[k].lstrip().startswith('#'):
            report.append('reorder skipped: preprocessor line at %d' % (k + 1))
            return
    order = [f for f in funcs if f.is_static and f.is_inline] + \
            [f for f in funcs if not f.is_static] + \
            [f for f in funcs if f.is_static and not f.is_inline]
    if [f.name for f in order] == [f.name for f in funcs]:
        return
    chunks = []
    for f in order:
        chunk = src.lines[f.comment_start:f.body_end + 1]
        chunks.append(chunk)
    new = []
    for idx, chunk in enumerate(chunks):
        if idx:
            new.append('')
        new.extend(chunk)
    src.lines[lo:hi + 1] = new
    src.rebuild()


def pass_case(src, report):
    lines = src.lines
    i = 0
    while i < len(lines):
        m = re.match(r'^(\s*)((?:case\b[^:]+|default)\s*:)\s*(\S.*)$', lines[i])
        if m and src.depth_at[i] > 0:
            indent, label, rest = m.groups()
            if rest.startswith('/*') or rest == '{' or rest.startswith('//'):
                i += 1
                continue
            if re.match(r'^(case\b|default\s*:)', rest):
                lines[i:i + 1] = [indent + label, indent + rest]
                src.rebuild()
                continue
            lines[i:i + 1] = [indent + label, indent + '\t' + rest]
            src.rebuild()
            i += 2
            continue
        i += 1
    src.rebuild()


def pass_blank(src, report):
    lines = src.lines
    i = 1
    while i < len(lines):
        if not lines[i].strip() and src.code_line[i - 1].rstrip().endswith('{'):
            del lines[i]
            src.rebuild()
            continue
        i += 1


DECL_RE = re.compile(
    r'^(?P<indent>\t+)(?P<static>static\s+)?(?P<type>(?:(?:const|volatile|unsigned|signed)\s+)*'
    r'(?:(?:struct|union|enum)\s+' + IDENT + r'\b|' + IDENT + r'\b)(?:\s+(?:const|volatile|unsigned|signed|int|long|short|char|double)\b)*)'
    r'(?P<ptr>(?:\s*\*)*(?:\s*\bconst\b)?)(?P<gap>\s*)(?P<name>' + IDENT + r')(?P<array>(?:\[[^\]]*\])*)\s*(?P<rest>=.*|;|,.*)$')


def decl_match(line):
    """DECL_RE with the requirement that a non-pointer declarator is
    separated from its type by whitespace."""
    m = DECL_RE.match(line)
    if m is None:
        return None
    if '*' not in m.group('ptr') and not m.group('gap'):
        return None
    if m.group('type').split()[-1] in ('return', 'goto', 'else', 'sizeof', 'case'):
        return None
    return m


def leading_group_end(src, f):
    """Return (first_decl_line, first_stmt_line) inside function f."""
    lines = src.lines
    i = f.head_end + 1
    first_decl = i
    while i < f.body_end:
        s = src.code_line[i].strip()
        if not lines[i].strip():
            i += 1
            continue
        if lines[i].lstrip().startswith('#') or (i > 0 and lines[i - 1].rstrip().endswith('\\')):
            i += 1
            continue
        m = decl_match(lines[i])
        if m and src.depth_at[i] == 1:
            # Skip a multi-line initializer.
            while i < f.body_end and not src.code_line[i].rstrip().endswith(';'):
                i += 1
            i += 1
            continue
        break
    return first_decl, i


def unique_name(name, leading, params):
    """Return name, or name_N when the function scope already uses it."""
    if name not in leading and name not in params:
        return name
    n = 2
    while ('%s_%d' % (name, n)) in leading or ('%s_%d' % (name, n)) in params:
        n += 1
    return '%s_%d' % (name, n)


def rename_in_block(lines, start, end, old, new):
    """Rename identifier old to new in lines[start:end] (code text only)."""
    pattern = re.compile(r'(?<![A-Za-z0-9_.])(?<!->)' + re.escape(old) + r'(?![A-Za-z0-9_])')
    for k in range(start, end):
        lines[k] = pattern.sub(new, lines[k])


def pass_hoist(src, report):
    funcs = find_functions(src, report)
    changed = False
    for f in reversed(funcs):
        first_decl, first_stmt = leading_group_end(src, f)
        lines = src.lines
        params = set()
        for p in f.params:
            pm = re.search(r'(' + IDENT + r')\s*(?:\[[^\]]*\])*$', p)
            if pm:
                params.add(pm.group(1))
        # Names in the leading group.
        leading = {}
        k = f.head_end + 1
        while k < first_stmt:
            m = decl_match(lines[k])
            if m:
                for d in re.split(r',(?![^\[]*\])', (m.group('name') + m.group('array') + m.group('rest')).rstrip(';')):
                    nm = re.match(r'\s*\**\s*(' + IDENT + r')', d)
                    if nm:
                        leading[nm.group(1)] = (m.group('type') + m.group('ptr')).strip()
            k += 1
        new_decls = []
        i = first_stmt
        scope_names = [dict(leading)]
        kinds = []          # one entry per open brace inside the body
        prev_code = ''
        prev_before = ''
        while i < f.body_end:
            line = lines[i]
            code = src.code_line[i]
            depth = src.depth_at[i]
            # Classify the braces opened on the previous line so that the
            # kind stack matches depth at the start of this line.
            if prev_code:
                for idx, ch in enumerate(prev_code):
                    if ch == '{':
                        before = prev_code[:idx].rstrip()
                        if before == '':
                            before = prev_before
                        aggregate = before.endswith('=') or before.endswith(',') \
                            or re.search(r'\b(struct|union|enum)\b[^;{)]*$', before) is not None \
                            or re.search(r'\)\s*$', before) is not None and '(' in before and \
                            re.search(r'\((?:const\s+)?(?:struct\s+)?' + IDENT + r'[\s\*]*\)\s*$', before) is not None
                        kinds.append('aggregate' if aggregate else 'block')
                    elif ch == '}':
                        if kinds:
                            kinds.pop()
            prev_before = code.rstrip() if code.strip() else (prev_before if prev_code is not None else '')
            prev_code = code
            in_aggregate = 'aggregate' in kinds
            # Maintain a scope stack from brace depth.
            while len(scope_names) < depth:
                scope_names.append({})
            while len(scope_names) > max(depth, 1):
                scope_names.pop()
            in_directive = line.lstrip().startswith('#') or (i > 0 and lines[i - 1].rstrip().endswith('\\'))
            if in_directive:
                i += 1
                continue
            if in_aggregate or src.pp_depth[i] != src.pp_depth[f.head_end]:
                if decl_match(line) and src.pp_depth[i] != src.pp_depth[f.head_end]:
                    report.append('%s: declaration inside #if region at line %d left in place' % (f.name, i + 1))
                i += 1
                continue
            m = decl_match(line)
            fm = re.match(r'^(\s*)for \((?P<type>(?:const\s+)?(?:unsigned\s+|signed\s+)?(?:' + IDENT + r'))\s+(?P<name>' + IDENT + r')\s*=\s*(?P<init>[^;]*);(?P<tail>.*)$', line)
            if fm and is_type_word(fm.group('type').split()[-1]):
                name = fm.group('name')
                typ = fm.group('type')
                clash = any(name in s for s in scope_names[:-1]) or name in params
                if clash or (name in scope_names[-1]):
                    report.append('%s: for-declaration %s shadows/clashes at line %d' % (f.name, name, i + 1))
                    i += 1
                    continue
                final = unique_name(name, leading, params)
                if final != name:
                    # Rename inside the loop's own scope: the loop statement
                    # and its body (the following block at greater depth).
                    e = i + 1
                    while e < f.body_end and (src.depth_at[e] > depth or
                            (src.depth_at[e] == depth and not src.code_line[e].strip())):
                        e += 1
                    if src.code_line[i].rstrip().endswith('{'):
                        pass
                    elif e == i + 1:
                        e = i + 2  # single unbraced statement line
                    rename_in_block(lines, i, e, name, final)
                    report.append('%s: renamed sibling for-declaration %s to %s at line %d' % (f.name, name, final, i + 1))
                new_decls.append('%s %s;' % (typ, final))
                leading[final] = typ
                lines[i] = re.sub(r'for \((?:const\s+)?(?:unsigned\s+|signed\s+)?' + IDENT + r'\s+' + IDENT + r'\s*=', 'for (%s =' % final, lines[i], count=1)
                changed = True
                i += 1
                continue
            if not m or code.strip().startswith('return') or 'goto' in code:
                i += 1
                continue
            if m.group('static'):
                i += 1
                continue
            if depth < 1:
                i += 1
                continue
            typ = (m.group('type') + m.group('ptr')).strip()
            typ = re.sub(r'\s+', ' ', typ)
            name = m.group('name')
            array = m.group('array')
            rest = m.group('rest')
            # Multi-declarator: only when no declarator has an initializer.
            if rest.startswith(','):
                whole = (name + array + rest).rstrip(';')
                if '=' in whole or not code.rstrip().endswith(';'):
                    report.append('%s: multi-declarator with initializer at line %d left in place' % (f.name, i + 1))
                    i += 1
                    continue
                names = []
                bad = False
                for d in whole.split(','):
                    dm = re.match(r'^\s*(\**)\s*(' + IDENT + r')((?:\[[^\]]*\])*)\s*$', d)
                    if not dm:
                        bad = True
                        break
                    names.append((dm.group(1), dm.group(2), dm.group(3)))
                if bad:
                    report.append('%s: multi-declarator at line %d left in place' % (f.name, i + 1))
                    i += 1
                    continue
                if any(any(nm in sc for sc in scope_names[:-1]) or nm in params for _, nm, _ in names):
                    report.append('%s: multi-declarator shadows an enclosing name at line %d' % (f.name, i + 1))
                    i += 1
                    continue
                block_end = i + 1
                while block_end < f.body_end and src.depth_at[block_end] >= depth:
                    block_end += 1
                for stars, nm, arr in names:
                    full = (typ + (' ' + stars if stars else '')).strip()
                    final = unique_name(nm, leading, params)
                    if final != nm:
                        rename_in_block(lines, i + 1, block_end, nm, final)
                        report.append('%s: renamed sibling declaration %s to %s at line %d' % (f.name, nm, final, i + 1))
                    scope_names[-1][final] = full
                    new_decls.append('%s%s%s%s;' % (full, '' if full.endswith('*') else ' ', final, arr))
                    leading[final] = full + arr
                del lines[i]
                src.rebuild()
                changed = True
                f.body_end -= 1
                if first_stmt > i:
                    first_stmt -= 1
                continue
            if rest.startswith('='):
                d = 0
                top_comma = False
                for ch in rest:
                    if ch in '([{':
                        d += 1
                    elif ch in ')]}':
                        d -= 1
                    elif ch == ',' and d == 0:
                        top_comma = True
                        break
                if top_comma:
                    report.append('%s: multi-declarator with initializer at line %d left in place' % (f.name, i + 1))
                    i += 1
                    continue
            # Aggregate initializer: leave.  A multi-line scalar initializer
            # keeps its continuation lines and only loses the declarator.
            if rest.startswith('=') and not code.rstrip().endswith(';'):
                e = i
                while e < f.body_end and not src.code_line[e].rstrip().endswith(';'):
                    e += 1
                joined = ' '.join(src.code_line[i:e + 1])
                if '{' in joined:
                    report.append('%s: aggregate initializer for %s at line %d left in place' % (f.name, name, i + 1))
                    i += 1
                    continue
            elif rest.startswith('=') and '{' in rest:
                report.append('%s: aggregate initializer for %s at line %d left in place' % (f.name, name, i + 1))
                i += 1
                continue
            base_type = typ.replace('const ', '') if not typ.endswith('*') else typ
            clash = any(name in s for s in scope_names[:-1]) or name in params
            if clash:
                report.append('%s: %s shadows an enclosing name at line %d' % (f.name, name, i + 1))
                i += 1
                continue
            final = unique_name(name, leading, params)
            if final != name:
                block_end = i + 1
                while block_end < f.body_end and src.depth_at[block_end] >= depth:
                    block_end += 1
                rename_in_block(lines, i + 1, block_end, name, final)
                report.append('%s: renamed sibling declaration %s to %s at line %d' % (f.name, name, final, i + 1))
                name = final
            scope_names[-1][name] = typ
            const_scalar = typ.startswith('const ') and not typ.endswith('*')
            decl_type = typ
            if const_scalar and rest.startswith('='):
                decl_type = typ[len('const '):]
                report.append('%s: dropped const from %s at line %d' % (f.name, name, i + 1))
            new_decls.append('%s%s%s%s;' % (decl_type, '' if decl_type.endswith('*') else ' ', name, array))
            leading[name] = typ + array
            if rest.startswith('='):
                lines[i] = '%s%s %s' % (m.group('indent'), name, rest)
                changed = True
                i += 1
            else:
                del lines[i]
                src.rebuild()
                changed = True
                f.body_end -= 1
                # do not advance: the next line moved up
                if first_stmt > i:
                    first_stmt -= 1
                continue
        if new_decls:
            insert = first_stmt
            # first_stmt points at the first statement; declarations go before
            # the blank line that separates the groups.
            k = insert
            while k > f.head_end + 1 and not lines[k - 1].strip():
                k -= 1
            if k == f.head_end + 1:
                # No leading group: add one plus a blank line.
                lines[k:k] = ['\t' + d for d in new_decls] + ['']
            else:
                lines[k:k] = ['\t' + d for d in new_decls]
            changed = True
            src.rebuild()
    if changed:
        src.rebuild()


def pass_declblank(src, report):
    funcs = find_functions(src, report)
    for f in reversed(funcs):
        first_decl, first_stmt = leading_group_end(src, f)
        if first_stmt == f.head_end + 1 or first_stmt >= f.body_end:
            continue
        if src.lines[first_stmt - 1].strip():
            src.lines.insert(first_stmt, '')
    src.rebuild()


PASSES = {
    'envelope': pass_envelope,
    'defs': pass_defs,
    'protos': pass_protos,
    'reorder': pass_reorder,
    'case': pass_case,
    'blank': pass_blank,
    'hoist': pass_hoist,
    'declblank': pass_declblank,
}
DEFAULT = ['envelope', 'defs', 'protos', 'reorder', 'case', 'blank', 'hoist', 'declblank']


def main(argv):
    apply = '--apply' in argv
    passes = list(DEFAULT)
    files = []
    for a in argv[1:]:
        if a.startswith('--passes='):
            passes = a[len('--passes='):].split(',')
        elif a == '--apply':
            pass
        else:
            files.append(a)
    for path in files:
        with open(path) as fh:
            text = fh.read()
        src = Source(text)
        report = []
        for p in passes:
            PASSES[p](src, report)
        out = src.text()
        if apply and out != text:
            with open(path, 'w') as fh:
                fh.write(out)
        status = 'changed' if out != text else 'unchanged'
        print('%s: %s' % (path, status))
        for r in report:
            print('  ! %s' % r)


if __name__ == '__main__':
    main(sys.argv)
