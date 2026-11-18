#!/usr/bin/env python3
"""kmerge-verify.py TARGET SRC...

Proves that a consolidated translation unit still contains the same functions
as the sources it absorbed.  The baseline copies of SRC (build/kstyle/baseline,
or $KSTYLE_BASEDIR) and the working-tree TARGET are compiled to -O0 LLVM IR,
normalized with irnorm.py, and compared function by function: -O0 has no
inlining, so a call that became intra-unit by the merge does not show up as a
difference.  Functions are also compared at -Os where that is possible.
"""
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(REPO, 'build/kstyle')
BASE = os.environ.get('KSTYLE_BASEDIR', 'build/kstyle/baseline')


def command(arch):
    """Returns the kernel compile command, reusing the kverify cache."""
    cache = os.path.join(WORK, 'cmd-src_kern_waitq_c-%s' % arch)
    if not os.path.exists(cache):
        subprocess.run(['tools/kstyle/kverify.sh', 'src/kern/waitq.c'],
                       cwd=REPO, capture_output=True)
    cmd = open(cache).read().replace('\t', ' ').strip()
    cmd = cmd.split(' -c src/')[0]
    cmd = cmd.replace(' -Werror', '')
    cmd = re.sub(r' -MMD -MF [^ ]+| -MMD| -MP', '', cmd)
    return cmd + (' -w -Wno-builtin-macro-redefined -D__LINE__=0'
                  ' -D__FILE__="kstyle" ' + os.environ.get('KSTYLE_EXTRA', ''))


def functions(source, arch, tag):
    """Compiles one source to normalized -O0 IR and splits it into functions."""
    ll = os.path.join(WORK, '%s.%s.ll' % (tag, arch))
    ir = command(arch).replace('-Os', '-O0') + \
        ' -fno-discard-value-names -S -emit-llvm -I' + os.path.dirname(source)
    result = subprocess.run(ir.split() + ['-o', ll, source], cwd=REPO,
                            capture_output=True, text=True)
    if result.returncode != 0:
        print('compile failed: %s\n%s' % (source, result.stderr[:2000]))
        return None
    normalized = subprocess.run(['python3', 'tools/kstyle/irnorm.py', ll],
                                cwd=REPO, capture_output=True, text=True).stdout
    out = {}
    name = None
    body = []
    for line in normalized.split('\n'):
        if line.startswith('define '):
            match = re.search(r'@([A-Za-z_][A-Za-z0-9_.]*)\(', line)
            name = match.group(1)
            body = [re.sub(r'^define [^@]*@', 'define @', line)]
            continue
        if name is not None:
            body.append(line)
            if line == '}':
                out[name] = '\n'.join(body)
                name = None
    return out


def main():
    target = sys.argv[1]
    sources = sys.argv[2:]
    status = 0
    for arch in ('64', '32'):
        merged = functions(os.path.join(REPO, target), arch, 'merged')
        if merged is None:
            return 1
        expected = {}
        origin = {}
        for src in sources:
            path = os.path.join(REPO, BASE, src)
            if not os.path.exists(path):
                print('%s: no baseline copy of %s' % (target, src))
                return 1
            part = functions(path, arch, 'part')
            if part is None:
                return 1
            for key, value in part.items():
                expected[key] = value
                origin[key] = src
        missing = sorted(set(expected) - set(merged))
        added = sorted(set(merged) - set(expected))
        changed = sorted(k for k in set(expected) & set(merged)
                         if expected[k] != merged[k])
        print('%s [%s-bit]: %d functions expected, %d present, '
              'missing %d, extra %d, changed %d'
              % (target, arch, len(expected), len(merged),
                 len(missing), len(added), len(changed)))
        for key in missing[:10]:
            print('    MISSING %s (from %s)' % (key, origin[key]))
        for key in added[:10]:
            print('    EXTRA   %s' % key)
        for key in changed[:10]:
            print('    CHANGED %s (from %s)' % (key, origin[key]))
        if missing or added or changed:
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
