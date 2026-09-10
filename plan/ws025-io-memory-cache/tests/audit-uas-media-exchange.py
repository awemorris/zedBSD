#!/usr/bin/env python3
"""Audit native UAS medium replacement without USB reattachment."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import json
from pathlib import Path
import re
import struct
import sys


def audit(directory):
    result = json.loads((directory / 'result.json').read_text())
    assert result.get('source_unchanged') and result.get('removable')
    assert result.get('replacement_sha256') == '219325ec03e898e5510ad21c78a41cbf80fca74c50f064bd872fb728d85704ef'
    text = (directory / 'guest.log').read_text(errors='replace')
    assert text.count('driver=usb-uas') == 1, 'USB class rebound during exchange'
    assert len(re.findall(r'usb-uas: sd[a-z]+ blocks=', text)) == 2
    raw = (directory / 'uas.pcap').read_bytes()
    offset = 24
    senses = []
    while offset < len(raw):
        assert offset + 16 <= len(raw)
        length = struct.unpack_from('<I', raw, offset + 8)[0]
        offset += 16
        packet = raw[offset:offset + length]
        assert len(packet) == length
        offset += length
        if len(packet) < 96 or packet[8] != ord('C') or packet[9] != 3 or packet[10] != 0x82:
            continue
        iu = packet[64:]
        if iu[0] != 3 or iu[6] != 2:
            continue
        sense = iu[16:]
        if len(sense) >= 14 and sense[0] & 0x7f == 0x70:
            senses.append([sense[2] & 15, sense[12], sense[13]])
    absent = next(i for i, s in enumerate(senses) if s == [2, 0x3a, 0])
    assert any(s == [6, 0x28, 0] for s in senses[absent + 1:]), 'no medium-change attention after absence'
    evidence = {'result': 'PASS absence/change sense, two publications and one USB binding', 'senses': senses}
    (directory / 'media-protocol.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(directory.name + ': ' + evidence['result'])


if __name__ == '__main__':
    audit(Path(sys.argv[1]))
