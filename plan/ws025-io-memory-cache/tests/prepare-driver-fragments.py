#!/usr/bin/env python3
"""Extract host-test translation units from the current consolidated drivers.

These files are disposable views, never alternate production implementations.
The native kernel builds and runtime gates exercise the complete translation units.
"""
from pathlib import Path
import hashlib
import json
import re

REPO = Path(__file__).resolve().parents[3]
PHASE = REPO / 'plan/ws025-io-memory-cache/phase031-driver-layout-style'
OUTPUT = REPO / 'plan/ws025-io-memory-cache/temp/p031-driver-fragments'


def prepare():
    rows = [line.split('\t') for line in (PHASE / 'source-map.tsv').read_text().splitlines()[1:]]
    counts = {}
    for old, new in rows:
        counts[new] = counts.get(new, 0) + 1
    dependencies = json.loads((PHASE / "fragment-headers.json").read_text())
    manifest = {}
    for old, new in rows:
        if counts[new] == 1:
            continue
        source = (REPO / new).read_text()
        name = Path(old).name
        begin = '/* Begin consolidated ' + name + '. */'
        end = '/* End consolidated ' + name + '. */'
        if source.count(begin) != 1 or source.count(end) != 1:
            raise RuntimeError('Missing or ambiguous production section: ' + old)
        fragment = source.split(begin, 1)[1].split(end, 1)[0]
        fragment = "".join('#include "' + str(OUTPUT.relative_to(REPO) / header) + '"\n' for header in dependencies.get(old, [])) + fragment
        def resolve_include(match):
            path = (REPO / new).parent / match[1]
            if path.is_file():
                return '#include "' + str(path.resolve()) + '"'
            return match[0]
        fragment = re.sub(r'#include "([^\"]+)"', resolve_include, fragment)
        target = OUTPUT / old
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text() != fragment:
            target.write_text(fragment)
        manifest[old] = {'production': new, 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                         'fragment_sha256': hashlib.sha256(fragment.encode()).hexdigest()}
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    prepare()
