#!/usr/bin/env python3
# ws177-p010 (backlog-p2 line 133): keiland-preview against bombs and damaged files (host-preview-fuzz.sh builds it).
# Each run has the file as fd 0 and a new file as fd 1, the limits Files gives the child (linux/spawn.c) when LIMIT is 1,
# and 10 s; it must end with one of the program's statuses 0 to 4 (made, unknown, damaged, too large, no memory),
# never by a signal, and the bombs must be refused (2 to 4) within the time.
#   python3 -I host-preview-fuzz.py PROGRAM FOLDER LIMIT SAMPLES SEED
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import io
import os
import random
import resource
import struct
import subprocess
import sys
import time
import zlib

from PIL import Image

program, folder, limit, samples, seed = sys.argv[1], sys.argv[2], sys.argv[3] == '1', int(sys.argv[4]), int(sys.argv[5])
failures = 0
STATUSES = {0: 'made', 1: 'unknown', 2: 'damaged', 3: 'too large', 4: 'no memory'}


def check(name, passed, detail=''):
    global failures
    print(('ok ' if passed else 'NOT OK ') + name + (': ' + detail if detail else ''))
    if not passed:
        failures += 1


def limits():
    """The child's limits as linux/spawn.c sets them (memory, processor time, what it writes)."""
    if limit:
        resource.setrlimit(resource.RLIMIT_AS, (1 << 30, 1 << 30))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))
    resource.setrlimit(resource.RLIMIT_FSIZE, (48 << 20, 48 << 20))


def run(name, data, *arguments):
    """Runs the program on bytes; returns its status (negative: the signal), its time and its output's size."""
    source = os.path.join(folder, 'input.bin')
    output = os.path.join(folder, 'output.ppm')
    with open(source, 'wb') as f:
        f.write(data)
    started = time.monotonic()
    with open(source, 'rb') as given, open(output, 'wb') as made:
        try:
            result = subprocess.run([program, '--width=256', '--height=256', *arguments], stdin=given, stdout=made,
                                    stderr=subprocess.PIPE, timeout=10, env={}, preexec_fn=limits)
            status = result.returncode
            if status not in STATUSES:
                with open(os.path.join(folder, 'crash-%s.bin' % name), 'wb') as kept:
                    kept.write(data)
                sys.stderr.write(result.stderr.decode(errors='replace')[-2000:])
        except subprocess.TimeoutExpired:
            status = 'timeout'
            with open(os.path.join(folder, 'timeout-%s.bin' % name), 'wb') as kept:
                kept.write(data)
    return status, time.monotonic() - started, os.path.getsize(output)


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def png(width, height, rows):
    """A PNG of 8-bit RGB with the given (already filtered) row bytes, compressed."""
    head = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', head) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')


def pdf(objects):
    out = b'%PDF-1.7\n'
    offsets = []
    for number, text in enumerate(objects, 1):
        offsets.append(len(out))
        out += b'%d 0 obj\n' % number + text + b'\nendobj\n'
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1) + b''.join(b'%010d 00000 n \n' % o for o in offsets)
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objects) + 1, xref)
    return out


def page(box, content, resources=b''):
    return pdf([b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
                b'<< /Type /Page /Parent 2 0 R /MediaBox [%s] %s /Contents 4 0 R >>' % (box, resources),
                b'<< /Length %d >>\nstream\n' % len(content) + content + b'\nendstream'])


def picture(kind):
    image = Image.new('RGB', (300, 200), (220, 40, 40))
    image.paste((40, 60, 220), (0, 100, 300, 200))
    out = io.BytesIO()
    if kind == 'GIF':
        image = image.convert('P', palette=Image.ADAPTIVE, colors=8)
    image.save(out, kind)
    return out.getvalue()


# A font the document does not embed (Helvetica) is drawn with the Mahora the program carries (backlog-p2 line 131):
# large black words on white leave dark pixels where they are.
words = page(b'0 0 400 100', b'1 g 0 0 400 100 re f 0 g BT /F1 60 Tf 10 25 Td (HELLO) Tj ET',
             b'/Resources << /Font << /F1 5 0 R >> >>')
words = words.replace(b'trailer', b'5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\ntrailer', 1)
status, spent, written = run('pdf-words', words, '--width=400', '--height=100')
with open(os.path.join(folder, 'output.ppm'), 'rb') as made:
    data = made.read()
dark = 0
if status == 0 and data.startswith(b'P6\n'):
    pixels = data.split(b'\n255\n', 1)[1]
    dark = sum(1 for at in range(0, len(pixels), 3) if pixels[at] < 96)
check('font not embedded drawn', status == 0 and dark > 400, 'status %s, %d dark pixels' % (status, dark))

