#!/usr/bin/env python3
"""AX211 VFIO acceptance using ordinary images; credentials arrive on stdin."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

REMOTE_CONTROL = r'''
import json,socket,sys,time
from pathlib import Path
r=json.load(sys.stdin);p=Path(r['directory'])
if r['operation'] != 'read':
 s=socket.socket(socket.AF_UNIX);s.connect(str(p/'monitor.sock'));s.settimeout(.02)
 if r['operation']=='quit':s.sendall(b'quit\n')
 else:
  keys={' ':'spc','/':'slash','-':'minus','.':'dot',':':'shift-semicolon','_':'shift-minus','=':'equal',';':'semicolon','$':'shift-4','?':'shift-slash'}
  for ch in r['text']+'\n':
   key='ret' if ch=='\n' else ch if ch.islower() or ch.isdigit() else 'shift-'+ch.lower() if ch.isupper() else keys[ch]
   s.sendall(('sendkey '+key+' 10\n').encode());time.sleep(.04)
   try:s.recv(65536)
   except socket.timeout:pass
 s.close()
f=p/'debugcon.log'
if f.exists():sys.stdout.write(f.read_text(errors='replace'))
'''


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


class Session:
    def __init__(self, args, credentials):
        self.args = args
        self.credentials = credentials
        self.raw = ''
        self.started = time.monotonic()
        self.sequence = 0
        self.events = []
        self.observed_offers = 0
        self.observed_bounds = 0
        self.process = None
        self.logfile = None
        self.remote = args.remote + '/guest'

    def redact(self, value):
        for secret in sorted(self.credentials.values(), key=len, reverse=True):
            value = value.replace(secret, '<redacted>')
            value = value.replace(secret.encode().hex(), '<redacted-hex>')
        value = re.sub(r'\b(?:ssid|bssid)=\S*', 'identity=<redacted>', value)
        return re.sub(r'(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b', '<mac>', value)

    def event(self, name, **values):
        row = dict(elapsed=round(time.monotonic()-self.started, 3), event=name, **values)
        self.events.append(row)
        print(json.dumps(row), flush=True)
        (self.args.work/'events.json').write_text(json.dumps(self.events, indent=2)+'\n')

    def control(self, operation='read', text=''):
        request = dict(directory=self.remote, operation=operation, text=text)
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', self.args.host,
            'sudo -n python3 -c '+shlex.quote(REMOTE_CONTROL)],
            input=json.dumps(request), text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(self.redact(result.stderr))
        self.raw = result.stdout.replace('\r', '')
        (self.args.work/'console-redacted.log').write_text(self.redact(self.raw))
        offers = len(re.findall(r'dhcpc: wlan0: offered [0-9.]+ by [0-9.]+', self.raw))
        bounds = len(re.findall(r'dhcpc: wlan0: bound [0-9.]+/[0-9]+ lease', self.raw))
        if offers > self.observed_offers:
            self.event('dhcp-offer-observed', total=offers)
        if bounds > self.observed_bounds:
            self.event('dhcp-bound-observed', total=bounds)
        self.observed_offers = offers
        self.observed_bounds = bounds
        return self.raw

    def wait(self, pattern, offset=0, timeout=120):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            self.control()
            found = re.search(pattern, self.raw[offset:], re.M)
            if found:
                return self.raw[offset:]
            if self.process.poll() is not None:
                raise RuntimeError('VFIO runner exited before expected guest output')
            time.sleep(.3)
        raise RuntimeError('guest timeout: '+self.redact(self.raw[offset:][-1800:]))

    def command(self, text, timeout=110):
        self.control()
        offset = len(self.raw)
        self.sequence += 1
        marker = 'P033_DONE_'+str(self.sequence)
        self.control('send', text+'; echo '+marker)
        output = self.wait('^'+marker+'$', offset, timeout)
        self.event('command-returned', command=self.redact(text))
        return output

    def connected(self, label):
        begin = time.monotonic()
        while time.monotonic()-begin < 180:
            output = self.command('wifi wlan0 status; ifconfig wlan0')
            if 'state=connected' in output and re.search(r'inet (?!0\.0\.0\.0)\d+\.', output):
                ping = self.command('ping -c 3 192.168.2.1', timeout=30)
                if '3 packets transmitted, 3 packets received' not in ping:
                    raise RuntimeError('LAN peer ping failed: '+self.redact(ping))
                self.event('connected-pass', scenario=label, wait_seconds=round(time.monotonic()-begin, 3))
                return
            time.sleep(5)
        raise RuntimeError('association/DHCP did not recover: '+label)

    def same_content(self, first, second):
        output = self.command('cksum '+first+' '+second)
        values = []
        for path in [first, second]:
            match = re.search(r'^(\d+)\s+(\d+)\s+'+re.escape(path)+r'$', output, re.M)
            if match is None:
                raise RuntimeError('missing checksum for '+path)
            values.append(match.groups())
        if values[0] != values[1]:
            raise RuntimeError('guest executable copy differs: '+first)
        self.event('guest-copy-verified', first=first, second=second)

    def start(self):
        a = self.args
        a.work.mkdir(parents=True, exist_ok=False)
        self.before = digest(a.image)
        subprocess.run(['ssh', a.host, 'mkdir -m 700 '+shlex.quote(a.remote)], check=True)
        runner = Path(__file__).resolve().parents[2]/'ws004/tests/run-intel-ax211-vfio-qemu.sh'
        subprocess.run(['scp', '-q', str(a.image), a.host+':'+a.remote+'/image.img'], check=True)
        subprocess.run(['scp', '-q', str(runner), a.host+':'+a.remote+'/runner.sh'], check=True)
        self.logfile = (a.work/'runner.log').open('w')
        command = 'sudo -n env AX211_VFIO_SAFE_ROUTE_DEVICE=enx6c1ff71a08b6 AX211_VFIO_RUN_TIMEOUT=1500 sh '
        if getattr(a, 'usb_wlan_port', None):
            command = command[:-3] + 'AX211_VFIO_USB_WLAN_PORT='+shlex.quote(a.usb_wlan_port)+' sh '
        command += ' '.join(shlex.quote(v) for v in [a.remote+'/runner.sh', a.remote+'/image.img', self.remote])
        self.process = subprocess.Popen(['ssh', a.host, command], stdout=self.logfile, stderr=subprocess.STDOUT)
        self.wait(r'login:\s*$', timeout=120)
        self.control('send', 'root')
        self.wait('Password:')
        self.control('send', '')
        self.wait(r'root@[^\n]*\$\s*$')
        self.event('login-pass', image_sha256=self.before)

    def stop(self):
        if self.process is not None:
            if self.process.poll() is None:
                try:
                    self.control('quit')
                except Exception as e:
                    self.event('quit-error', message=self.redact(str(e)))
                try:
                    self.process.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    raise RuntimeError('VFIO runner did not restore host promptly; inspect remote host')
            self.logfile.close()
            log = (self.args.work/'runner.log').read_text()
            if 'restored 0000:00:14.3 to iwlwifi' not in log:
                raise RuntimeError('host restoration was not verified')
            self.event('host-restored', runner_exit=self.process.returncode,
                       source_unchanged=self.before == digest(self.args.image))

    def run(self):
        c = self.credentials
        profile = 'net wifi set-key '+c['ssid']+' '+c['key']+' auto'
        if self.args.scenario == 'association-cancel':
            self.command('net wifi set-key '+c['ssid']+' incorrect-test-key auto')
            offset = len(self.raw)
            self.command('net wifi enable')
            for attempt in range(4):
                self.wait(r'intel-ax211: association hardware ready', offset, timeout=180)
                state = self.command('wifi wlan0 status')
                if re.search(r'state=(authenticating|associating|four-way)', state):
                    break
                self.event('association-ended-before-observation', attempt=attempt+1)
                offset = len(self.raw)
            else:
                raise RuntimeError('association was not in progress at cancellation')
            self.command('net wifi disable')
            self.event('association-cancelled')
            self.command(profile)
            self.command('net wifi enable')
            self.connected('cancel-association-enable')
            return
        if self.args.scenario == 'dhcp-recovery':
            self.command('cp /sbin/dhcpc /root/p033-dhcpc-original')
            self.same_content('/sbin/dhcpc', '/root/p033-dhcpc-original')
            self.command('cp /usr/bin/p033-dhcpc /sbin/dhcpc')
            self.same_content('/sbin/dhcpc', '/usr/bin/p033-dhcpc')
            self.command('touch /run/p033-drop-dhcp')
            self.command(profile)
            offset = len(self.raw)
            self.command('net wifi enable')
            self.wait(r'p033: DHCP reply suppressed', offset, timeout=180)
            self.wait(r'dhcpc: wlan0: offer: Connection timed out', offset, timeout=30)
            self.event('dhcp-no-response-timeout-pass')
            processes = self.command('ps -A')
            count = len(re.findall(r'^.*\bdhcpc\b.*$', processes, re.M))
            if count > 1:
                raise RuntimeError('multiple DHCP child processes remain')
            self.event('dhcp-child-count-pass', count=count)
            self.command('rm /run/p033-drop-dhcp')
            self.connected('dhcp-response-resumed')
            self.command('net wifi disable')
            self.command('cp /root/p033-dhcpc-original /sbin/dhcpc')
            self.same_content('/sbin/dhcpc', '/root/p033-dhcpc-original')
            self.command('net wifi enable')
            self.connected('ordinary-dhcpc-restored')
            return
        if self.args.scenario == 'key-first':
            self.command(profile)
            self.command('net wifi enable')
            self.connected('key-first')
            return
        self.command('net wifi enable')
        self.command(profile)
        self.connected('enable-first')
        self.command('net wifi enable')
        self.command('net wifi enable')
        self.connected('repeated-enable')
        self.command('net wifi disable')
        self.command('net wifi enable')
        self.connected('disable-enable')
        self.command('net wifi disable')
        self.command('net wifi enable')
        self.command('net wifi disable')
        self.command('net wifi enable')
        self.connected('cancel-scan-enable')
        self.command('net wifi disable')
        self.command('net wifi set-key '+c['ssid']+' incorrect-test-key auto')
        offset = len(self.raw)
        self.command('net wifi enable')
        self.wait(r'networkd: automatic Wi-Fi attempt:', offset, timeout=180)
        self.event('wrong-key-failure-observed')
        self.command(profile)
        self.connected('correct-key-recovery')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='awe@10.0.10.25')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--remote', required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--scenario', choices=['enable-first','key-first','dhcp-recovery','association-cancel'], required=True)
    args = parser.parse_args()
    credentials = json.load(sys.stdin)
    for key in ['ssid', 'key']:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', credentials[key]):
            raise RuntimeError('fixture currently requires simple shell-safe credentials')
    session = Session(args, credentials)
    try:
        session.start()
        session.run()
        if session.observed_offers == 0 or session.observed_bounds == 0:
            raise RuntimeError('console OFFER/bound events were not observed')
        session.event('scenarios-pass')
    except Exception as e:
        session.event('failed', message=session.redact(str(e)))
        raise SystemExit(1)
    finally:
        session.stop()


if __name__ == '__main__':
    main()
