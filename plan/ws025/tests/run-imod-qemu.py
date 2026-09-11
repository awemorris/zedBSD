#!/usr/bin/env python3
"""Compare isolated IMOD artifacts using production USB/HID acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]


class QMP:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(5)
        try:
            self.socket.connect(str(path))
            self.stream = self.socket.makefile('rwb', buffering=0)
            self.greeting = json.loads(self.stream.readline())
            self.sequence = 0
            self.call('qmp_capabilities')
        except BaseException:
            self.socket.close()
            raise

    def call(self, name, arguments=None):
        self.sequence += 1
        request = {'execute': name, 'id': self.sequence}
        if arguments is not None:
            request['arguments'] = arguments
        self.stream.write((json.dumps(request) + '\n').encode())
        while True:
            line = self.stream.readline()
            if not line:
                raise EOFError('QMP closed before register capture')
            reply = json.loads(line)
            if reply.get('id') == self.sequence:
                if 'error' in reply:
                    raise RuntimeError(reply)
                return reply['return']

    def read32(self, address):
        raw = self.call('human-monitor-command',
                        {'command-line': f'xp /1wx 0x{address:x}'})
        match = re.search(r':\s+0x([0-9a-fA-F]+)', raw)
        if match is None:
            raise RuntimeError(f'cannot decode MMIO read: {raw!r}')
        return int(match[1], 16), raw

    def close(self):
        self.stream.close()
        self.socket.close()


def devices(buses):
    for bus in buses:
        for device in bus['devices']:
            yield device
            bridge = device.get('pci_bridge')
            if bridge:
                yield from devices([bridge['bus']])


def capture(path, expected):
    qmp = QMP(path)
    try:
        pci = qmp.call('query-pci')
        matches = [d for d in devices(pci) if d.get('qdev_id') == 'xhci']
        if len(matches) != 1:
            raise RuntimeError(f'expected one xHCI, found {matches}')
        bars = [r for r in matches[0]['regions'] if r['bar'] == 0]
        if len(bars) != 1 or bars[0]['address'] <= 0:
            raise RuntimeError(f'invalid BAR0: {bars}')
        base = bars[0]['address']
        rtsoff, raw_offset = qmp.read32(base + 0x18)
        offset = rtsoff & ~31
        if offset < 0x20 or offset + 0x28 > bars[0]['size']:
            raise RuntimeError(f'RTSOFF outside BAR0: {offset:x}')
        value, raw_imod = qmp.read32(base + offset + 0x24)
        result = dict(pci=pci, bar0=base, rtsoff=rtsoff, register=value,
                      interval=value & 0xffff, expected=expected,
                      raw_rtsoff=raw_offset, raw_imod=raw_imod)
        if result['interval'] != expected:
            raise RuntimeError(f'IMOD readback mismatch: {result}')
        return result
    finally:
        qmp.close()


def storage_measurements(text):
    rows = re.findall(r'IMOD-STORAGE SAMPLE (\d+) write_ns=(\d+) fsync_ns=(\d+) read_ns=(\d+)\r?$', text, re.M)
    if [int(row[0]) for row in rows] != list(range(64)):
        raise RuntimeError('missing, duplicate or reordered storage samples')
    ends = re.findall(r'IMOD-STORAGE PASS samples=64 bytes=65536 wall_ns=(\d+) process_cpu_us=(\d+)\r?$', text, re.M)
    if len(ends) != 1:
        raise RuntimeError('missing unique confirmed write/readback completion')
    start = text.index('IMOD-STORAGE READY ')
    end = text.index('IMOD-STORAGE PASS ')
    concurrent = text[start:end]
    if 'USB-HID-GUEST RECORD PASS role=relative' not in concurrent:
        raise RuntimeError('HID record did not complete during storage workload')
    resolutions = re.findall(r'IMOD-STORAGE READY [^\n]*clock_resolution_ns=(\d+)', text)
    if len(resolutions) != 1 or int(resolutions[0]) <= 0:
        raise RuntimeError('missing positive guest clock resolution')
    irq_rows = re.findall(r'IMOD-STORAGE IRQ entry=(\d+) owned=(\d+) events=(\d+) duration_ns=(\d+)\r?$', text, re.M)
    if len(irq_rows) != 1:
        raise RuntimeError('missing unique IRQ measurement')
    entry, owned, events, duration = map(int, irq_rows[0])
    if not (entry > 0 and owned > 0 and events > 0 and duration > 0):
        raise RuntimeError('invalid or inactive xHCI IRQ sample')
    irq = dict(entries=entry, owned=owned, events=events, duration_ns=duration,
               entries_per_second=entry * 1e9 / duration,
               owned_per_second=owned * 1e9 / duration,
               scope='Aggregate xHCI callbacks; independently sampled counters at interval boundaries, not IRQ service latency')
    metrics = {}
    for column, name in enumerate(('write', 'fsync', 'readback'), 1):
        samples = [int(row[column]) for row in rows]
        ordered = sorted(samples)
        metrics[name] = dict(samples_ns=samples, minimum_ns=ordered[0],
                             maximum_ns=ordered[-1],
                             **{f'p{p}_ns': ordered[(len(ordered) * p + 99) // 100 - 1]
                                for p in (50, 95, 99)})
    return dict(irq=irq, operations=metrics, bytes_per_sample=65536, samples=64,
                clock_resolution_ns=int(resolutions[0]),
                wall_ns=int(ends[0][0]), process_cpu_us=int(ends[0][1]),
                hid_overlap=True,
                scope='Guest cached-file operations and tick-accounted process CPU; paced wall time. No physical IRQ or controller latency claim.')


def run_cell(out, value, usb2=False):
    campaign = out / f'i{value}'
    env = dict(os.environ, USB_HID_QEMU_CELLS='xhci',
               USB_HID_XHCI_IMOD=str(value), USB_HID_QMP_DIAGNOSTICS='1',
               USB_HID_IMOD_STORAGE='1', USB_HID_XHCI_USB2_ONLY='1' if usb2 else '0')
    record = {'setting': value, 'usb2_only': usb2}
    started = time.monotonic()
    with (out / f'i{value}.log').open('w') as log:
        process = subprocess.Popen(
            ['bash', str(REPO / 'plan/ws006/tests/qemu-usb-hid-acceptance.sh'),
             str(campaign)], cwd=REPO, env=env, stdout=log,
            stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = started + 2400
            guest_log = campaign / 'xhci/guest.log'
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('bounded IMOD campaign exceeded 2400 s')
                text = guest_log.read_text(errors='replace') if guest_log.exists() else ''
                if 'readback' not in record and 'xhci: PCI controller,' in text:
                    record['readback'] = capture(campaign / 'xhci/observe.sock', value)
                    (out / f'i{value}-readback.json').write_text(
                        json.dumps(record['readback'], indent=2) + '\n')
                    print(f'IMOD {value}: actual register verified', flush=True)
                time.sleep(.25)
            record['exit_code'] = process.returncode
            if process.returncode != 0:
                raise RuntimeError(f'IMOD {value} campaign exit {process.returncode}')
            if 'readback' not in record:
                raise RuntimeError('campaign ended without actual IMOD readback')
            record['functional_results'] = (campaign / 'results.tsv').read_text()
            record['storage'] = storage_measurements(guest_log.read_text(errors='replace'))
            if usb2:
                resets = re.findall(r'xhci: port 4 reset complete portsc=([0-9a-fA-F]+)',
                                    guest_log.read_text(errors='replace'))
                if not resets or any((int(word, 16) >> 10) & 15 != 3 for word in resets):
                    raise RuntimeError('USB2 root speed was not confirmed')
                record['root_speed_id'] = 3
            record['result'] = 'PASS functional campaign and actual interval'
            print(f'IMOD {value}: functional campaign PASS', flush=True)
        except BaseException as error:
            record['failure'] = str(error)
            raise
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            record['wall_seconds_including_build'] = time.monotonic() - started
            (out / f'i{value}-result.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--usb2', action='store_true')
    parser.add_argument('--values', nargs='+', type=int, choices=[0, 160, 4000],
                        default=[0, 160, 4000])
    args = parser.parse_args()
    out = args.output.resolve()
    out.relative_to(REPO / 'plan/ws025/temp')
    out.mkdir(parents=True, exist_ok=False)
    source = REPO / 'src/drivers/pci/pci-xhci.c'
    digest = lambda: hashlib.sha256(source.read_bytes()).hexdigest()
    result = dict(source_sha256=digest(),
                  workload_sha256=hashlib.sha256((REPO / 'plan/ws025/tests/imod-storage-guest.c').read_bytes()).hexdigest(), cells=[],
                  scope='QEMU functional comparison; no physical latency/CPU claim')
    try:
        for value in args.values:
            result['cells'].append(run_cell(out, value, args.usb2))
        result['result'] = 'PASS'
    finally:
        result['source_sha256_after'] = digest()
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if result['source_sha256'] != result['source_sha256_after']:
        raise RuntimeError('production xHCI source changed during comparison')


if __name__ == '__main__':
    main()
