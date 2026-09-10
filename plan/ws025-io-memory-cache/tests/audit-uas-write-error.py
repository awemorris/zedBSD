#!/usr/bin/env python3
"""Prove the native write-error cell reached WRITE(16), not its RMW read."""
import json
from pathlib import Path
import struct
import sys

out = Path(sys.argv[1])
raw = (out / 'uas.pcap').read_bytes()
assert struct.unpack_from('<I', raw)[0] == 0xa1b2c3d4
packets = []
offset = 24
while offset < len(raw):
    size = struct.unpack_from('<I', raw, offset + 8)[0]
    offset += 16
    packet = raw[offset:offset + size]
    offset += size
    assert len(packet) == size
    if len(packet) < 68 or packet[9] != 3:
        continue
    if (packet[8] == ord('S') and packet[10] == 1) or (packet[8] == ord('C') and packet[10] == 0x82):
        packets.append(packet[64:])
writes = [(i, p) for i, p in enumerate(packets) if len(p) >= 32 and p[0] == 1 and p[16] == 0x8a and int.from_bytes(p[18:26], 'big') == 16384]
assert len(writes) == 1, 'expected one failed WRITE(16) at 8 MiB'
i, command = writes[0]
tag = command[2:4]
timeout = '--timeout' in sys.argv[2:]
assert not any(len(p) >= 32 and p[0] == 1 and p[16] == 0x8a and int.from_bytes(p[18:26], 'big') == 32768 for p in packets), 'later write escaped sticky failure'
result = {'tag': int.from_bytes(tag, 'big'), 'command': command.hex()}
if timeout:
    run = json.loads((out / 'result.json').read_text())
    assert 'UASWRITE TIMEOUT' in run['write_error_output']
    trace = (out / 'write-timeout.trace').read_text()
    if run['speed'] == 'super':
        assert 'xhci: port 2 reset complete' in run['write_error_output']
        assert 'usb_uas_reset' in trace
        assert any(len(p) >= 32 and p[0] == 1 and p[16] == 0x12 for p in packets[i+1:]), 'missing reprobe'
    else:
        aborts = [(j, p) for j, p in enumerate(packets[i+1:], i+1) if p[0] == 5]
        assert len(aborts) == 1
        j, abort = aborts[0]
        assert len(abort) == 16 and abort[4] == 1 and abort[6:8] == tag and abort[2:4] != tag
        response = next(p for p in packets[j+1:] if p[0] == 4 and p[2:4] == abort[2:4])
        assert len(response) == 8 and response[7] == 0
        assert 'usb_uas_tmf_abort_task' in trace
    result['result'] = 'PASS one timed-out WRITE, recovery, no WRITE replay or later write'
else:
    sense = next(p for p in packets[i+1:] if len(p) >= 16 and p[0] == 3 and p[2:4] == tag)
    assert sense[6] == 2, 'write must complete CHECK CONDITION'
    result['sense'] = sense.hex()
    result['result'] = 'PASS failed WRITE(16), CHECK CONDITION, later write not submitted'
(out / 'write-protocol.json').write_text(json.dumps(result, indent=2) + '\n')
print(result['result'])
