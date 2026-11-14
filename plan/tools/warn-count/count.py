#!/usr/bin/python3 -I
"""Count compiler warnings per package from the logs cc-log.sh kept.

Usage: count.py LOG_DIR [REPO_ROOT]

LOG_DIR holds one file per object, named after the object's path under
build/<platform>/packages/ with '/' turned into '%'; the first component
is the package name.  Prints Markdown tables: per package (warnings, the
distinct source locations, objects compiled), per flag, and the
package count without warnings.
"""
import collections
import os
import re
import sys

WARN = re.compile(r'^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*?)(?: \[(?P<flag>-W[^\],]+)[^\]]*\])?$')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    log_dir = sys.argv[1]
    root = os.path.abspath(sys.argv[2]) + '/' if len(sys.argv) > 2 else ''
    objects = collections.Counter()
    per_pkg = collections.Counter()
    per_pkg_loc = collections.defaultdict(set)
    per_pkg_flags = collections.defaultdict(collections.Counter)
    per_flag = collections.Counter()
    per_flag_loc = collections.defaultdict(set)
    all_loc = set()
    for name in sorted(os.listdir(log_dir)):
        if not name.endswith('.log'):
            continue
        pkg = name.split('%', 1)[0]
        objects[pkg] += 1
        with open(os.path.join(log_dir, name), errors='replace') as f:
            for line in f:
                m = WARN.match(line.rstrip('\n'))
                if not m:
                    continue
                flag = m.group('flag') or '(none)'
                path = m.group('file')
                if root and path.startswith(root):
                    path = path[len(root):]
                loc = (path, m.group('line'), m.group('col'), flag)
                per_pkg[pkg] += 1
                per_pkg_loc[pkg].add(loc)
                per_pkg_flags[pkg][flag] += 1
                per_flag[flag] += 1
                per_flag_loc[flag].add(loc)
                all_loc.add(loc)
    clean = sorted(p for p in objects if per_pkg[p] == 0)
    print('# Warning count\n')
    print('- packages compiled: %d' % len(objects))
    print('- packages with warnings: %d' % (len(objects) - len(clean)))
    print('- packages without warnings: %d' % len(clean))
    print('- warnings (per compile): %d' % sum(per_pkg.values()))
    print('- distinct locations (file:line:col:flag): %d\n' % len(all_loc))
    print('## Per flag\n')
    print('| flag | warnings | distinct locations |')
    print('|---|---:|---:|')
    for flag, n in per_flag.most_common():
        print('| %s | %d | %d |' % (flag, n, len(per_flag_loc[flag])))
    print('\n## Per package (with warnings)\n')
    print('| package | warnings | distinct locations | objects | flags |')
    print('|---|---:|---:|---:|---|')
    for pkg, n in sorted(per_pkg.items(), key=lambda kv: (-kv[1], kv[0])):
        if n == 0:
            continue
        flags = ', '.join('%s %d' % (f, c) for f, c in per_pkg_flags[pkg].most_common())
        print('| %s | %d | %d | %d | %s |' % (pkg, n, len(per_pkg_loc[pkg]), objects[pkg], flags))
    print('\n## Packages without warnings\n')
    print(' '.join(clean))


main()
