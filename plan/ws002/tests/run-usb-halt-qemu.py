#!/usr/bin/env python3
"""USB-root halt probe with independent QMP CPU state, disposable media only."""
from pathlib import Path
import argparse
import json
import re
import runpy
import socket
import shutil
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
f = runpy.run_path(str(REPO / 'plan/ws019/tests/run-formatter-qemu.py'))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--dirty', action='store_true')
    parser.add_argument('--no-swap', action='store_true')
    parser.add_argument('--cpus', type=int, choices=[1, 4], default=4)
    parser.add_argument('--usb-topology', choices=['xhci', 'paired'], default='xhci')
    args = parser.parse_args()
    out = args.output.resolve()
    out.relative_to(REPO / 'plan/ws002/temp')
    out.mkdir(parents=True, exist_ok=False)
    metadata = f['prepare_boot'](out, False, False, REPO / 'build/arch-images/amd64-ws019-installer.ufs')
    if args.no_swap:
        start = f['boot_payload'](out / 'boot.img')
        image = f"{out / 'boot.img'}@@{start * 512}"
        config = out / 'no-swap.cfg'
        subprocess.run(['mcopy', '-i', image, '::/zedbsd.cfg', str(config)], check=True)
        config.write_text('\n'.join(line for line in config.read_text().splitlines() if not line.startswith('swap0=')) + '\n')
        subprocess.run(['mcopy', '-o', '-i', image, str(config), '::/zedbsd.cfg'], check=True)
        metadata['boot_sha256_before'] = f['digest'](out / 'boot.img')
    metadata['cpus'] = args.cpus
    metadata['usb_topology'] = args.usb_topology
    metadata['swap_enabled'] = not args.no_swap
    f['create_nvme'](out / 'gpt.img')
    qmp_path = out / 'qmp.sock'
    guest = f['FormatterGuest'](out, usb_boot=True, usb_topology=args.usb_topology,
        extra_args=['-smp', str(args.cpus), '-qmp', f'unix:{qmp_path},server=on,wait=off'])
    connection = None
    channel = None
    try:
        guest.login()
        guest.run('mount', 'tmpfs')
        if args.no_swap:
            assert 'swap: swap0 source=' not in guest.text()
        else:
            assert 'swap: active sources=1 total=16383' in guest.text()
        assert 'vfs: root=overlay' in guest.text()
        if args.dirty:
            guest.run('cp /sbin/mkfs /root/halt-marker')
            marker = guest.run('cksum -a sha256 /root/halt-marker', '[0-9a-f]{64}')
            metadata['marker_sha256'] = re.search(r'^([0-9a-f]{64})  /root/halt-marker$', marker, re.M).group(1)
            assert metadata['marker_sha256'] == f['digest'](REPO / 'build/amd64/bin/mkfs')
        connection = socket.socket(socket.AF_UNIX)
        connection.settimeout(10)
        connection.connect(str(qmp_path))
        channel = connection.makefile('rwb', buffering=0)
        metadata['qmp_greeting'] = json.loads(channel.readline())
        serial = 0
        def command(name, arguments=None):
            nonlocal serial
            serial += 1
            request = {'execute': name, 'id': serial}
            if arguments is not None:
                request['arguments'] = arguments
            channel.write((json.dumps(request) + '\n').encode())
            while True:
                reply = json.loads(channel.readline())
                if reply.get('id') == serial:
                    assert 'error' not in reply, reply
                    return reply['return']
        command('qmp_capabilities')
        at = len(guest.text())
        guest.send('halt')
        samples = []
        end = time.monotonic() + 75
        while time.monotonic() < end:
            registers = command('human-monitor-command', {'command-line': 'info registers -a'})
            # A scheduler idle HLT has interrupts enabled; terminal CLI/HLT does not.
            states = re.findall(r'RIP=([0-9a-f]+).*?RFL=([0-9a-f]+).*?HLT=([01])', registers, re.S)
            terminal = len(states) == args.cpus and all(int(h) == 1 and int(flags, 16) & 0x200 == 0 for _, flags, h in states)
            samples.append({'terminal_cli_hlt': terminal, 'registers': registers})
            if len(samples) >= 3 and all(s['terminal_cli_hlt'] for s in samples[-3:]):
                break
            time.sleep(1)
        metadata['cpu_samples'] = samples
        metadata['shutdown_log'] = guest.text()[at:]
        metadata['result'] = 'HALT completed' if samples[-1]['terminal_cli_hlt'] else 'HALT not established'
        metadata['usb_shutdown_errors'] = bool(re.search(r'driver shutdown failed|controller stop failed', metadata['shutdown_log']))
        assert metadata['result'] == 'HALT completed', metadata['shutdown_log']
        assert not re.search(r'panic:|fatal:|assert:|amd64 fault', guest.text(), re.I), guest.text()
        assert not re.search(r'BOT CBW error|driver shutdown failed|controller stop failed', metadata['shutdown_log']), metadata['shutdown_log']
    except BaseException as error:
        metadata['failure'] = str(error)
        raise
    finally:
        if channel is not None:
            channel.close()
        if connection is not None:
            connection.close()
        guest.stop()
        metadata['production_sha256_after'] = f['digest'](REPO / 'build/amd64/hdd-image.img')
        (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
    assert metadata['production_sha256'] == metadata['production_sha256_after']
    if args.dirty:
        restart = out / 'persistence'
        restart.mkdir()
        for name in ['boot.img', 'gpt.img']:
            f['copy_image'](out / name, restart / name)
        shutil.copyfile(out / 'vars.fd', restart / 'vars.fd')
        second = f['FormatterGuest'](restart, usb_boot=True,
            usb_topology=args.usb_topology, extra_args=['-smp', str(args.cpus)])
        try:
            second.login()
            second.run('cksum -a sha256 /root/halt-marker', metadata['marker_sha256'])
            at = len(second.text())
            second.send('reboot')
            second.login(at)
            second.run('cksum -a sha256 /root/halt-marker', metadata['marker_sha256'])
            assert not re.search(r'BOT CBW error|driver shutdown failed|controller stop failed|panic:|fatal:|assert:|amd64 fault', second.text(), re.I), second.text()
            metadata['persistence_and_reboot'] = 'PASS'
        except BaseException as error:
            metadata['persistence_and_reboot'] = 'FAIL'
            metadata['persistence_failure'] = str(error)
            raise
        finally:
            second.stop()
            (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(metadata['result'], 'USB errors:', metadata['usb_shutdown_errors'])

if __name__ == '__main__':
    main()