# The bombs: each must stop with a status of its own (too large or no memory), in time.
bombs = [
    ('png-huge-header', png(65535, 65535, b'\0' * 64)),
    ('png-zlib-bomb', png(8192, 8192, b'\0' * ((8192 * 3 + 1) * 8192))),
    ('png-wide-row', png(1 << 24, 1, b'\0' * 32)),
    ('jpeg-huge-header', picture('JPEG').replace(struct.pack('>HH', 200, 300), struct.pack('>HH', 65000, 65000), 1)),
    ('gif-huge-screen', picture('GIF')[:6] + struct.pack('<HH', 65535, 65535) + picture('GIF')[10:]),
    ('ppm-huge-header', b'P6\n100000 100000\n255\n' + b'\0' * 1024),
    ('input-past-64mib', b'P6\n4 4\n255\n' + b'\0' * (65 << 20)),
    ('pdf-huge-page', page(b'0 0 1000000000 1000000000', b'0 g 0 0 100 100 re f')),
]
# A bomb is refused (damaged when its decoder refuses the header itself, too large or no memory); a huge page may be
# drawn at the size asked.
expected = {'pdf-huge-page': (0, 2, 3, 4)}
for name, data in bombs:
    status, spent, written = run(name, data)
    wanted = expected.get(name, (2, 3, 4))
    check('bomb ' + name, status in wanted and spent < 10, 'status %s in %.2f s, %d bytes out' % (status, spent, written))

# Documents that cost time or depth: a deep array, a long content stream, a page tree that names itself.
deep = page(b'0 0 100 100', b'0 g ' + b'[' * 200000 + b']' * 200000 + b' 0 0 10 10 re f')
check_name = 'pdf-deep-array'
status, spent, written = run(check_name, deep)
check('hostile ' + check_name, status in STATUSES and spent < 10, 'status %s in %.2f s' % (status, spent))
long_content = page(b'0 0 100 100', b'0 g 0 0 1 1 re f ' * 2000000)
status, spent, written = run('pdf-long-content', long_content)
check('hostile pdf-long-content', status in STATUSES and spent < 10, 'status %s in %.2f s' % (status, spent))
loop = pdf([b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [2 0 R] /Count 1 >>'])
status, spent, written = run('pdf-page-loop', loop)
check('hostile pdf-page-loop', status in STATUSES and spent < 10, 'status %s in %.2f s' % (status, spent))
self_length = pdf([b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
                   b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>',
                   b'<< /Length 4 0 R >>\nstream\n0 g 0 0 10 10 re f\nendstream'])
status, spent, written = run('pdf-length-self', self_length)
check('hostile pdf-length-self', status in STATUSES and spent < 10, 'status %s in %.2f s' % (status, spent))

# The damaged files: good ones changed at random (bytes flipped or set, a cut, a piece repeated or put elsewhere).
good = {'png': picture('PNG'), 'jpeg': picture('JPEG'), 'gif': picture('GIF'), 'ppm': picture('PPM'),
        'pdf': page(b'0 0 300 200', b'0.8 0.2 0.2 rg 0 0 300 200 re f 0 g BT /F1 24 Tf 10 10 Td (Preview) Tj ET',
                    b'/Resources << /Font << /F1 5 0 R >> >>')}
chance = random.Random(seed)
counts = {}
worst = 0.0
for index in range(samples):
    kind = sorted(good)[index % len(good)]
    data = bytearray(good[kind])
    for _ in range(chance.randint(1, 8)):
        how = chance.randrange(5)
        at = chance.randrange(len(data))
        if how == 0:
            data[at] ^= 1 << chance.randrange(8)
        elif how == 1:
            data[at] = chance.choice((0, 0xff, 0x7f, 0x80, chance.randrange(256)))
        elif how == 2:
            del data[at:]
            if not data:
                data = bytearray(b'\x89')
        elif how == 3:
            size = chance.randint(1, 64)
            data[at:at] = data[at:at + size] * chance.randint(1, 50)
        else:
            size = chance.randint(1, 16)
            piece = data[at:at + size]
            del data[at:at + size]
            place = chance.randrange(len(data) + 1)
            data[place:place] = piece
    status, spent, written = run('%s-%d' % (kind, index), bytes(data))
    worst = max(worst, spent)
    counts[status] = counts.get(status, 0) + 1
    if status not in STATUSES:
        check('damaged %s-%d' % (kind, index), False, 'status %s' % status)
summary = ', '.join('%s %d' % (STATUSES.get(status, status), count) for status, count in sorted(counts.items(), key=str))
check('damaged files (%d, seed %d)' % (samples, seed), all(status in STATUSES for status in counts), summary + ', slowest %.2f s' % worst)

print('host-preview-fuzz: ' + ('PASS' if failures == 0 else '%d FAILED' % failures))
sys.exit(1 if failures else 0)
