#!/usr/bin/env python3
# WS032: every dynamic symbol a package needs must be provided by something
# that will be loaded with it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Linking succeeds with --allow-shlib-undefined, which the tree needs because
# libc.so takes __rtld_exports from the loader.  That flag also hides a symbol
# nothing defines, which would only appear as a failure to start on the target.
# This checks the closure on the host instead.
#
# usage: check-unresolved-symbols.py <readelf> <elf>...
import subprocess
import sys


def dynamic_symbols(readelf, path):
    """Returns the (undefined, defined) dynamic symbol names of one ELF."""
    output = subprocess.run([readelf, '--dyn-syms', path],
                            capture_output=True, text=True, check=True).stdout
    undefined, defined = set(), set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(':'):
            continue
        index, name = fields[6], fields[7].split('@')[0]
        if not name:
            continue
        (undefined if index == 'UND' else defined).add(name)
    return undefined, defined


def main(argv):
    if len(argv) < 3:
        print('usage: %s <readelf> <elf>...' % argv[0], file=sys.stderr)
        return 2
    readelf, paths = argv[1], argv[2:]

    provided = set()
    wanted = {}
    for path in paths:
        undefined, defined = dynamic_symbols(readelf, path)
        provided |= defined
        wanted[path] = undefined

    failures = 0
    for path, undefined in wanted.items():
        missing = sorted(undefined - provided)
        if missing:
            failures += 1
            print('%s: %d unresolved: %s' % (path, len(missing), missing[:16]),
                  file=sys.stderr)
    print('unresolved-symbols: %d file(s) checked, %d with unresolved symbols'
          % (len(paths), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
