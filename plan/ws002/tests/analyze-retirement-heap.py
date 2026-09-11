#!/usr/bin/env python3
"""Validate captured amd64 heap links without dereferencing guest pointers."""
from pathlib import Path
import json
import struct
import sys

directory = Path(sys.argv[1]).resolve()
capture = json.loads((directory / 'capture.json').read_text())
base = capture['owners']['kernel_heap_storage']['address']
reports = []
for index in range(len(capture['samples'])):
    memory = (directory / f'kernel_heap_storage-{index}.bin').read_bytes()
    state = struct.unpack('<16Q', (directory / f'kernel_heap-{index}.bin').read_bytes())
    begin, end, first, free_head = state[2:6]
    assert begin == base and end == base + len(memory)
    report = {'sample': index, 'allocator': dict(zip(
        ['original_base', 'original_size', 'begin', 'end', 'first', 'free_list',
         'current_bytes', 'peak_bytes', 'largest_failed', 'errors', 'fail_after',
         'allocations', 'observer', 'observer_context', 'grow', 'grow_context'], state)),
        'physical': [], 'errors': []}
    def block_at(address):
        if address < begin or address > end - 56 or address % 8:
            raise ValueError(f'out-of-heap/unaligned block {address:#x}')
        fields = struct.unpack_from('<II6Q', memory, address - base)
        return dict(zip(['magic', 'state', 'capacity', 'used', 'previous_physical',
                         'next_physical', 'previous_free', 'next_free'], fields))
    seen = set()
    pointer, previous, expected = first, 0, begin
    used = 0
    free_physical = set()
    while pointer:
        if pointer in seen:
            report['errors'].append(f'physical cycle to {pointer:#x}')
            break
        seen.add(pointer)
        try:
            block = block_at(pointer)
        except ValueError as error:
            report['errors'].append(str(error))
            break
        report['physical'].append({'address': pointer, **block})
        for condition, reason in [
            (pointer == expected, 'noncontiguous physical address'),
            (block['magic'] == 0x42393848, 'bad magic'),
            (block['state'] in (0x46524545, 0x55534544), 'bad state'),
            (block['previous_physical'] == previous, 'bad previous link'),
            (block['capacity'] <= end - pointer - 56, 'capacity exceeds heap'),
            (block['used'] <= block['capacity'], 'used exceeds capacity')]:
            if not condition:
                report['errors'].append(f'{pointer:#x}: {reason}')
        if block['state'] == 0x55534544:
            used += block['used']
        else:
            free_physical.add(pointer)
        expected = pointer + 56 + block['capacity']
        previous, pointer = pointer, block['next_physical']
    if expected != end:
        report['errors'].append(f'physical extent ends at {expected:#x}, expected {end:#x}')
    if used != state[6]:
        report['errors'].append(f'used-byte sum {used} != counter {state[6]}')
    free_seen = set()
    pointer, previous = free_head, 0
    while pointer:
        if pointer in free_seen:
            report['errors'].append(f'free-list cycle to {pointer:#x}')
            break
        free_seen.add(pointer)
        try:
            block = block_at(pointer)
        except ValueError as error:
            report['errors'].append(str(error))
            break
        if pointer not in free_physical or block['previous_free'] != previous:
            report['errors'].append(f'{pointer:#x}: free-list ownership mismatch')
        previous, pointer = pointer, block['next_free']
    if free_seen != free_physical:
        report['errors'].append('free list differs from physical free blocks')
    reports.append(report)
(directory / 'heap-analysis.json').write_text(json.dumps(reports, indent=2) + '\n')
for report in reports:
    print('sample', report['sample'], 'blocks', len(report['physical']),
          'errors', report['errors'])
