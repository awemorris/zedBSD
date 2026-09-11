#!/usr/bin/env python3
"""Generate malformed compiler ELF inputs; acceptance is tested by the loader."""
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
rows = ['arch\tcase\texpected']
for arch in ('amd64', 'i386'):
    raw = (root / (arch + '.elf')).read_bytes()
    wide = raw[4] == 2
    fmt = '<IIQQQQQQ' if wide else '<IIIIIIII'
    phoff = struct.unpack_from('<Q' if wide else '<I', raw, 32 if wide else 28)[0]
    entsize, count = struct.unpack_from('<HH', raw, 54 if wide else 42)
    headers = [list(struct.unpack_from(fmt, raw, phoff + i * entsize)) for i in range(count)]
    tls = [i for i, h in enumerate(headers) if h[0] == 7]
    assert len(tls) == 1
    index = tls[0]
    h = headers[index]
    off, va, fs, ms, al = (2, 3, 5, 6, 7) if wide else (1, 2, 4, 5, 7)
    assert h[al] == 64 and h[fs] == 4 and h[ms] > h[fs]
    text = (root / (arch + '.disassembly')).read_text()
    assert ('%fs:' if wide else '%gs:') in text
    rows.append(f'{arch}\tvalid\tload')
    for kind in ('empty', 'zero', 'offset'):
        assert (root / f'{arch}-{kind}.elf').is_file()
        rows.append(f'{arch}\t{kind}\tload')
    cases = {
        'filesz-over-memsz': {fs: h[ms] + 1},
        'file-outside': {off: len(raw) + 1},
        'file-truncated-range': {off: len(raw) - 1},
        'alignment-three': {al: 3},
        'alignment-over-limit': {al: 8192},
        'address-overflow': {va: (1 << (64 if wide else 32)) - 2},
        'memory-over-limit': {ms: 1024 * 1024 + 1},
        'offset-incongruent': {off: h[off] + 1},
    }
    for name, changes in cases.items():
        mutated = bytearray(raw)
        header = h.copy()
        for field, value in changes.items():
            header[field] = value
        struct.pack_into(fmt, mutated, phoff + index * entsize, *header)
        (root / f'{arch}-{name}.elf').write_bytes(mutated)
        rows.append(f'{arch}\t{name}\tENOEXEC')
    (root / f'{arch}-truncated.elf').write_bytes(raw[:h[off] + h[fs] - 1])
    rows.append(f'{arch}\ttruncated\tENOEXEC')
    mutated = bytearray(raw)
    spare = next(i for i, header in enumerate(headers) if header[0] == 0x6474e551)
    struct.pack_into(fmt, mutated, phoff + spare * entsize, *h)
    (root / f'{arch}-duplicate.elf').write_bytes(mutated)
    rows.append(f'{arch}\tduplicate\tENOEXEC')
(root / 'manifest.tsv').write_text('\n'.join(rows) + '\n')
print('TLS compiler fixtures: PASS (8 valid, 20 malformed; FS/GS local-exec)')
