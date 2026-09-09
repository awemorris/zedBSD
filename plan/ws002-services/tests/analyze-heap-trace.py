#!/usr/bin/env python3
"""Decode the private amd64 allocator provenance ring from stopped snapshots."""
from pathlib import Path
import json
import struct
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
capture = json.loads((root / 'capture.json').read_text())
kernel = root / 'campaign/build/amd64/vmunix'
for index in range(len(capture.get('samples', []))):
    data = (root / f'kernel_heap_trace-{index}.bin').read_bytes()
    assert len(data) == 2048 * 40
    sequence, = struct.unpack('<Q', (root / f'kernel_heap_trace_sequence-{index}.bin').read_bytes())
    failed, = struct.unpack('<I', (root / f'kernel_heap_trace_failed-{index}.bin').read_bytes())
    entries = []
    for offset in range(0, len(data), 40):
        values = struct.unpack_from('<4Q2I', data, offset)
        if values[0]:
            entries.append(dict(zip(['sequence', 'caller', 'pointer', 'size', 'cpu', 'event'], values)))
    entries.sort(key=lambda row: row['sequence'])
    assert not entries or entries[-1]['sequence'] == sequence
    callers = sorted({row['caller'] for row in entries if row['caller']})
    names = subprocess.check_output(['addr2line', '-f', '-e', str(kernel),
                                    *[hex(address) for address in callers]], text=True).splitlines()
    assert len(names) == len(callers) * 2
    locations = {address: names[n * 2:n * 2 + 2] for n, address in enumerate(callers)}
    for row in entries:
        row['location'] = locations.get(row['caller'])
    (root / f'trace-{index}.json').write_text(json.dumps(
        {'sequence': sequence, 'failed_event': failed, 'entries': entries}, indent=2) + '\n')
    print('sample', index, 'failed', failed, 'sequence', sequence)
    for row in entries[-20:]:
        print(row['sequence'], row['event'], hex(row['pointer']), row['size'], row['location'])
