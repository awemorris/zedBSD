#!/usr/bin/env python3
"""Capture installed QEMU UAS control descriptors, without claiming UAS I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
QMP = runpy.run_path(str(Path(__file__).with_name('run-imod-qemu.py')))['QMP']


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            result.update(block)
    return result.hexdigest()


def descriptors(path):
    raw = path.read_bytes()
    if len(raw) < 24 or struct.unpack_from('<I', raw)[0] != 0xa1b2c3d4:
        raise ValueError('expected little-endian microsecond pcap')
    if struct.unpack_from('<I', raw, 20)[0] != 220:
        raise ValueError('expected USB Linux mmapped link type')
    offset = 24
    pending = None
    replies = []
    while offset < len(raw):
        if len(raw) - offset < 16:
            raise ValueError('partial pcap header')
        size = struct.unpack_from('<I', raw, offset + 8)[0]
        offset += 16
        if size < 64 or size > len(raw) - offset:
            raise ValueError('invalid usbmon record size')
        packet = raw[offset:offset + size]
        offset += size
        if packet[9] != 2:
            continue
        if packet[8] == ord('S'):
            pending = struct.unpack_from('<BBHHH', packet, 40)
        elif packet[8] == ord('C') and pending is not None:
            setup = pending
            pending = None
            if setup[0] != 0x80 or setup[1] != 6:
                continue
            if struct.unpack_from('<i', packet, 28)[0] != 0:
                continue
            data = packet[64:]
            if len(data) < 2 or setup[2] >> 8 != data[1]:
                continue
            if data[1] == 1 and len(data) == 18:
                replies.append(data)
            elif data[1] == 2 and len(data) >= 9:
                if struct.unpack_from('<H', data, 2)[0] == len(data):
                    replies.append(data)
    return list(dict.fromkeys(replies))


def configuration(raw):
    result = {'raw_hex': raw.hex(), 'interfaces': []}
    offset = 0
    interface = None
    endpoint = None
    while offset < len(raw):
        if len(raw) - offset < 2:
            raise ValueError('partial descriptor header')
        size, kind = raw[offset:offset + 2]
        if size < 2 or size > len(raw) - offset:
            raise ValueError('invalid descriptor size')
        item = raw[offset:offset + size]
        if kind == 4:
            if size != 9:
                raise ValueError('invalid interface descriptor')
            interface = dict(number=item[2], alternate=item[3],
                             endpoint_count=item[4], class_code=item[5],
                             subclass=item[6], protocol=item[7], endpoints=[])
            result['interfaces'].append(interface)
            endpoint = None
        elif kind == 5:
            if size != 7 or interface is None:
                raise ValueError('invalid endpoint descriptor')
            endpoint = dict(address=item[2], attributes=item[3],
                            max_packet=struct.unpack_from('<H', item, 4)[0])
            interface['endpoints'].append(endpoint)
        elif kind == 0x30:
            if size != 6 or endpoint is None or 'companion' in endpoint:
                raise ValueError('invalid companion association')
            endpoint['companion'] = dict(max_burst=item[2], attributes=item[3],
                                         max_streams_exponent=item[3] & 31)
        elif kind == 0x24:
            if size != 4 or endpoint is None or 'pipe_id' in endpoint:
                raise ValueError('invalid UAS pipe association')
            endpoint['pipe_id'] = item[2]
            endpoint['pipe_reserved'] = item[3]
        offset += size
    return result


def run_cell(out, speed):
    cell = out / speed
    cell.mkdir()
    source = REPO / 'build/amd64/hdd-image.img'
    source_hash = digest(source)
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source),
                    str(cell / 'boot.img')], check=True)
    shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', cell / 'vars.fd')
    with (cell / 'uas.img').open('xb') as disk:
        disk.truncate(32 * 1024 * 1024)
    target_hash = digest(cell / 'uas.img')
    args = ['qemu-system-x86_64', '-machine', 'q35,usb=off', '-m', '512',
            '-smp', '4', '-display', 'none', '-serial', 'none', '-nic', 'none',
            '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
            '-drive', f'if=pflash,format=raw,file={cell}/vars.fd',
            '-device', 'qemu-xhci,id=xhci',
            '-drive', f'if=none,format=raw,id=boot,file={cell}/boot.img',
            '-device', 'usb-storage,bus=xhci.0,port=4,drive=boot,bootindex=1',
            '-drive', f'if=none,format=raw,id=uasdisk,file={cell}/uas.img',
            '-debugcon', f'file:{cell}/guest.log',
            '-qmp', f'unix:{cell}/qmp.sock,server=on,wait=off']
    bus = 'xhci.0'
    if speed == 'high':
        args += ['-device', 'ich9-usb-ehci1,id=ehci']
        bus = 'ehci.0'
    args += ['-device', f'usb-uas,id=uas,bus={bus},port=2,pcap={cell}/uas.pcap',
             '-device', 'scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=uasdisk']
    (cell / 'argv.json').write_text(json.dumps(args, indent=2) + '\n')
    record = dict(speed=speed, production_sha256=source_hash,
                  qemu_version=subprocess.check_output(
                      ['qemu-system-x86_64', '--version'], text=True))
    qmp = None
    with (cell / 'qemu.log').open('w') as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 150
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited during enumeration')
                guest = cell / 'guest.log'
                text = guest.read_text(errors='replace') if guest.exists() else ''
                if re.search(r'panic:|fatal:|invalid kernel allocation free', text, re.I):
                    raise RuntimeError('kernel failure during enumeration')
                if 'class 08/06/62' in text and 'login:' in text:
                    break
                time.sleep(.2)
            else:
                raise TimeoutError('zedBSD did not enumerate UAS and reach login')
            qmp = QMP(cell / 'qmp.sock')
            record['usb_topology'] = qmp.call('human-monitor-command',
                                             {'command-line': 'info usb'})
            qmp.call('quit')
            process.wait(timeout=10)
            captured = descriptors(cell / 'uas.pcap')
            configurations = [configuration(d) for d in captured if d[1] == 2]
            uas = [(c, i) for c in configurations for i in c['interfaces']
                   if (i['class_code'], i['subclass'], i['protocol']) == (8, 6, 0x62)]
            if not uas:
                raise RuntimeError('no complete UAS configuration reply captured')
            for _, interface in uas:
                endpoints = interface['endpoints']
                if len(endpoints) != 4 or sorted(e.get('pipe_id', 0) for e in endpoints) != [1, 2, 3, 4]:
                    raise RuntimeError('unexpected UAS endpoint/pipe set')
            for index, raw in enumerate(captured):
                (cell / f'descriptor-{index}.bin').write_bytes(raw)
            record['configurations'] = configurations
            record['device_descriptors'] = [d.hex() for d in captured if d[1] == 1]
            record['kernel_enumeration'] = [line for line in text.splitlines()
                                             if 'class 08/06/62' in line]
            record['result'] = 'PASS UAS enumeration and real descriptor capture; data path not implemented'
            print(speed, record['result'], flush=True)
        except BaseException as error:
            record['failure'] = str(error)
            raise
        finally:
            if qmp is not None:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            record['production_sha256_after'] = digest(source)
            record['target_unchanged'] = target_hash == digest(cell / 'uas.img')
            (cell / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    if record['production_sha256_after'] != source_hash or not record['target_unchanged']:
        raise RuntimeError('unexpected source/target modification during enumeration')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.relative_to(REPO / 'plan/ws025-io-memory-cache/temp')
    out.mkdir(parents=True, exist_ok=False)
    for speed in ['high', 'super']:
        run_cell(out, speed)


if __name__ == '__main__':
    main()
