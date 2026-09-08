#!/usr/bin/env python3
"""kdelta.py TARGET BASELINE_SOURCE...

Compares the optimized code of a rewritten unit against the sources it came
from, function by function.

Paragraphing that only adds blank lines and comments leaves the code exactly
as it was.  Turning nested conditions into guard clauses does not: the
control-flow graph changes, so the -O0 IR no longer matches.  This tool
therefore reports, for every function, the optimized instruction count and
the multiset of symbols it calls.  A guard-clause rewrite is expected to keep
the calls identical and the instruction count within a couple of
instructions; anything else needs to be looked at.
"""
import collections
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(REPO, 'build/kstyle')
BASE = os.environ.get('KSTYLE_BASEDIR', 'build/kstyle/baseline')
OBJDUMP = os.path.join(REPO, 'build/llvm/bin/llvm-objdump')


def command():
    cache = os.path.join(WORK, 'cmd-src_kern_waitq_c-64')
    if not os.path.exists(cache):
        subprocess.run(['tools/kstyle/kverify.sh', 'src/kern/waitq.c'],
                       cwd=REPO, capture_output=True)
    cmd = open(cache).read().replace('\t', ' ').strip().split(' -c src/')[0]
    cmd = cmd.replace(' -Werror', '')
    cmd = re.sub(r' -MMD -MF [^ ]+| -MMD| -MP', '', cmd)
    return cmd + (' -w -Wno-builtin-macro-redefined -D__LINE__=0'
                  ' -D__FILE__="kstyle" ' + os.environ.get('KSTYLE_EXTRA', ''))


def profile(source, tag):
    """Compiles one source and profiles every function it defines."""
    obj = os.path.join(WORK, '%s.delta.o' % tag)
    result = subprocess.run(
        command().split() + ['-I' + os.path.dirname(source), '-c', source, '-o', obj],
        cwd=REPO, capture_output=True, text=True)
    if result.returncode != 0:
        print('compile failed: %s\n%s' % (source, result.stderr[:1500]))
        return None
    text = subprocess.run([OBJDUMP, '-d', '-r', '--no-show-raw-insn', obj],
                          cwd=REPO, capture_output=True, text=True).stdout
    out = {}
    name = None
    for line in text.split('\n'):
        head = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
        if head:
            name = head.group(1)
            out[name] = [0, collections.Counter()]
            continue
        if name is None:
            continue
        if re.match(r'^\s+[0-9a-f]+:\s+\S', line):
            out[name][0] += 1
        call = re.search(r'R_[A-Z0-9_]+\s+([A-Za-z_]\w*)', line)
        if call:
            out[name][1][call.group(1)] += 1
    return out


def main():
    target = sys.argv[1]
    sources = sys.argv[2:]
    new = profile(os.path.join(REPO, target), 'new')
    if new is None:
        return 1
    old = {}
    for src in sources:
        part = profile(os.path.join(REPO, BASE, src), 'old')
        if part is None:
            return 1
        old.update(part)
    shared = sorted(set(old) & set(new))
    same = 0
    drift = []
    calls = []
    for key in shared:
        if old[key][1] != new[key][1]:
            calls.append(key)
        elif old[key][0] == new[key][0]:
            same += 1
        else:
            drift.append((abs(old[key][0] - new[key][0]), key,
                          old[key][0], new[key][0]))
    print('%s: %d functions compared, %d byte-for-byte in size, '
          '%d size drift, %d call-set changes'
          % (target, len(shared), same, len(drift), len(calls)))
    for delta, key, a, b in sorted(drift, reverse=True)[:15]:
        print('    SIZE %-44s %4d -> %4d (%+d)' % (key, a, b, b - a))
    for key in calls[:15]:
        print('    CALLS %-43s %s -> %s' % (key, dict(old[key][1]), dict(new[key][1])))
    return 1 if calls else 0


if __name__ == '__main__':
    sys.exit(main())
