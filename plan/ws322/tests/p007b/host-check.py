# The host check of the JVT streams for the VF2's VPU (ws322-p007b): for each stream, jvt-host writes what the Wave5
# engine puts in its ring, ffmpeg decodes it, and its frames are compared with the reference's (make-jvt.sh's .sha256):
# in order, and as a set (a frame decoded right but printed elsewhere is in the set).  The probe's display order is
# compared with the true one (the order counts between IDR pictures) from jvt-host's lines.
#   python3 -I plan/ws322/tests/p007b/host-check.py JVT-HOST JVT OUT [NAME...]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import subprocess
import sys


def hashes(path):
    run = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', path, '-pix_fmt', 'nv12',
                          '-f', 'framehash', '-hash', 'sha256', '-'], capture_output=True, text=True, timeout=600)
    return [line.split(',')[-1].strip() for line in run.stdout.splitlines() if line and not line.startswith('#')]


def main():
    tool, jvt, out = sys.argv[1:4]
    names = sys.argv[4:] or sorted(n[:-5] for n in os.listdir(jvt) if n.endswith('.h264'))
    os.makedirs(out, exist_ok=True)
    for name in names:
        source = os.path.join(jvt, name + '.h264')
        rewritten = os.path.join(out, name + '.ring.h264')
        with open(os.path.join(jvt, name + '.sha256')) as f:
            reference = [line.strip() for line in f if line.strip()]
        run = subprocess.run([tool, source, rewritten], capture_output=True, text=True, timeout=600)
        with open(os.path.join(out, name + '.txt'), 'w') as f:
            f.write(run.stdout)
        pictures = []
        printed = []
        stop = ''
        for line in run.stdout.splitlines():
            words = line.split()
            if words[0] == 'P':
                pictures.append((int(words[1]), int(words[23]), int(words[25])))
            elif words[0] == 'O':
                printed.append(int(words[1]))
            elif words[0] == 'STOP':
                stop = line
        # The true display order: by the place between IDR pictures and operations 5.
        true = [index for _, _, index in sorted((period, place, index) for index, place, period in pictures)]
        decoded = hashes(rewritten) if pictures else []
        original = hashes(source)
        same = sum(1 for a, b in zip(decoded, reference) if a == b)
        known = set(reference)
        found = sum(1 for h in decoded if h in known)
        plain = sum(1 for a, b in zip(original, reference) if a == b)
        # What the probe would print: the ring's frames (in the true order) in the probe's order.
        where = {index: place for place, index in enumerate(true)}
        probe = [decoded[where[index]] if where[index] < len(decoded) else '' for index in printed]
        predicted = sum(1 for a, b in zip(probe, reference) if a == b)
        order = sum(1 for a, b in zip(printed, true) if a != b)
        print(f'{name}: pictures {len(pictures)} ref {len(reference)} | ring decode {len(decoded)} in-order {same} '
              f'in-set {found} | original {len(original)} in-order {plain} | probe-order-misplaced {order} '
              f'probe-predicted-match {predicted} | {stop}',
              flush=True)


main()
