#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${BOOTS_ARCH:-pc98}"
build="${BOOTS_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/user-init"
files="$work/files"
image="$work/user-init.img"
monitor="$work/monitor.sock"
before="$work/menu-before.ppm"
after="$work/menu-after.ppm"
mode="${BOOTS_USER_TEST_MODE:-int}"

case "$mode" in
int)
	elf_target=INIT.ELF
	elf_source="$build/INIT.ELF"
	probe_symbol=user_int_probe
	probe_magic=0x42544332
	probe_size=32
	expect_failure=0
	;;
fault)
	elf_target=USER-FAULT.ELF
	elf_source="$build/USER-FAULT.ELF"
	probe_symbol=user_fault_probe
	probe_magic=0x42544654
	probe_size=36
	expect_failure=0
	;;
missing)
	elf_target=
	elf_source=
	probe_symbol=user_int_probe
	probe_magic=0x42544332
	probe_size=32
	expect_failure=1
	;;
malformed)
	elf_target=
	elf_source="$repo/tests/invalid-init.txt"
	probe_symbol=user_int_probe
	probe_magic=0x42544332
	probe_size=32
	expect_failure=1
	;;
*)
	echo "unknown BOOTS_USER_TEST_MODE: $mode" >&2
	exit 1
	;;
esac

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "BIOS directory not found: $bios_dir" >&2; exit 1; }
mkdir -p "$work" "$files"
rm -f -- "$image" "$monitor" "$before" "$after" "$files/INIT.ELF"

build_targets=(BOOT.SYS)
if test -n "$elf_target"; then
	build_targets+=("$elf_target")
fi
make -C "$repo" ARCH="$arch" -j"$(nproc)" "${build_targets[@]}"
if test -n "$elf_source"; then
	cp "$elf_source" "$files/INIT.ELF"
fi
BOOTS_FILES="$files" "$repo/scripts/make-hdd-image.sh" "$image"

probe_vma="$(nm -n "$build/stage2.elf" | \
	awk -v symbol="$probe_symbol" '$3 == symbol { value = "0x" $1 } END { print value }')"
test -n "$probe_vma" || { echo "$probe_symbol symbol not found" >&2; exit 1; }
probe_phys="$(python3 - "$probe_vma" <<'PY'
import sys
vma = int(sys.argv[1], 0)
if not 0x80000000 <= vma < 0x90000000:
    raise SystemExit('probe is outside the direct-mapped kernel')
print(hex(vma - 0x80000000))
PY
)"

"$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios_dir" \
	-nic none -drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
	-display none -serial none -qmp "unix:$monitor,server=on,wait=off" \
	-snapshot -no-reboot -no-shutdown >/dev/null 2>&1 &
qemu_pid=$!
cleanup()
{
	if kill -0 "$qemu_pid" 2>/dev/null; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
	rm -f -- "$monitor"
}
trap cleanup EXIT INT TERM

python3 - "$monitor" "$probe_phys" "$probe_magic" "$probe_size" "$mode" "$expect_failure" "$before" "$after" <<'PY'
import json
import pathlib
import socket
import struct
import sys
import time

monitor, probe_text, magic_text, size_text, mode, failure_text, before_name, after_name = sys.argv[1:]
probe = int(probe_text, 0)
expected_magic = int(magic_text, 0)
probe_size = int(size_text, 0)
expect_failure = int(failure_text) != 0
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.monotonic() + 10
while True:
    try:
        client.connect(monitor)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.monotonic() >= deadline:
            raise SystemExit('QMP socket did not appear')
        time.sleep(.1)
stream = client.makefile('rwb', buffering=0)
json.loads(stream.readline())

def qmp(execute, arguments=None, reply=True):
    request = {'execute': execute}
    if arguments is not None:
        request['arguments'] = arguments
    stream.write(json.dumps(request).encode() + b'\n')
    if not reply:
        return None
    while True:
        result = json.loads(stream.readline())
        if 'return' in result:
            return result['return']
        if 'error' in result:
            raise SystemExit(f'QMP {execute}: {result["error"]}')

def memory(address, count):
    text = qmp('human-monitor-command',
               {'command-line': f'xp /{count}bx 0x{address:x}'})
    values = []
    for line in text.splitlines():
        if ':' not in line:
            continue
        for token in line.split(':', 1)[1].split():
            values.append(int(token, 16))
    return bytes(values[:count])

def key(qcode):
    qmp('input-send-event', {'events': [
        {'type': 'key', 'data': {'down': True,
          'key': {'type': 'qcode', 'data': qcode}}},
        {'type': 'key', 'data': {'down': False,
          'key': {'type': 'qcode', 'data': qcode}}},
    ]})

qmp('qmp_capabilities')
deadline = time.monotonic() + (10 if expect_failure else 35)
record = None
while time.monotonic() < deadline:
    raw = memory(probe, probe_size)
    if len(raw) == probe_size:
        if int.from_bytes(raw[:4], 'little') == expected_magic:
            fields = struct.unpack('<IIIIIIii' if probe_size == 32 else
                                   '<IIIIIIIii', raw)
            record = fields
            break
    time.sleep(.25)
if record is None and not expect_failure:
    qmp('screendump', {'filename': before_name})
    print(f'BOOT header: {memory(0x20000, 4)!r}; probe bytes: '
          f'{memory(probe, probe_size).hex()}', file=sys.stderr)
    print(qmp('human-monitor-command', {'command-line': 'info registers'}),
          file=sys.stderr)
    raise SystemExit('ring-3 INT 0xc2 probe was not observed')
if record is not None and expect_failure:
    raise SystemExit(f'{mode} INIT.ELF unexpectedly reached user mode')
if expect_failure:
    print(f'{mode} INIT.ELF was rejected; checking live GUI')
elif mode == 'int':
    magic, count, vector, cs, eip, eax, pid, tid = record
    if count < 1 or vector != 0xc2 or (cs & 3) != 3 or \
       eax != 0x49334332 or pid != 1 or tid <= 0:
        raise SystemExit(f'invalid probe record: {record!r}')
    print(f'INT 0xc2: count={count} cs=0x{cs:x} eip=0x{eip:x} '
          f'eax=0x{eax:x} pid={pid} tid={tid}')
elif mode == 'fault':
    magic, count, vector, cs, eip, error, address, pid, tid = record
    if count < 1 or vector != 6 or (cs & 3) != 3 or pid != 1 or tid <= 0:
        raise SystemExit(f'invalid fault probe record: {record!r}')
    print(f'user fault: vector={vector} count={count} cs=0x{cs:x} '
          f'eip=0x{eip:x} pid={pid} tid={tid}')

# The default AUTOEXEC draws the existing BeUI menu and waits for a key.
time.sleep(12)
qmp('screendump', {'filename': before_name})
time.sleep(1)
key('down')
time.sleep(2)
qmp('screendump', {'filename': after_name})
time.sleep(1)
before = pathlib.Path(before_name).read_bytes()
after = pathlib.Path(after_name).read_bytes()
if not before.startswith(b'P6') or not after.startswith(b'P6'):
    raise SystemExit('QEMU screenshots are not PPM images')
if before == after:
    raise SystemExit('GUI did not respond to the Down key')
qmp('quit', reply=False)
stream.close()
print('GUI remained live and responded after the user test')
PY

wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"
echo "Boots user $mode test: PASS"
