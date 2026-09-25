#!/usr/bin/env python3
"""ws035-p007: checks the WAV that QEMU's wav audiodev recorded.

  hda-wav-check.py pattern FILE FRAMES   the guest's counting pattern
                                          (left = n, right = ~n) appears
                                          whole and in order
  hda-wav-check.py patterns FILE N1 N2 ... several runs of the pattern, in
                                          order, each after the previous one
  hda-wav-check.py peak FILE              prints the largest |sample|
  hda-wav-check.py windows FILE           prints the largest |sample| of each
                                          quarter second

The data chunk is found by walking the RIFF chunks; a header QEMU never
finalised (killed QEMU) is tolerated by reading to the end of the file.
"""
import struct
import sys


def samples(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        sys.exit('not a WAV file')
    offset = 12
    while offset + 8 <= len(data):
        kind, size = struct.unpack_from('<4sI', data, offset)
        if kind == b'fmt ':
            fmt, channels, rate, _, _, bits = struct.unpack_from('<HHIIHH', data, offset + 8)
            info = (fmt, channels, rate, bits)
        if kind == b'data':
            body = data[offset + 8:]
            body = body[:len(body) - len(body) % 4]
            return info, struct.unpack('<%dh' % (len(body) // 2), body)
        offset += 8 + size
    sys.exit('no data chunk')


def main():
    mode, path = sys.argv[1], sys.argv[2]
    info, values = samples(path)
    print('wav: format=%d channels=%d rate=%d bits=%d frames=%d' % (info + (len(values) // 2,)))
    if mode == 'peak':
        print('peak=%d' % max((abs(v) for v in values), default=0))
        return 0
    if mode == 'windows':
        step = info[2] // 4 * info[1]
        peaks = [max((abs(v) for v in values[i:i + step]), default=0)
                 for i in range(0, len(values), step)]
        print('windows:', ' '.join(str(p) for p in peaks))
        return 0

    left = values[0::2]
    right = values[1::2]
    if mode == 'patterns':
        after = 0
        for count in sys.argv[3:]:
            start = find_run(left, right, after, int(count))
            if start is None:
                return 1
            after = start + int(count)
        return 0
    start = find_run(left, right, 0, int(sys.argv[3]))
    if start is None:
        return 1
    leading = values[:start * 2]
    print('pattern: leading non-silent samples=%d' % sum(1 for v in leading if v != 0))
    return 0


def find_run(left, right, after, frames):
    """Checks one run of the pattern that starts at or after frame after."""
    try:
        start = next(i for i in range(after, len(left)) if left[i] == 0 and right[i] == -1)
    except StopIteration:
        print('pattern: start not found')
        return None
    for n in range(frames):
        i = start + n
        if i >= len(left):
            print('pattern: truncated at frame %d of %d' % (n, frames))
            return None
        want_left = struct.unpack('<h', struct.pack('<H', n & 0xffff))[0]
        want_right = struct.unpack('<h', struct.pack('<H', (~n) & 0xffff))[0]
        if left[i] != want_left or right[i] != want_right:
            print('pattern: mismatch at frame %d (got %d,%d)' % (n, left[i], right[i]))
            return None
    print('pattern: %d frames bit-exact from frame %d' % (frames, start))
    return start


sys.exit(main())
