#!/usr/bin/env python3
"""Observe real AX211 plus exact USB RTL8822BU selection and recovery."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import time

spec = importlib.util.spec_from_file_location('p033', Path(__file__).with_name('run-p033-ax211-session.py'))
p033 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p033)


def connected(session, label, started):
    last = {}
    while time.monotonic()-started < 180:
        winners = []
        addresses = []
        for interface in ['wlan0', 'wlan1']:
            output = session.command('wifi '+interface+' status; ifconfig '+interface)
            match = re.search(r'^state=(\S+) scan=(\S+) administrative=(\S+)', output, re.M)
            if match is None:
                raise RuntimeError('missing WLAN status: '+interface)
            state = match.groups()
            address = re.search(r'inet (?!0\.0\.0\.0)([0-9.]+)', output)
            if address:
                addresses.append(interface)
            observation = state + (address.group(1) if address else '',)
            if last.get(interface) != observation:
                session.event('radio-observed', scenario=label, interface=interface,
                              state=state[0], scan=state[1], administrative=state[2],
                              address=observation[3], since_action=round(time.monotonic()-started, 3))
                last[interface] = observation
            if state[0] == 'connected' and address:
                winners.append(interface)
        if len(winners) > 1 or len(addresses) > 1:
            raise RuntimeError('multiple managed L2/L3 owners')
        if winners:
            ping = session.command('ping -c 3 192.168.2.1', timeout=30)
            if '3 packets transmitted, 3 packets received' not in ping:
                raise RuntimeError('selected radio failed LAN ping')
            session.event('dual-connected-pass', scenario=label, interface=winners[0],
                          seconds=round(time.monotonic()-started, 3))
            if not session.args.baseline_only:
                loser = 'wlan1' if winners[0] == 'wlan0' else 'wlan0'
                stopped = False
                for attempt in range(8):
                    output = session.command('wifi '+loser+' status; ifconfig '+loser)
                    if 'scan=scanning' not in output and 'stop-pending=no' in output:
                        if 'authorized=yes' in output or re.search(r'inet (?!0\.0\.0\.0)[0-9.]+', output):
                            raise RuntimeError('losing radio retains L2/L3 ownership')
                        stopped = True
                        break
                    time.sleep(1)
                if not stopped:
                    raise RuntimeError('losing radio scan did not stop')
                processes = session.command('ps -A')
                children = len(re.findall(r'^.*\bdhcpc\b.*$', processes, re.M))
                if children > 1:
                    raise RuntimeError('multiple DHCP children remain')
                session.event('single-owner-pass', scenario=label, loser=loser, dhcp_children=children)
            return
        time.sleep(1)
    raise RuntimeError('dual-radio automatic connection did not recover')


def run(session):
    c = session.credentials
    profile = 'net wifi set-key '+c['ssid']+' '+c['key']+' auto'
    interfaces = session.command('ifconfig wlan0; ifconfig wlan1')
    if not all(re.search(r'^'+name+r': flags=', interfaces, re.M) for name in ['wlan0', 'wlan1']):
        raise RuntimeError('two WLAN interfaces were not published')
    started = time.monotonic()
    session.event('dual-action-start', scenario=session.args.scenario)
    if session.args.scenario == 'enable-first':
        session.command('net wifi enable')
        session.command(profile)
    else:
        session.command(profile)
        session.command('net wifi enable')
    connected(session, session.args.scenario, started)
    if session.args.baseline_only:
        return
    offers_before = len(re.findall(r'dhcpc: wlan[01]: offered ', session.raw))
    started = time.monotonic()
    session.command('net wifi enable')
    connected(session, 'repeated-enable', started)
    if len(re.findall(r'dhcpc: wlan[01]: offered ', session.raw)) != offers_before:
        raise RuntimeError('repeated enable restarted an established DHCP session')
    session.event('repeated-enable-preserved-lease')
    session.command('net wifi disable')
    started = time.monotonic()
    session.command('net wifi enable')
    connected(session, 'disable-enable', started)
    session.command('net wifi disable')
    interrupted = session.command('net wifi enable; wifi wlan0 status; wifi wlan1 status; net wifi disable')
    if not re.search(r'scan=scanning|state=authenticating|state=associating', interrupted):
        raise RuntimeError('cancellation did not observe an active scan or association')
    session.event('inflight-cancellation-observed')
    started = time.monotonic()
    session.command('net wifi enable')
    connected(session, 'cancel-scan-enable', started)
    for interface in ['wlan0', 'wlan1']:
        session.command('wifi '+interface+' status')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='awe@10.0.10.25')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--remote', required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--usb-wlan-port', default='3-3')
    parser.add_argument('--scenario', choices=['enable-first', 'key-first'], default='enable-first')
    parser.add_argument('--baseline-only', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[1-9][0-9]*-[1-9][0-9]*(?:\.[1-9][0-9]*)*', args.usb_wlan_port):
        parser.error('USB topology must be bus-port[.port]')
    credentials = json.load(sys.stdin)
    for key in ['ssid', 'key']:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', credentials[key]):
            raise RuntimeError('simple shell-safe credentials required')
    session = p033.Session(args, credentials)
    try:
        session.start()
        run(session)
        session.event('scenarios-pass')
    except Exception as error:
        session.event('failed', message=session.redact(str(error)))
        raise SystemExit(1)
    finally:
        session.stop()


if __name__ == '__main__':
    main()
