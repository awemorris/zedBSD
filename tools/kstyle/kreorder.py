#!/usr/bin/env python3
"""kreorder.py FILE...

Rewrites one C source into the file order of plan/coding-style.md:

    file-wide macros and constants, enums, structures and other type
    definitions, file-scope variables, forward declarations, public function
    definitions, static function definitions.

Includes stay directly under the file header, and the relative order inside
each group is preserved so that a definition never moves ahead of something
it depends on.  Consolidation markers are removed.  Every comment stays
attached to the item it introduces.
"""
import re
import sys

FUNCTION_HEAD = re.compile(r'^[A-Za-z_]\w*\s*\(')
MARKER = re.compile(r'^/\* (?:Begin|End) consolidated \S+\. \*/$')


def split_items(lines):
    """Splits top-level lines into (comment, body) items."""
    items = []
    pending = []
    drop_next_comment = [False]
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()
        if MARKER.match(stripped):
            if stripped.startswith('/* Begin'):
                drop_next_comment[0] = True
            pending = []
            i += 1
            continue
        if stripped == '':
            pending.append(line)
            i += 1
            continue
        if stripped.startswith('/*') and '*/' not in stripped:
            block = [line]
            i += 1
            while i < n and '*/' not in lines[i]:
                block.append(lines[i])
                i += 1
            if i < n:
                block.append(lines[i])
                i += 1
            if drop_next_comment[0]:
                drop_next_comment[0] = False
                pending = []
                continue
            pending.extend(block)
            continue
        if stripped.startswith('/*') and stripped.endswith('*/'):
            pending.append(line)
            i += 1
            continue
        drop_next_comment[0] = False
        if stripped.startswith('#'):
            block = [line]
            if re.match(r'^#\s*(if|ifdef|ifndef)', stripped):
                # A conditional group moves as one item so that the lines it
                # guards never leave it.
                level = 1
                i += 1
                while i < n and level > 0:
                    block.append(lines[i])
                    guard = lines[i].strip()
                    if re.match(r'^#\s*(if|ifdef|ifndef)', guard):
                        level += 1
                    elif re.match(r'^#\s*endif', guard):
                        level -= 1
                    i += 1
                items.append((pending, block))
                pending = []
                continue
            while block[-1].rstrip().endswith('\\') and i + 1 < n:
                i += 1
                block.append(lines[i])
            items.append((pending, block))
            pending = []
            i += 1
            continue
        # An ordinary declaration or definition: read to its end.
        block = []
        depth = 0
        started = False
        while i < n:
            block.append(lines[i])
            depth += lines[i].count('{') - lines[i].count('}')
            if lines[i].count('{'):
                started = True
            if started and depth == 0:
                i += 1
                break
            if not started and depth == 0 and lines[i].rstrip().endswith(';'):
                i += 1
                break
            i += 1
        items.append((pending, block))
        pending = []
    return items, pending


def classify(body):
    """Names the group one item belongs to."""
    text = '\n'.join(body)
    first = body[0].strip()
    if first.startswith('#include'):
        return 'include'
    if first.startswith('#'):
        return 'macro'
    has_body = '{' in text and not text.rstrip().endswith(';')
    if has_body:
        head = body[0]
        # A definition head is the return type; the name follows on the
        # next line in the repository style, or on the same line.
        if re.match(r'^\s*(static\b|__attribute__.*static\b)', head):
            return 'static'
        if len(body) > 1 and body[1].strip().startswith('static '):
            return 'static'
        return 'public'
    if re.match(r'^\s*(typedef\s+)?(struct|union|enum)\b', first) and '{' in text:
        return 'enum' if first.lstrip().startswith('enum') else 'type'
    if first.startswith('typedef'):
        return 'type'
    if '(' in text and text.rstrip().endswith(');'):
        return 'proto'
    return 'var'


def reorder(path):
    text = open(path).read()
    lines = text.split('\n')
    # The file header is the copyright block plus the description block that
    # follows it; both are comment blocks before the first real item.
    head = []
    i = 0
    blocks = 0
    while i < len(lines) and blocks < 2:
        if lines[i].strip() == '':
            head.append(lines[i])
            i += 1
            continue
        if not lines[i].lstrip().startswith('/*'):
            break
        while i < len(lines):
            head.append(lines[i])
            done = '*/' in lines[i]
            i += 1
            if done:
                break
        blocks += 1
    while head and head[-1].strip() == '':
        head.pop()
    collapsed = []
    for line in head:
        if line.strip() == '' and collapsed and collapsed[-1].strip() == '':
            continue
        collapsed.append(line)
    head = collapsed
    items, trailing = split_items(lines[i:])
    groups = {k: [] for k in
              ('include', 'macro', 'enum', 'type', 'var', 'proto', 'var_after',
               'public', 'static')}
    for comment, body in items:
        groups[classify(body)].append((comment, body))
    out = list(head)
    if groups['include']:
        out.append('')
        seen = set()
        for comment, body in groups['include']:
            key = body[0].strip()
            if key in seen:
                continue
            seen.add(key)
            clean = [c for c in comment if c.strip() != '']
            if clean and out and out[-1].strip() != '':
                out.append('')
            out.extend(clean)
            out.extend(body)
    # A file-scope table that names a function needs that function declared
    # first, so such a variable follows the forward declarations.
    declared = set()
    for comment, body in groups['proto']:
        for name in re.findall(r'\b([A-Za-z_]\w*)\s*\(', '\n'.join(body)):
            declared.add(name)
    late = []
    early = []
    for item in groups['var']:
        text = '\n'.join(item[1])
        if any(re.search(r'\b%s\b' % re.escape(name), text) for name in declared):
            late.append(item)
        else:
            early.append(item)
    groups['var'] = early
    groups['var_after'] = late
    previous = 'include'
    for kind in ('macro', 'enum', 'type', 'var', 'proto', 'var_after'):
        for comment, body in groups[kind]:
            clean = [c for c in comment if c.strip() != '']
            if out and out[-1].strip() != '':
                if clean or kind != 'proto' or previous != 'proto':
                    out.append('')
            previous = kind
            out.extend(clean)
            out.extend(body)
    for kind in ('public', 'static'):
        for comment, body in groups[kind]:
            clean = [c for c in comment if c.strip() != '']
            if out and out[-1].strip() != '':
                out.append('')
            out.extend(clean)
            out.extend(body)
    while out and out[-1].strip() == '':
        out.pop()
    open(path, 'w').write('\n'.join(out) + '\n')
    return {k: len(v) for k, v in groups.items()}


if __name__ == '__main__':
    for name in sys.argv[1:]:
        counts = reorder(name)
        print('%-38s %s' % (name, ' '.join('%s=%d' % kv for kv in counts.items() if kv[1])))
