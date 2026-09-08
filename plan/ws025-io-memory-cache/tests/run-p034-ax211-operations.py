#!/usr/bin/env python3
"""Bounded direct-wifi operation sequences on AX211; credentials use stdin."""
import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import random
import re
import sys
import time

path = Path(__file__).with_name('run-p033-ax211-session.py')
spec = importlib.util.spec_from_file_location('p033', path)
p033 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p033)
# Background connections exercise down while another command owns the attempt.
p033.REMOTE_CONTROL = p033.REMOTE_CONTROL.replace("keys={' '", "keys={'&':'shift-7',' '")


def scan_ready(session):
    session.command('wifi wlan0 search start')
    begin = time.monotonic()
    while time.monotonic()-begin < 90:
        status = session.command('wifi wlan0 status')
        if 'scan=complete' in status:
            return
        time.sleep(3)
    raise RuntimeError('preparatory full scan did not complete')


def run(session, seed, rounds):
    rng = random.Random(seed)
    credentials = session.credentials
    connect = 'wifi wlan0 connect '+credentials['ssid']+' '+credentials['key']
    session.command('net wifi disable')
    session.command('wifi wlan0 up')
    for attempt in range(3):
        scan_ready(session)
        session.command(connect, timeout=60)
        if 'state=connected' in session.command('wifi wlan0 status'):
            session.event('direct-initial-connection-pass', attempt=attempt+1)
            break
    else:
        raise RuntimeError('direct connection precondition failed')
    session.event('random-sequence-start', seed=seed, rounds=rounds)
    for index in range(rounds):
        # Every third operation interrupts a fresh connect at a recorded delay.
        if index % 3 == 2:
            session.command('wifi wlan0 up')
            scan_ready(session)
            delay = rng.choice([0, 1, 3])
            command = connect+' & sleep '+str(delay)+'; wifi wlan0 down; wait'
        else:
            command = rng.choice(['wifi wlan0 up', 'wifi wlan0 down', connect,
                                  'wifi wlan0 disconnect'])
        session.event('operation-start', index=index, command=session.redact(command))
        session.command(command, timeout=180)
    session.command('wifi wlan0 down')
    offset = len(session.raw)
    begin = time.monotonic()
    while time.monotonic()-begin < 20:
        session.control()
        time.sleep(1)
    diagnostics = [line for line in session.raw[offset:].splitlines()
                   if 'intel-ax211:' in line and re.search(r'failed|error=|rejected', line)]
    counts = Counter(session.redact(line) for line in diagnostics)
    session.event('idle-observation', seconds=20, diagnostic_count=len(diagnostics),
                  most_common=counts.most_common(3))
    if any(count >= 5 for count in counts.values()):
        raise RuntimeError('same AX211 failure continued during idle observation')
    status = session.command('wifi wlan0 status')
    if 'administrative=down' not in status or 'stop-pending=yes' in status:
        raise RuntimeError('down did not retire interface')
    session.command('net wifi set-key '+credentials['ssid']+' '+credentials['key']+' auto')
    session.command('net wifi enable')
    session.connected('managed-recovery-after-direct-operations')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='awe@10.0.10.25')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--remote', required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=882211)
    parser.add_argument('--rounds', type=int, default=12)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 24:
        parser.error('rounds must be between 1 and 24')
    credentials = json.load(sys.stdin)
    for key in ['ssid', 'key']:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', credentials[key]):
            raise RuntimeError('simple shell-safe credentials required')
    session = p033.Session(args, credentials)
    try:
        session.start()
        run(session, args.seed, args.rounds)
        session.event('scenarios-pass')
    except Exception as error:
        session.event('failed', message=session.redact(str(error)))
        raise SystemExit(1)
    finally:
        session.stop()


if __name__ == '__main__':
    main()
