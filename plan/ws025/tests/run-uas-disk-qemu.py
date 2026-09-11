#!/usr/bin/env python3
"""Boot disposable BOT media and exercise a separate high-speed UAS disk."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import struct
import time

REPO = Path(__file__).resolve().parents[3]
helper = runpy.run_path(str(Path(__file__).with_name('capture-uas-descriptors.py')))
QMP, digest = helper['QMP'], helper['digest']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--mounted-exchange', action='store_true')
    parser.add_argument('--dirty-exchange', action='store_true')
    parser.add_argument('--partitioned', action='store_true')
    parser.add_argument('--initially-empty', action='store_true')
    parser.add_argument('--media-exchange', action='store_true')
    parser.add_argument('--removable', action='store_true')
    parser.add_argument('--speed', choices=['high', 'super'], default='high')
    parser.add_argument('--inflight-disconnect', action='store_true')
    parser.add_argument('--write-timeout', action='store_true')
    parser.add_argument('--write-error', action='store_true')
    parser.add_argument('--held-reference', action='store_true')
    parser.add_argument('--filesystem', action='store_true')
    parser.add_argument('--lifecycle', action='store_true')
    parser.add_argument('--timeout-recovery', action='store_true')
    options = parser.parse_args()
    if sum((options.lifecycle, options.timeout_recovery, options.filesystem, options.inflight_disconnect, options.held_reference, options.write_error, options.write_timeout, options.media_exchange, options.initially_empty, options.mounted_exchange, options.dirty_exchange)) > 1:
        parser.error('choose lifecycle, timeout-recovery or filesystem')
    if options.dirty_exchange:
        options.mounted_exchange = True
    if options.mounted_exchange:
        options.filesystem = True
        options.removable = True
    if options.partitioned and not options.media_exchange:
        parser.error('--partitioned requires --media-exchange')
    if options.media_exchange or options.initially_empty:
        options.removable = True
    if options.write_timeout:
        options.write_error = True
    out = options.output.resolve()
    out.relative_to(REPO / 'plan/ws025/temp')
    out.mkdir(parents=True, exist_ok=False)
    source = REPO / 'build/amd64/hdd-image.img'
    record = {'source_sha256': digest(source), 'speed': options.speed, 'removable': options.removable, 'initially_empty': options.initially_empty}
    speed_label = options.speed + '-speed'
    uas_bus = 'xhci.0' if options.speed == 'super' else 'ehci.0'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(out / 'boot.img')], check=True)
    if options.held_reference or options.write_error or options.dirty_exchange:
        sysroot = REPO / 'build/amd64/sysroot/usr'
        obj, binary = out / 'held.o', out / 'held'
        subprocess.run([str(REPO / 'build/llvm/bin/clang'), '--target=x86_64-unknown-zedbsd',
                        '-nostdinc', '-isystem', str(sysroot / 'include'), '-I' + str(REPO / 'include/uapi'),
                        '-DKERN_USER_ABI_LP64=1', '-ffreestanding', '-fno-pie', '-O1',
                        '-ffunction-sections', '-fdata-sections', *(['-DUAS_WRITE_TIMEOUT'] if options.write_timeout else []), '-Wall', '-Wextra', '-Werror',
                        '-c', str(Path(__file__).with_name('uas-dirty-guest.c' if options.dirty_exchange else 'uas-write-error-guest.c' if options.write_error else 'uas-held-guest.c')), '-o', str(obj)], check=True)
        subprocess.run(['ld', '-m', 'elf_x86_64', '--gc-sections', '-nostdlib', '-static',
                        '-z', 'max-page-size=4096', '-z', 'stack-size=0x100000',
                        '-T', str(REPO / 'platform/amd64/user.ld'), str(sysroot / 'lib/crt0.o'),
                        str(sysroot / 'lib/libc.o'), str(obj), '-o', str(binary)], check=True)
        with source.open('rb') as stream:
            stream.seek(512)
            header = stream.read(512)
            assert header[:8] == b'EFI PART'
            lba, slots, width = struct.unpack_from('<QII', header, 72)
            assert 0 < slots <= 4096 and 128 <= width <= 4096
            stream.seek(lba * 512)
            entries = stream.read(slots * width)
        candidates = []
        for i in range(slots):
            entry = entries[i*width:(i+1)*width]
            if entry[:16] == bytes(16): continue
            first = struct.unpack_from('<Q', entry, 32)[0]
            if subprocess.run(['mdir', '-i', f'{source}@@{first*512}', '::/rootfs.img'], capture_output=True).returncode == 0:
                candidates.append((i+1, first))
        assert len(candidates) == 1
        record['helper_partition'] = candidates[0][0]
        subprocess.run(['mcopy', '-i', f'{out / "boot.img"}@@{candidates[0][1]*512}', str(binary), '::/uasheld'], check=True)
    shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', out / 'vars.fd')
    target = out / 'uas.img'
    target.write_bytes(b'\xa5' * (32 * 1024 * 1024))
    args = ['qemu-system-x86_64', '-machine', 'q35,usb=off', '-m', '512', '-smp', '4',
            '-display', 'none', '-serial', 'none', '-nic', 'none',
            '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
            '-drive', f'if=pflash,format=raw,file={out}/vars.fd',
            '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,port=3',
            '-drive', f'if=none,format=raw,id=boot,file={out}/boot.img',
            '-device', 'usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1',
            '-device', 'ich9-usb-ehci1,id=ehci',
            '-drive', f'if=none,format=raw,id=uasdisk,file={target}',
            '-device', f'usb-uas,id=uas,bus={uas_bus},port=2,pcap={out}/uas.pcap',
            '-device', 'scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=uasdisk' + (',removable=true' if options.removable else ''),
            '-debugcon', f'file:{out}/guest.log', '-qmp', f'unix:{out}/qmp.sock,server=on,wait=off']
    if options.initially_empty:
        index = args.index(f'if=none,format=raw,id=uasdisk,file={target}')
        args[index] = 'if=none,id=uasdisk'
    if options.speed == 'super':
        index = args.index('ich9-usb-ehci1,id=ehci') - 1
        del args[index:index + 2]
    if options.write_error:
        config = out / 'blkdebug.conf'
        config.write_text('[inject-error]\nevent = "write_aio"\niotype = "write"\nerrno = "5"\nsector = "16384"\nonce = "on"\nimmediately = "off"\n')
        if options.write_timeout:
            config.write_text(config.read_text() + 'delay-ns = "7000000000"\n')
        index = args.index(f'if=none,format=raw,id=uasdisk,file={target}') - 1
        args[index:index+2] = ['-blockdev', json.dumps({'driver': 'raw', 'node-name': 'uasdisk',
            'file': {'driver': 'blkdebug', 'config': str(config), 'image': {'driver': 'file', 'filename': str(target)}}})]
    if options.timeout_recovery or options.inflight_disconnect:
        backing_arg = f'if=none,format=raw,id=uasdisk,file={target}'
        index = args.index(backing_arg) - 1
        args[index:index + 2] = [
            '-object', 'throttle-group,id=uaslimit',
            '-blockdev', json.dumps({'driver': 'throttle', 'node-name': 'uasdisk',
                                    'throttle-group': 'uaslimit',
                                    'file': {'driver': 'raw', 'file': {'driver': 'file', 'filename': str(target)}}})]
    if options.timeout_recovery and (options.speed == 'super' or options.removable):
        args += ['-trace', f'enable=usb_uas_reset,file={out}/reset.trace']
    if options.inflight_disconnect:
        args += ['-trace', f'enable=usb_uas_*,file={out}/inflight.trace']
    if options.write_timeout:
        args += ['-trace', f'enable=usb_uas_*,file={out}/write-timeout.trace']
    (out / 'argv.json').write_text(json.dumps(args, indent=2))
    qmp = None
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        def wait(pattern, start=0, seconds=150):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited')
                text = (out / 'guest.log').read_text(errors='replace') if (out / 'guest.log').exists() else ''
                if re.search(r'panic:|fatal:|invalid kernel allocation free|VFS initialization failed', text, re.I):
                    raise RuntimeError('kernel failure')
                match = re.search(pattern, text[start:])
                if match:
                    return match, text
                time.sleep(.2)
            raise TimeoutError(pattern)
        def type_line(line):
            special = {' ': 'spc', '/': 'slash', '=': 'equal', '-': 'minus', '.': 'dot', '\n': 'ret', ':': 'shift-semicolon', '&': 'shift-7'}
            for char in line + '\n':
                qmp.call('human-monitor-command', {'command-line': 'sendkey ' + ('shift-' + char.lower() if char.isupper() else special.get(char, char))})
                time.sleep(.06)
        try:
            _, text = wait(r'login:')
            qmp = QMP(out / 'qmp.sock')
            if options.initially_empty:
                if 'usb-uas:' in text or text.count('driver=usb-uas') != 1:
                    raise RuntimeError('empty LUN binding/publication contract failed')
                record['empty_at_login'] = True
                start = len(text)
                qmp.call('blockdev-change-medium', {'device': 'uasdisk',
                    'filename': str(target), 'format': 'raw'})
                _, text = wait(r'usb-uas: sd[a-z]+ blocks=65536 block-size=512', start, 60)
            match = re.search(r'usb-uas: (sd[a-z]+) blocks=65536 block-size=512 policy=(\d+) ' + speed_label, text)
            if match is None:
                # USB enumeration may finish after init presents the login prompt.
                match, text = wait(r'usb-uas: (sd[a-z]+) blocks=65536 block-size=512 policy=(\d+) ' + speed_label, 0, 45)
            record['disk'] = match[1]
            record['policy'] = int(match[2])
            if record['policy'] == 0:
                raise RuntimeError('UAS disk has no persistence policy')
            type_line('root')
            wait(r'Password:')
            type_line('')
            _, text = wait(r'root[^\n]*[$#]')
            commands = [f'dd if=/dev/zero of=/dev/{record["disk"]} bs=512 seek=8 count=4',
                        'sync',
                        f'dd if=/dev/{record["disk"]} of=/root/uas-read bs=512 skip=8 count=4',
                        'cksum -a sha256 /root/uas-read']
            for command in commands:
                start = len(text)
                type_line(command)
                _, text = wait(r'root[^\n]*[$#]', start, 90)
            expected = hashlib.sha256(bytes(2048)).hexdigest()
            if expected not in text:
                raise RuntimeError('UAS readback checksum missing')
            record['readback_sha256'] = expected
            if options.media_exchange:
                replacement = out / 'replacement.img'
                replacement_bytes = bytearray(b'\x5a' * (32 * 1024 * 1024))
                if options.partitioned:
                    replacement_bytes[:512] = bytes(512)
                    replacement_bytes[446:462] = struct.pack('<BBBBBBBBII',
                        0, 0, 1, 0, 0x83, 254, 255, 255, 2048, 32768)
                    replacement_bytes[510:512] = b'\x55\xaa'
                replacement.write_bytes(replacement_bytes)
                start = len(text)
                qmp.call('eject', {'device': 'uasdisk', 'force': True})
                # Let the readiness monitor observe absence before reinsertion.
                time.sleep(3)
                qmp.call('blockdev-change-medium', {'device': 'uasdisk',
                    'filename': str(replacement), 'format': 'raw'})
                match, text = wait(r'usb-uas: (sd[a-z]+) blocks=65536 block-size=512', start, 60)
                record['replacement_disk'] = match.group(1)
                read_disk = match.group(1) + ('1' if options.partitioned else '')
                if options.partitioned:
                    time.sleep(3)
                    record['replacement_partition'] = read_disk
                for command in [f'dd if=/dev/{read_disk} of=/root/uas-new bs=512 skip=8 count=4',
                                'cksum -a sha256 /root/uas-new']:
                    start = len(text)
                    type_line(command)
                    _, text = wait(r'root[^\n]*[$#]', start, 45)
                replacement_hash = hashlib.sha256(b'\x5a' * 2048).hexdigest()
                if replacement_hash not in text[start:]:
                    raise RuntimeError('in-place replacement readback mismatch')
                if replacement.read_bytes() != replacement_bytes:
                    raise RuntimeError('replacement backing was modified')
                record['replacement_sha256'] = replacement_hash
            if options.write_error:
                for command in ['mkdir -p /run/heldboot',
                                f'mount -t fat -r sda{record["helper_partition"]} /run/heldboot',
                                'cp /run/heldboot/uasheld /run/uasheld',
                                'chmod 700 /run/uasheld', 'umount /run/heldboot']:
                    start = len(text)
                    type_line(command)
                    _, text = wait(r'root[^\n]*[$#]', start, 45)
                start = len(text)
                type_line(f'/run/uasheld /dev/{record["disk"]}')
                _, text = wait(r'root[^\n]*[$#]', start, 60)
                record['write_error_output'] = text[start:]
                if options.write_timeout and 'UASWRITE TIMEOUT' not in text[start:]:
                    raise RuntimeError('write did not time out')
                if 'UASWRITE STICKY PASS' not in text[start:]:
                    raise RuntimeError('write failure or sticky-error contract failed')
            if options.timeout_recovery:
                recovery_start = len(text)
                if options.speed == 'super' or options.removable:
                    record['reset_count_before_failure'] = (out / 'reset.trace').read_text().count('usb_uas_reset')
                qmp.call('qom-set', {'path': '/objects/uaslimit', 'property': 'limits', 'value': {'bps-read': 256}})
                # A cold disk BIO is 4096 bytes: 256 B/s delays the next read about
                # 16 s. QOM limit changes do not rearm an already waiting timer.
                record['throttle_limits'] = qmp.call('qom-get', {'path': '/objects/uaslimit', 'property': 'limits'})
                for block in (4096, 8192):
                    start = len(text)
                    type_line(f'dd if=/dev/{record["disk"]} of=/root/uas-timeout bs=512 skip={block} count=1')
                    read_started = time.monotonic()
                    _, text = wait(r'root[^\n]*[$#]', start, 45)
                    record[f'read_{block}_wait_seconds'] = time.monotonic() - read_started
                    (out / f'throttle-read-{block}.txt').write_text(text[start:])
                record['failed_read'] = text[start:]
                expected_error = ('Connection timed out' in record['failed_read'] or
                                  (options.removable and 'Unknown error' in record['failed_read'] and
                                   record['read_8192_wait_seconds'] >= 4.5))
                if not expected_error or '0 bytes transferred' not in record['failed_read']:
                    raise RuntimeError('throttled read did not report failure')
                qmp.call('qom-set', {'path': '/objects/uaslimit', 'property': 'limits', 'value': {'bps-read': 0}})
                if options.removable:
                    match, text = wait(r'usb-uas: (sd[a-z]+) blocks=65536 block-size=512', recovery_start, 60)
                    record['disk'] = match.group(1)
                    record['recovery_new_publication'] = True
                start = len(text)
                type_line(f'dd if=/dev/{record["disk"]} of=/root/uas-recovered bs=512 skip=8192 count=1')
                _, text = wait(r'root[^\n]*[$#]', start, 45)
                record['recovered_read'] = text[start:]
                start = len(text)
                type_line('cksum -a sha256 /root/uas-recovered')
                _, text = wait(r'root[^\n]*[$#]', start, 45)
                recovered_hash = hashlib.sha256(b'\xa5' * 512).hexdigest()
                if recovered_hash not in text[start:]:
                    raise RuntimeError('new read after timeout did not recover')
                record['recovery_readback_sha256'] = recovered_hash
            if options.filesystem:
                def run_command(command):
                    nonlocal text
                    start = len(text)
                    type_line(command)
                    _, text = wait(r'root[^\n]*[$#]', start, 90)
                    return text[start:]
                start = len(text)
                type_line(f'mkfs -t ufs --profile=native /dev/{record["disk"]}')
                confirmation, text = wait(r'FORMAT ' + record['disk'] + r':[0-9]+', start, 45)
                type_line(confirmation[0])
                _, text = wait(r'root[^\n]*[$#]', len(text), 90)
                run_command('mkdir -p /run/uas')
                run_command(f'mount -t ufs {record["disk"]} /run/uas')
                mounts = run_command('mount')
                if not re.search(re.escape(record['disk']) + r' on /run/uas type ufs', mounts):
                    raise RuntimeError('UAS UFS mount not established')
                run_command('dd if=/dev/zero of=/run/uas/payload bs=512 count=128')
                flushed = run_command('sync /run/uas/payload && echo uasfilesynced')
                if not re.search(r'\nuasfilesynced\r?\n', flushed):
                    raise RuntimeError('explicit file fsync failed')
                record['file_fsync'] = 'PASS sync FILE invokes fsync and exits zero'
                run_command('umount /run/uas')
                mounts = run_command('mount')
                if ' on /run/uas ' in mounts:
                    raise RuntimeError('UAS UFS unmount failed')
                run_command(f'mount -t ufs {record["disk"]} /run/uas')
                mounts = run_command('mount')
                if not re.search(re.escape(record['disk']) + r' on /run/uas type ufs', mounts):
                    raise RuntimeError('UAS UFS remount failed')
                filesystem_hash = hashlib.sha256(bytes(65536)).hexdigest()
                if filesystem_hash not in run_command('cksum -a sha256 /run/uas/payload'):
                    raise RuntimeError('UAS UFS persisted file hash mismatch')
                record['filesystem_sha256'] = filesystem_hash
                if options.mounted_exchange:
                    live_force = run_command('umount -f /run/uas && echo uasliveforce')
                    if re.search(r'\nuasliveforce\r?\n', live_force):
                        raise RuntimeError('force accepted a live medium')
                    if ' on /run/uas ' not in run_command('mount'):
                        raise RuntimeError('live force refusal lost attachment')
                    run_command('cd /run/uas')
                    replacement = out / 'replacement.img'
                    replacement.write_bytes(b'\x5a' * (32 * 1024 * 1024))
                    if options.dirty_exchange:
                        run_command('cd /root')
                        for command in ['mkdir -p /run/heldboot',
                                        f'mount -t fat -r sda{record["helper_partition"]} /run/heldboot',
                                        'cp /run/heldboot/uasheld /run/uasheld',
                                        'chmod 700 /run/uasheld', 'umount /run/heldboot',
                                        'sysctl vfs.writeback.control=/run/uas:on']:
                            run_command(command)
                        helper_start = len(text)
                        type_line('/run/uasheld')
                        ready, text = wait(r'UASDIRTY READY ([0-9]+)', helper_start, 30)
                        record['dirty_before_loss'] = int(ready.group(1))
                    exchange_start = len(text)
                    qmp.call('eject', {'device': 'uasdisk', 'force': True})
                    time.sleep(3)
                    qmp.call('blockdev-change-medium', {'device': 'uasdisk',
                        'filename': str(replacement), 'format': 'raw'})
                    time.sleep(3)
                    if options.dirty_exchange:
                        type_line('')
                        _, text = wait(r'root[^\n]*[$#]', exchange_start, 30)
                        if 'UASDIRTY BUSYREFUSED' not in text[helper_start:]:
                            raise RuntimeError('held dirty FD did not refuse force: ' + text[helper_start:])
                        closed = re.search(r'UASDIRTY CLOSED ([0-9]+)', text[helper_start:])
                        if closed is None or int(closed.group(1)) < 4096:
                            raise RuntimeError('dirty helper did not retain dirty credits after close: ' + text[helper_start:])
                        record['dirty_after_close'] = int(closed.group(1))
                        run_command('cd /run/uas')
                    stale = run_command('cksum -a sha256 /run/uas/payload && echo uasstalesuccess')
                    record['old_mount_read'] = stale
                    if filesystem_hash in stale or re.search(r'\nuasstalesuccess\r?\n', stale):
                        raise RuntimeError('old mounted file still readable after medium exchange')
                    if 'usb-uas:' in text[exchange_start:]:
                        raise RuntimeError('new disk published while old mount retained')
                    ordinary = run_command('umount /run/uas && echo uasordinarysuccess')
                    if re.search(r'\nuasordinarysuccess\r?\n', ordinary):
                        raise RuntimeError('ordinary unmount falsely succeeded on revoked medium')
                    if ' on /run/uas ' not in run_command('mount'):
                        raise RuntimeError('ordinary refusal lost the old attachment')
                    busy_force = run_command('umount -f /run/uas && echo uasbusyforce')
                    record['busy_force'] = busy_force
                    if re.search(r'\nuasbusyforce\r?\n', busy_force):
                        raise RuntimeError('force ignored a retained cwd')
                    if ' on /run/uas ' not in run_command('mount'):
                        raise RuntimeError('busy force refusal lost attachment')
                    run_command('cd /root')
                    forced = run_command('umount -f /run/uas && echo uasforcesuccess')
                    record['forced_unmount'] = forced
                    if options.dirty_exchange:
                        discarded = re.search(r'VM dirty bytes=([0-9]+)', forced)
                        if discarded is None or int(discarded.group(1)) < 4096:
                            raise RuntimeError('no retained VM dirty disposal evidence: ' + forced)
                        record['dirty_discarded'] = int(discarded.group(1))
                    if not re.search(r'\nuasforcesuccess\r?\n', forced):
                        raise RuntimeError('explicit revoked UFS unmount failed: ' + forced)
                    if ' on /run/uas ' in run_command('mount'):
                        raise RuntimeError('revoked UFS mount did not retire')
                    if options.dirty_exchange:
                        stats = run_command('sysctl vfs.writeback.stats')
                        record['writeback_after_discard'] = stats
                        if not re.search(r'mounts=0 workers=0 busy=0 dirty=0 reserved=0 tickets=0', stats):
                            raise RuntimeError('writeback ownership not fully retired: ' + stats)
                    match, text = wait(r'usb-uas: (sd[a-z]+) blocks=65536 block-size=512', exchange_start, 60)
                    raw_read = run_command(f'dd if=/dev/{match.group(1)} of=/root/uas-new bs=512 count=4')
                    replacement_hash = hashlib.sha256(b'\x5a' * 2048).hexdigest()
                    if replacement_hash not in run_command('cksum -a sha256 /root/uas-new'):
                        raise RuntimeError('post-unmount replacement read failed: ' + raw_read)
                    if replacement.read_bytes() != b'\x5a' * (32 * 1024 * 1024):
                        raise RuntimeError('old mounted filesystem modified replacement')
                    record['mounted_exchange'] = 'PASS old cache rejected, ordinary refusal, explicit force, publication and new readback'
                    if options.dirty_exchange:
                        record['mounted_exchange'] += '; held FD refused, dirty bytes discarded and writeback accounting cleared'
                else:
                    run_command('umount /run/uas')
            if options.lifecycle or options.inflight_disconnect or options.held_reference:
                binding = re.search(r'usb(\d+): device (\d+) interface 0 class 08/06/62 driver=usb-uas', text)
                if binding is None:
                    # Console writers can interleave the class-bind line. This
                    # topology has one UAS function with QEMU's fixed product ID.
                    binding = re.search(r'usb(\d+): device (\d+) port 2 46f4:0003 class 00 configuration=1 configured', text)
                if binding is None:
                    raise RuntimeError('missing UAS binding identity')
                if options.held_reference:
                    for command in ['mkdir -p /run/heldboot',
                                    f'mount -t fat -r sda{record["helper_partition"]} /run/heldboot',
                                    'cp /run/heldboot/uasheld /run/uasheld',
                                    'chmod 700 /run/uasheld', 'umount /run/heldboot']:
                        start = len(text)
                        type_line(command)
                        _, text = wait(r'root[^\n]*[$#]', start, 45)
                    start = len(text)
                    type_line(f'/run/uasheld /dev/{record["disk"]}')
                    _, text = wait(r'UASHELD READY', start, 45)
                    held_start = len(text)
                    qmp.call('qom-set', {'path': '/machine/peripheral/uas', 'property': 'attached', 'value': False})
                    _, text = wait(r'driver detach pending', held_start, 45)
                    qmp.call('device_del', {'id': 'uas'})
                    replacement = out / 'replacement.img'
                    replacement.write_bytes(b'\x5a' * (32 * 1024 * 1024))
                    qmp.call('blockdev-add', {'driver': 'raw', 'node-name': 'replacement',
                                           'file': {'driver': 'file', 'filename': str(replacement)}})
                    qmp.call('device_add', {'driver': 'usb-uas', 'id': 'uasreplacement', 'bus': uas_bus, 'port': '2'})
                    qmp.call('device_add', {'driver': 'scsi-hd', 'id': 'replacementlun', 'bus': 'uasreplacement.0',
                                           'scsi-id': 0, 'lun': 0, 'drive': 'replacement'})
                    qmp.call('qom-set', {'path': '/machine/peripheral/uasreplacement', 'property': 'attached', 'value': True})
                    type_line('')
                    _, text = wait(r'UASHELD OLDREJECTED', held_start, 45)
                    record['old_descriptor'] = 'PASS cached read, write and fsync rejected while held'
                    time.sleep(2)
                    held_text = (out / 'guest.log').read_text(errors='replace')[held_start:]
                    notifications = re.findall(r'usb' + binding[1] + r': device ' + binding[2] + r' port 2 driver detach pending \(17\)', held_text)
                    if len(notifications) != 1:
                        raise RuntimeError('unchanged detach error was not reported exactly once')
                    record['pending_notifications'] = len(notifications)

                    start = len(text)
                    type_line('')
                    _, text = wait(r'UASHELD CLOSED', start, 45)
                    expected = hashlib.sha256(b'\x5a' * 2048).hexdigest()
                if options.inflight_disconnect:
                    qmp.call('qom-set', {'path': '/objects/uaslimit', 'property': 'limits', 'value': {'bps-read': 256}})
                    start = len(text)
                    type_line(f'dd if=/dev/{record["disk"]} of=/root/uas-prime bs=512 skip=4096 count=1')
                    _, text = wait(r'root[^\n]*[$#]', start, 45)
                    trace_offset = len((out / 'inflight.trace').read_text())
                    read_start = len(text)
                    type_line(f'dd if=/dev/{record["disk"]} of=/root/uas-detached bs=512 skip=8192 count=1')
                    deadline = time.monotonic() + 3
                    while True:
                        trace = (out / 'inflight.trace').read_text()[trace_offset:]
                        admitted = re.search(r'usb_uas_command dev (\d+), tag (0x[0-9a-f]+)', trace)
                        if admitted:
                            break
                        if time.monotonic() >= deadline:
                            raise RuntimeError('no pending command before removal')
                        time.sleep(.05)
                    if re.search(r'usb_uas_scsi_complete dev ' + admitted[1] + ', tag ' + admitted[2] + r',', trace):
                        raise RuntimeError('read completed before removal')
                    current = (out / 'guest.log').read_text(errors='replace')
                    if re.search(r'root[^\n]*[$#]', current[read_start:]):
                        raise RuntimeError('guest read returned before removal')
                    (out / 'pending-before-removal.txt').write_text(trace)
                    record['pending_tag'] = admitted[2]
                    qmp.socket.settimeout(45)
                if not options.held_reference:
                    start = len(text)
                    if options.inflight_disconnect:
                        qmp.call('qom-set', {'path': '/machine/peripheral/uas', 'property': 'attached', 'value': False})
                    else:
                        qmp.call('device_del', {'id': 'uas'})
                    if options.inflight_disconnect:
                        qmp.socket.settimeout(5)
                        _, text = wait(r'root[^\n]*[$#]', read_start, 45)
                        record['detached_read'] = text[read_start:]
                        if '0 bytes transferred' not in record['detached_read']:
                            raise RuntimeError('removed read did not fail with zero bytes')
                    _, text = wait(r'usb' + binding[1] + r': device ' + binding[2] + r' port 2 disconnected', start, 60)
                    start = len(text)
                    if options.inflight_disconnect:
                        # Reconnect the same physical emulated device. Deleting its
                        # SCSI child can retain an unnamed backend after cancelled I/O.
                        qmp.call('qom-set', {'path': '/objects/uaslimit', 'property': 'limits', 'value': {'bps-read': 0}})
                        qmp.call('qom-set', {'path': '/machine/peripheral/uas', 'property': 'attached', 'value': True})
                    else:
                        # Legacy -drive backend is auto-deleted with its SCSI child.
                        qmp.call('blockdev-add', {'driver': 'raw', 'node-name': 'uasdisk2',
                                                  'file': {'driver': 'file', 'filename': str(target)}})
                        qmp.call('device_add', {'driver': 'usb-uas', 'id': 'uas2', 'bus': uas_bus, 'port': '2'})
                        qmp.call('device_add', {'driver': 'scsi-hd', 'id': 'uaslun2', 'bus': 'uas2.0',
                                               'scsi-id': 0, 'lun': 0, 'drive': 'uasdisk2'})
                        qmp.call('qom-set', {'path': '/machine/peripheral/uas2', 'property': 'attached', 'value': True})
                published, text = wait(r'usb-uas: (sd[a-z]+) blocks=65536 block-size=512 policy=1 ' + speed_label, start, 60)
                record['replug_disk'] = published[1]
                for command in [f'dd if=/dev/{published[1]} of=/root/uas-replug bs=512 skip=8 count=4',
                                'cksum -a sha256 /root/uas-replug']:
                    start = len(text)
                    type_line(command)
                    _, text = wait(r'root[^\n]*[$#]', start, 90)
                if expected not in text[start:]:
                    raise RuntimeError('replug readback hash mismatch')
                record['replug_readback_sha256'] = expected
                type_line('halt')
                samples = []
                deadline = time.monotonic() + 75
                while time.monotonic() < deadline:
                    registers = qmp.call('human-monitor-command', {'command-line': 'info registers -a'})
                    states = re.findall(r'RIP=([0-9a-f]+).*?RFL=([0-9a-f]+).*?HLT=([01])', registers, re.S)
                    terminal = len(states) == 4 and all(h == '1' and int(flags, 16) & 0x200 == 0 for _, flags, h in states)
                    samples.append({'terminal': terminal, 'registers': registers})
                    if len(samples) >= 3 and all(s['terminal'] for s in samples[-3:]):
                        break
                    time.sleep(1)
                (out / 'halt-samples.json').write_text(json.dumps(samples, indent=2) + '\n')
                text = (out / 'guest.log').read_text(errors='replace')
                if len(samples) < 3 or not all(s['terminal'] for s in samples[-3:]):
                    raise RuntimeError('four-CPU halt not established')
                if re.search(r'panic:|fatal trap|assertion failed|driver shutdown failed|controller stop failed', text, re.I):
                    raise RuntimeError('guest shutdown failure')
                record['halt'] = 'PASS four CPUs HLT with IF clear, three samples'
            qmp.call('quit')
            process.wait(timeout=10)
            if options.held_reference and (out / 'replacement.img').read_bytes() != b'\x5a' * (32 * 1024 * 1024):
                raise RuntimeError('replacement medium was modified by old descriptor')
            actual = target.read_bytes()
            if not options.filesystem and actual != b'\xa5' * 4096 + bytes(2048) + b'\xa5' * (len(actual) - 6144):
                raise RuntimeError('persisted target differs outside intended write or write missing')
            record['result'] = 'PASS ' + speed_label + ' disk probe, raw write/sync/readback and persisted backing bytes'
            if options.initially_empty:
                record['result'] += '; initially empty LUN, insertion without USB reconnect'
            if options.media_exchange:
                record['result'] += '; in-place eject/change-medium and ' + ('new partition readback' if options.partitioned else 'fresh cached-offset readback')
            if options.write_error:
                record['result'] += '; backend write error, successful cold read and sticky write/fsync errors'
            if options.filesystem:
                record['result'] = 'PASS UAS UFS mount/write/sync/unmount/remount and 64KiB file readback'
            if options.mounted_exchange:
                record['result'] += '; ' + record['mounted_exchange']
            if options.lifecycle or options.inflight_disconnect or options.held_reference:
                record['result'] += '; ' + ('held descriptor replacement' if options.held_reference else 'in-flight disconnect' if options.inflight_disconnect else 'idle detach') + ', replug/readback and checked halt'
            if options.timeout_recovery:
                packets = []
                raw = (out / 'uas.pcap').read_bytes()
                offset = 24
                while offset < len(raw):
                    size = struct.unpack_from('<I', raw, offset + 8)[0]
                    offset += 16
                    packet = raw[offset:offset + size]
                    offset += size
                    if len(packet) < 64 or packet[9] != 3:
                        continue
                    if (packet[8] == ord('S') and packet[10] == 1) or (packet[8] == ord('C') and packet[10] == 0x82):
                        iu = packet[64:]
                        if len(iu) >= 4:
                            packets.append({'id': iu[0], 'tag': int.from_bytes(iu[2:4], 'big'), 'hex': iu.hex()})
                record['ius'] = packets
                aborts = [(i, p) for i, p in enumerate(packets) if p['id'] == 5]
                if options.speed == 'super' or options.removable:
                    trace = (out / 'reset.trace').read_text()
                    record['reset_trace'] = trace
                    if trace.count('usb_uas_reset') <= record['reset_count_before_failure']:
                        raise RuntimeError('no device reset after timed-out read')
                    if aborts:
                        raise RuntimeError('unexpected high-speed abort in SS reset path')
                    commands = [bytes.fromhex(p['hex'])[16:] for p in packets if p['id'] == 1]
                    reads = [i for i, c in enumerate(commands) if len(c) >= 10 and c[0] == 0x88 and int.from_bytes(c[2:10], 'big') == 8192]
                    if len(reads) != 2 or not any(c[0] == 0x12 for c in commands[reads[0]+1:reads[1]]):
                        raise RuntimeError('missing reprobe between failed and new READ')
                    record['result'] += '; device reset, reprobe and successful new read after timeout'
                else:
                    if len(aborts) != 1:
                        raise RuntimeError('expected one captured ABORT TASK')
                    index, abort = aborts[0]
                    wire = bytes.fromhex(abort['hex'])
                    failed_tag = int.from_bytes(wire[6:8], 'big')
                    if len(wire) != 16 or wire[4] != 1 or failed_tag == abort['tag']:
                        raise RuntimeError('invalid native abort identity')
                    responses = [p for p in packets[index + 1:] if p['id'] == 4 and p['tag'] == abort['tag']]
                    if len(responses) != 1 or bytes.fromhex(responses[0]['hex'])[7] != 0:
                        raise RuntimeError('missing TMF COMPLETE response')
                    if not any(p['id'] == 1 and p['tag'] == failed_tag for p in packets[:index]):
                        raise RuntimeError('aborted tag has no earlier command')
                    response_index = next(i for i in range(index + 1, len(packets))
                                          if packets[i]['id'] == 4 and packets[i]['tag'] == abort['tag'])
                    new_commands = [p for p in packets[response_index + 1:] if p['id'] == 1]
                    if not new_commands or new_commands[0]['tag'] in (failed_tag, abort['tag']):
                        raise RuntimeError('no fresh command after task retirement')
                    previous = [p for p in packets[:index] if p['id'] == 1 and p['tag'] == failed_tag]
                    failed_cdb = bytes.fromhex(previous[0]['hex'])[16:]
                    new_cdb = bytes.fromhex(new_commands[0]['hex'])[16:]
                    if len(previous) != 1 or failed_cdb != new_cdb or failed_cdb[0] != 0x88 or int.from_bytes(failed_cdb[2:10], 'big') != 8192:
                        raise RuntimeError('failed and explicitly retried READ identities differ')
                    record['result'] += '; captured task abort and successful new read after timeout'

        except BaseException as error:
            record['failure'] = str(error)
            if qmp is not None:
                try:
                    record['failure_registers'] = qmp.call('human-monitor-command', {'command-line': 'info registers -a'})
                except Exception as diagnostic_error:
                    record['diagnostic_error'] = str(diagnostic_error)
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
            record['source_unchanged'] = digest(source) == record['source_sha256']
            (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        if not record['source_unchanged']:
            raise RuntimeError('source image changed')
        print(record['result'], flush=True)

if __name__ == '__main__':
    main()
