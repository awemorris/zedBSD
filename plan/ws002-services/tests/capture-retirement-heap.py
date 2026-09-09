#!/usr/bin/env python3
"""Bounded paired-I/O replay and QMP heap capture, not an acceptance oracle."""
from pathlib import Path
import json
import os
import re
import socket
import subprocess
import sys
import time

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws002-services/temp')
out.mkdir(parents=True, exist_ok=False)
trace_mode = sys.argv[2:] == ['--trace']
if sys.argv[2:] and not trace_mode:
    raise ValueError('only --trace is supported after the output directory')
campaign = out / 'campaign'
env = dict(os.environ, USB_HID_QEMU_CELLS='paired',
           USB_HID_QMP_DIAGNOSTICS='1', BOOT_TIMEOUT_SECONDS='90',
           COMMAND_TIMEOUT_SECONDS='180', CELL_TIMEOUT_SECONDS='600')
if trace_mode:
    env['USB_HID_HEAP_TRACE'] = '1'
log = (out / 'campaign.log').open('w')
proc = subprocess.Popen(['bash', str(repo / 'plan/ws006-input/tests/qemu-usb-hid-acceptance.sh'),
                         str(campaign)], cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT)
connection = None
channel = None
result = {'purpose': 'diagnostic replay; stopped snapshots are not acceptance'}
result['trace_enabled'] = trace_mode
result['retirement_churn'] = env.get('USB_HID_RETIREMENT_CHURN', '0')
try:
    deadline = time.monotonic() + 600
    guest_log = campaign / 'paired/guest.log'
    while proc.poll() is None and time.monotonic() < deadline:
        text = guest_log.read_text(errors='replace') if guest_log.exists() else ''
        if ('USB-HID-GUEST STORAGE READY' in text or (trace_mode and 'boot:' in text)) and time.time() - guest_log.stat().st_mtime > 15:
            break
        time.sleep(.5)
    if proc.poll() is None and ('USB-HID-GUEST STORAGE READY' in text or trace_mode):
        connection = socket.socket(socket.AF_UNIX)
        connection.settimeout(10)
        connection.connect(str(campaign / 'paired/observe.sock'))
        channel = connection.makefile('rwb', buffering=0)
        result['greeting'] = json.loads(channel.readline())
        sequence = 0
        def command(name, arguments=None):
            global sequence
            sequence += 1
            request = {'execute': name, 'id': sequence}
            if arguments is not None:
                request['arguments'] = arguments
            channel.write((json.dumps(request) + '\n').encode())
            while True:
                line = channel.readline()
                if not line:
                    raise EOFError('QEMU closed diagnostic channel')
                reply = json.loads(line)
                if reply.get('id') == sequence:
                    if 'error' in reply:
                        raise RuntimeError(reply)
                    return reply['return']
        def hmp(query):
            return command('human-monitor-command', {'command-line': query})
        command('qmp_capabilities')
        kernel = campaign / 'build/amd64/vmunix'
        symbols = subprocess.check_output(['nm', '-S', str(kernel)], text=True)
        owners = {}
        names = ['kernel_heap', 'kernel_heap_storage']
        if trace_mode:
            names += ['kernel_heap_trace', 'kernel_heap_trace_sequence', 'kernel_heap_trace_failed']
        for name in names:
            match = re.search(r'^([0-9a-f]+) ([0-9a-f]+) \w ' + name + '$', symbols, re.M)
            if match is None:
                raise RuntimeError('missing symbol ' + name)
            owners[name] = {'address': int(match[1], 16), 'size': int(match[2], 16)}
        result['owners'] = owners
        result['samples'] = []
        # A structural failure flag is definitive.  Independently retain a
        # bounded watchdog suspicion when a CPU remains inside the allocator
        # walk/check with no trace progress; this is capture evidence only.
        watchdog_ranges = []
        for name in ['pointer_block', 'heap_allocator_trace_validate']:
            match = re.search(r'^([0-9a-f]+) ([0-9a-f]+) \w ' + name + '$', symbols, re.M)
            if match is not None:
                start = int(match[1], 16)
                watchdog_ranges.append((name, start, start + int(match[2], 16)))
        result['watchdog_ranges'] = [dict(name=name, start=start, end=end)
                                     for name, start, end in watchdog_ranges]
        result['watchdog_observations'] = []
        take_capture = not trace_mode
        trigger = 'scheduled snapshots' if take_capture else None
        sustained = {}
        while trace_mode and proc.poll() is None and time.monotonic() < deadline:
            try:
                reply = hmp(f'x /1wx 0x{owners["kernel_heap_trace_failed"]["address"]:x}')
            except (EOFError, BrokenPipeError, ConnectionResetError):
                break
            value = re.search(r':\s+0x([0-9a-f]+)', reply)
            if value is None:
                raise RuntimeError(reply)
            if int(value[1], 16):
                take_capture = True
                trigger = 'structural failure flag'
                break
            sequence_reply = hmp(f'x /1gx 0x{owners["kernel_heap_trace_sequence"]["address"]:x}')
            sequence_value = re.search(r':\s+0x([0-9a-f]+)', sequence_reply)
            registers = hmp('info registers -a')
            now = time.monotonic()
            active = set()
            parts = re.split(r'CPU#(\d+)', registers)
            for part in range(1, len(parts), 2):
                cpu = int(parts[part])
                rip = re.search(r'RIP=([0-9a-f]+)', parts[part + 1])
                if rip is None:
                    continue
                address = int(rip[1], 16)
                for name, start, end in watchdog_ranges:
                    if start <= address < end:
                        active.add((cpu, name))
                        sequence_number = int(sequence_value[1], 16) if sequence_value else None
                        prior = sustained.get((cpu, name))
                        if prior is None or prior['sequence'] != sequence_number:
                            sustained[(cpu, name)] = {
                                'since': now, 'sequence': sequence_number, 'samples': 1}
                        else:
                            prior['samples'] += 1
                            if now - prior['since'] >= 1.5 and prior['samples'] >= 4:
                                result['watchdog_observations'].append({
                                    'cpu': cpu, 'symbol': name,
                                    'seconds': now - prior['since'],
                                    'samples': prior['samples'],
                                    'trace_sequence': sequence_number,
                                    'registers': registers})
                                take_capture = True
                                trigger = 'sustained allocator walk suspicion'
                                break
                if take_capture:
                    break
            for key in list(sustained):
                if key not in active:
                    del sustained[key]
            if take_capture:
                break
            time.sleep(.5)
        result['capture_trigger'] = trigger
        for index in range(3 if take_capture else 0):
            command('stop')
            try:
                sample = {'registers': hmp('info registers -a'), 'usb': hmp('info usb')}
                hmp('cpu 0')
                for name, owner in owners.items():
                    destination = out / f'{name}-{index}.bin'
                    reply = hmp(f'memsave 0x{owner["address"]:x} 0x{owner["size"]:x} "{destination}"')
                    sample[name] = reply
                    if not destination.exists() or destination.stat().st_size != owner['size']:
                        raise RuntimeError(f'memsave failed: {reply}')
                sample['stacks'] = {}
                parts = re.split(r'CPU#(\d+)', sample['registers'])
                for part in range(1, len(parts), 2):
                    frame = re.search(r'RBP=([0-9a-f]+)', parts[part + 1])
                    if frame is not None:
                        sample['stacks'][parts[part]] = hmp(
                            f'x /64gx 0x{int(frame[1], 16):x}')
                result['samples'].append(sample)
            finally:
                command('cont')
            (out / 'capture.json').write_text(json.dumps(result, indent=2) + '\n')
            time.sleep(2)
    result['campaign_exit'] = proc.wait(timeout=max(1, deadline - time.monotonic()))
except BaseException as error:
    result['capture_error'] = str(error)
    raise
finally:
    if channel is not None:
        channel.close()
    if connection is not None:
        connection.close()
    if proc.poll() is None:
        # The campaign has its own bounded QEMU timeout and cleanup.
        proc.wait(timeout=650)
    log.close()
    (out / 'capture.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({'campaign_exit': result.get('campaign_exit'),
                  'samples': len(result.get('samples', []))}))
