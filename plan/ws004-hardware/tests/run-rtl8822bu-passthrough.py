#!/usr/bin/env python3
"""Run one bounded exact-device cell; credentials arrive only through stdin."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading
import time


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            value.update(block)
    return value.hexdigest()


def emit(event, **values):
    print(json.dumps({'event': event, **values}), flush=True)


class Guest:
    def __init__(self, directory, image, usb, secrets):
        self.directory = directory
        self.secrets = sorted(secrets, key=len, reverse=True)
        self.raw = ''
        self.deadline = time.monotonic() + 1200
        self.debug = None
        self.monitor = None
        self.proc = None
        self.stderr = (directory / 'qemu.log').open('w')
        self.source = image
        self.source_hash = digest(image)
        self.debug_path = directory / 'debug.sock'
        self.monitor_path = directory / 'monitor.sock'
        self.log = directory / 'console-redacted.log'
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', directory / 'vars.fd')
        subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(image),
                        str(directory / 'guest.img')], check=True)
        arguments = ['qemu-system-x86_64', '-machine', 'q35,accel=kvm',
                     '-cpu', 'host,+invtsc', '-m', '1024', '-smp', '4',
                     '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                     '-drive', f'if=pflash,format=raw,file={directory / "vars.fd"}',
                     '-device', 'qemu-xhci,id=bootxhci',
                     '-drive', f'if=none,id=boot,file={directory / "guest.img"},format=raw',
                     '-device', 'usb-storage,bus=bootxhci.0,drive=boot,bootindex=1',
                     '-device', 'qemu-xhci,id=wlanxhci',
                     '-device', f'usb-host,hostbus={usb[0]},hostaddr={usb[1]},bus=wlanxhci.0',
                     '-nic', 'none', '-display', 'none', '-serial', 'none',
                     '-chardev', f'socket,id=dbg,path={self.debug_path},server=on,wait=off',
                     '-device', 'isa-debugcon,iobase=0xe9,chardev=dbg',
                     '-monitor', f'unix:{self.monitor_path},server=on,wait=off',
                     '-no-reboot']
        (directory / 'qemu-arguments.json').write_text(json.dumps(arguments, indent=2))
        self.proc = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=self.stderr)
        try:
            self.debug = self.connect(self.debug_path)
            self.monitor = self.connect(self.monitor_path)
        except Exception:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
            if self.debug is not None:
                self.debug.close()
            self.stderr.close()
            raise
        self.debug.setblocking(False)
        self.monitor.setblocking(False)

    def connect(self, path):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            connection = socket.socket(socket.AF_UNIX)
            try:
                connection.connect(str(path))
                return connection
            except OSError:
                connection.close()
                time.sleep(.02)
        raise RuntimeError('QEMU socket did not become available')

    def redact(self, value):
        value = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', value)
        for secret in self.secrets:
            value = value.replace(secret, '<redacted>')
            value = value.replace(secret.encode().hex(), '<redacted-hex>')
        value = re.sub(r'(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b', '<redacted-mac>', value)
        value = re.sub(r'(?i)\bwlx[0-9a-f]{12}\b', '<redacted-interface>', value)
        value = re.sub(r'\b(?:ssid|bssid)=(?:"(?:[^"\\]|\\.)*"|\S+)',
                       'identity=<redacted>', value)
        return value

    def pump(self):
        if self.debug is not None:
            while True:
                try:
                    data = self.debug.recv(65536)
                except (BlockingIOError, OSError):
                    break
                if not data:
                    break
                self.raw += data.decode(errors='replace').replace('\r', '')
        # Store complete lines only, so fragmented secrets never escape redaction.
        complete = self.raw.rsplit('\n', 1)[0] if '\n' in self.raw else ''
        self.log.write_text(self.redact(complete) + '\n')
        if self.monitor is not None:
            while True:
                try:
                    if not self.monitor.recv(65536):
                        break
                except (BlockingIOError, OSError):
                    break

    def wait(self, pattern, start=0, timeout=120):
        end = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < end:
            self.pump()
            value = self.raw[start:]
            match = re.search(pattern, value, re.M)
            if match:
                return value, match
            if self.proc.poll() is not None:
                raise RuntimeError('QEMU exited before expected output')
            time.sleep(.05)
        raise RuntimeError('guest wait timed out: ' + self.redact(self.raw[start:][-2000:]))

    def send(self, value):
        keys = {' ': 'spc', '/': 'slash', '-': 'minus', '.': 'dot',
                ':': 'shift-semicolon', '_': 'shift-minus', '=': 'equal'}
        for char in value:
            if 'a' <= char <= 'z' or '0' <= char <= '9':
                key = char
            elif 'A' <= char <= 'Z':
                key = 'shift-' + char.lower()
            else:
                key = keys[char]
            self.monitor.sendall(f'sendkey {key} 10\n'.encode())
            self.pump()
            time.sleep(.025)
        self.monitor.sendall(b'sendkey ret 10\n')
        time.sleep(.05)

    def command(self, command, timeout=120, required=None, code=0):
        start = len(self.raw)
        self.send('wlan-probe ' + command)
        output, match = self.wait(r'^q080-exit (-?\d+)$', start, timeout)
        self.wait(r'root@[^\s]*:[^\n]*\$ ?$', start, 20)
        if int(match.group(1)) != code:
            raise RuntimeError('command failed: ' + self.redact(output))
        if required and not re.search(required, output, re.M):
            raise RuntimeError('required observation missing: ' + self.redact(output))
        emit('guest-command-pass', command=command)
        return output

    def scan_complete(self, after_generation=None):
        end = min(self.deadline, time.monotonic() + 90)
        while time.monotonic() < end:
            output = self.command('run /sbin/net wifi list')
            if after_generation is not None and scan_generation(output) <= after_generation:
                time.sleep(1)
                continue
            if re.search(r'interface=wlan0 scan state=2 .*error=0', output):
                return output
            if re.search(r'interface=wlan0 scan state=[34]', output):
                raise RuntimeError('scan failed: ' + self.redact(output))
            time.sleep(1)
        raise RuntimeError('scan did not complete within 90 seconds')

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.pump()
        self.log.write_text(self.redact(self.raw))
        for connection in (self.monitor, self.debug):
            if connection is not None:
                connection.close()
        self.stderr.close()


class Payload(http.server.BaseHTTPRequestHandler):
    data = b'zedBSD RTL8822BU dual-band payload\n' * 128

    def do_GET(self):
        if self.path != '/q080-payload':
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(self.data)))
        self.end_headers()
        self.wfile.write(self.data)

    def log_message(self, *args):
        pass


def scan_generation(output):
    match = re.search(r'^interface=wlan0 scan state=\d+ generation=(\d+)\b',
                      output, re.M)
    if match is None:
        raise RuntimeError('scan status has no generation')
    return int(match.group(1))


def verify_final_reopen(guest, result):
    before = scan_generation(guest.command('run /sbin/net wifi list'))
    guest.command('run /sbin/net wifi enable')
    after = scan_generation(guest.scan_complete(after_generation=before))
    guest.command('run /sbin/net wifi disable')
    guest.command('run /sbin/wifi wlan0 status', required=(
        r'^state=down scan=\S+ administrative=down authenticated=no '
        r'associated=no key=no authorized=no retries=\d+ error=0(?:\s|$)'))
    result['final_reopen'] = {'scan_complete': True, 'administrative_down': True,
                              'generation_before': before, 'generation_after': after}
    emit('final-reopen-pass', **result['final_reopen'])


def cleanup_error(result, stage, error):
    result['status'] = 'failed'
    # Exception messages can contain console data; retain only their types.
    result.setdefault('cleanup_errors', []).append(
        {'stage': stage, 'error': type(error).__name__})


def cleanup_cell(guest, server, observer, output, device, interface, image,
                 original_routes, result):
    try:
        if guest is not None:
            try:
                guest.stop()
            except Exception as error:
                cleanup_error(result, 'guest-stop', error)
        for stage, action in (('server-shutdown', server.shutdown),
                              ('server-close', server.server_close)):
            try:
                action()
            except Exception as error:
                cleanup_error(result, stage, error)
        if observer is not None:
            try:
                result['usb_descriptor_observation'] = observer.stop()
            except Exception as error:
                cleanup_error(result, 'observer-stop', error)
        time.sleep(.5)
        checks = (
            ('target_present', lambda: device.exists()
             and (device / 'idVendor').read_text().strip() == '2357'
             and (device / 'idProduct').read_text().strip() == '0138'),
            ('target_unbound', lambda: not (interface / 'driver').exists()),
            ('routes_unchanged', lambda: subprocess.check_output(
                ['ip', '-4', 'route', 'show', 'default']) == original_routes),
            ('source_unchanged', lambda: digest(image) == result['input_sha256']),
        )
        result['restored'] = {}
        for name, check in checks:
            try:
                result['restored'][name] = bool(check())
            except Exception as error:
                result['restored'][name] = False
                cleanup_error(result, name, error)
        if not all(result['restored'].values()):
            result['status'] = 'failed'
    finally:
        # The disposable image contains the provisioned credential store.
        # Diagnostics must not bypass its removal, but an unjoined QEMU may
        # still write the image. A failed constructor leaves ownership unknown
        # and has not reached credential provisioning, so retain that case too.
        try:
            temporary_image = output / 'guest.img'
            if temporary_image.exists():
                if guest is None or (guest.proc is not None
                                      and guest.proc.poll() is None):
                    cleanup_error(result, 'guest-image-process-not-joined',
                                  RuntimeError())
                else:
                    temporary_image.unlink()
        except Exception as error:
            cleanup_error(result, 'guest-image-remove', error)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('image', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--usb-path', default='4-4')
    parser.add_argument('--usbmon', action='store_true')
    parser.add_argument('--cycles', type=int, choices=(1, 2, 3), default=1)
    args = parser.parse_args()
    credentials = json.load(sys.stdin)
    secrets = [credentials['key']] + [band['ssid'] for band in credentials['bands']]
    assert [band['label'] for band in credentials['bands']] == ['2g', '5g']
    assert 8 <= len(credentials['key']) <= 63
    assert all(1 <= len(band['ssid']) <= 32 for band in credentials['bands'])
    assert all(re.fullmatch(r'[A-Za-z0-9 /_.:=\-]+', value) for value in secrets), 'unsupported fixture input character'
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    device = Path('/sys/bus/usb/devices') / args.usb_path
    interface = Path(str(device) + ':1.0')
    assert (device / 'idVendor').read_text().strip() == '2357'
    assert (device / 'idProduct').read_text().strip() == '0138'
    assert not (interface / 'driver').exists()
    usb = [(device / field).read_text().strip() for field in ('busnum', 'devnum')]
    original_routes = subprocess.check_output(['ip', '-4', 'route', 'show', 'default'])
    assert b'enx6c1ff71a08b6' in original_routes
    result = {'status': 'failed', 'usb_path': args.usb_path,
              'usb_speed': (device / 'speed').read_text().strip(),
              'input_sha256': digest(args.image),
              'cycles_requested': args.cycles, 'bands': []}
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Payload)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    guest = None
    observer = None
    try:
        if args.usbmon:
            from rtl8822bu_usbmon import DescriptorObserver
            observer = DescriptorObserver(*usb)
        guest = Guest(output, args.image, usb, secrets)
        emit('started', usb_speed=result['usb_speed'], image_sha256=result['input_sha256'])
        guest.wait(r'login:\s*$')
        start = len(guest.raw)
        guest.send('root')
        guest.wait('Password:', start)
        guest.send('')
        guest.wait(r'root@[^\s]*:[^\n]*\$ ?$', start)
        emit('logged-in')
        start = len(guest.raw)
        guest.send('wlan-probe setup')
        guest.wait('q080-credential-input', start)
        for value in [band['ssid'] for band in credentials['bands']] + [credentials['key']]:
            guest.send(value)
        _, setup = guest.wait(r'^q080-exit (-?\d+)$', start)
        assert setup.group(1) == '0', 'credential store setup failed'
        guest.wait(r'root@[^\s]*:[^\n]*\$ ?$', start)
        emit('credentials-provisioned')
        # Diagnostic repetitions share the guest deadline and stop on failure.
        for index, band in enumerate(credentials['bands'] * args.cycles):
            label = band['label']
            guest.command('run /sbin/net wifi enable')
            guest.scan_complete()
            guest.command('connect' + label)
            status = guest.command('run /sbin/wifi wlan0 status', required='authorized=yes')
            channel = re.search(r'channel=(\d+)', status)
            assert channel, 'authorized status has no channel'
            channel = int(channel.group(1))
            assert (1 <= channel <= 11) if label == '2g' else channel in (36, 40, 44, 48)
            configuration = guest.command('run /sbin/ifconfig wlan0', required=r'inet \d')
            guest.command('run /sbin/route show')
            guest.command('run /bin/ping -c 3 -W 2 10.0.10.25', required=r'3 (?:packets )?received')
            port = server.server_address[1]
            guest.command(f'run /bin/timeout 30 /bin/fetch -q -o /tmp/q080-payload http://10.0.10.25:{port}/q080-payload')
            guest.command('run /bin/wc -c /tmp/q080-payload', required=str(len(Payload.data)))
            checksum = subprocess.run(['cksum'], input=Payload.data, capture_output=True, check=True).stdout.decode().split()[0]
            guest.command('run /bin/cksum /tmp/q080-payload', required=rf'^{checksum}\s+{len(Payload.data)}\s+')
            guest.command('run /sbin/net wifi disconnect')
            guest.command('run /sbin/net wifi disable')
            guest.command('run /sbin/wifi wlan0 status', required='administrative=down')
            result['bands'].append({'cycle': index // 2 + 1, 'band': label, 'channel': channel, 'authorized': True,
                                    'dhcp': True, 'ping_received': 3, 'payload_bytes': len(Payload.data)})
            emit('band-pass', **result['bands'][-1])
        verify_final_reopen(guest, result)
        result['status'] = 'passed'
    except Exception as error:
        result['error'] = guest.redact(str(error)) if guest else type(error).__name__
        emit('failure', reason=result['error'])
        if guest and guest.proc.poll() is None:
            for command in ('run /sbin/wifi wlan0 status', 'run /sbin/ifconfig wlan0'):
                try:
                    guest.command(command, timeout=15)
                except Exception:
                    pass
    finally:
        cleanup_cell(guest, server, observer, output, device, interface,
                     args.image, original_routes, result)
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        emit('finished', status=result['status'], restored=result['restored'])
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    os.umask(0o077)
    sys.exit(main())
