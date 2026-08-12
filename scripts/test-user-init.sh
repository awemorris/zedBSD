#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/user-init"
files="$work/files"
image="$work/user-init.img"
monitor="$work/monitor.sock"
before="$work/menu-before.ppm"
after="$work/menu-after.ppm"
mode="${ZEDBSD_USER_TEST_MODE:-int}"
qemu_memory="${ZEDBSD_QEMU_MEMORY:-64}"

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
swap)
	elf_target=USER-SWAP.ELF
	elf_source="$build/USER-SWAP.ELF"
	probe_symbol=user_int_probe
	probe_magic=0x42544332
	probe_size=32
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
	echo "unknown ZEDBSD_USER_TEST_MODE: $mode" >&2
	exit 1
	;;
esac
require_gui="${ZEDBSD_USER_REQUIRE_GUI:-0}"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "BIOS directory not found: $bios_dir" >&2; exit 1; }
mkdir -p "$work" "$files"
rm -f -- "$image" "$monitor" "$before" "$after" "$files/INIT.ELF"

build_targets=(vmunix)
if test -n "$elf_target"; then
	build_targets+=("$elf_target")
fi
"$repo/build.sh" "${build_targets[0]}" "$arch" "${build_targets[@]:1}"
if test -n "$elf_source"; then
	cp "$elf_source" "$files/INIT.ELF"
fi
if test "$mode" = swap; then
	ZEDBSD_TEST_MB=40 ZEDBSD_SWAP_SIZE_MIB=32 \
		ZEDBSD_SH_IMAGE="$elf_source" \
		ZEDBSD_FILES="$files" \
		"$repo/scripts/make-hdd-image.sh" "$image"
else
	ZEDBSD_SH_IMAGE="${elf_source:-$build/bin/sh}" ZEDBSD_FILES="$files" \
		"$repo/scripts/make-hdd-image.sh" "$image"
fi

if test "$mode" = missing; then
	offset=$((4 * 17 * 512))
	mdel -i "$image@@$offset" ::BIN/SH
fi

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
backend_vma="$(nm -n "$build/stage2.elf" | awk '$3 == "fat_swap_backend" { print "0x" $1 }')"
stats_vma="$(nm -n "$build/stage2.elf" | awk '$3 == "vm_reclaim_counters" { print "0x" $1 }')"
backend_phys="$(python3 - "$backend_vma" <<'PY'
import sys
print(hex(int(sys.argv[1], 0) - 0x80000000))
PY
)"
stats_phys="$(python3 - "$stats_vma" <<'PY'
import sys
print(hex(int(sys.argv[1], 0) - 0x80000000))
PY
)"

"$qemu" -M pc9821 -cpu 486 -m "$qemu_memory" -accel tcg -L "$bios_dir" \
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

python3 - "$monitor" "$probe_phys" "$probe_magic" "$probe_size" "$mode" "$expect_failure" "$before" "$after" "$backend_phys" "$stats_phys" "$require_gui" <<'PY'
import json
import pathlib
import socket
import struct
import sys
import time

monitor, probe_text, magic_text, size_text, mode, failure_text, before_name, after_name, backend_text, stats_text, gui_text = sys.argv[1:]
probe = int(probe_text, 0)
backend_address = int(backend_text, 0)
stats_address = int(stats_text, 0)
expected_magic = int(magic_text, 0)
probe_size = int(size_text, 0)
expect_failure = int(failure_text) != 0
require_gui = int(gui_text) != 0
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
deadline = time.monotonic() + 35
record = None
while not expect_failure and time.monotonic() < deadline:
    raw = memory(probe, probe_size)
    if len(raw) == probe_size:
        if int.from_bytes(raw[:4], 'little') == expected_magic:
            fields = struct.unpack('<IIIIIIii' if probe_size == 32 else
                                   '<IIIIIIIii', raw)
            # The real libc performs several calls.  Do not accept the
            # first mmap observation before the final exit trap has updated
            # the same record.
            if ((mode == 'int' and fields[1] >= 3 and fields[5] == 1) or
                (mode == 'swap' and
                 (fields[5] == 0x53574150 or
                  (fields[1] >= 5 and fields[5] == 1))) or
                mode not in ('int', 'swap')):
                record = fields
                break
    time.sleep(.25)
if record is None and not expect_failure:
    qmp('screendump', {'filename': before_name})
    print(f'BOOT header: {memory(0x20000, 4)!r}; probe bytes: '
          f'{memory(probe, probe_size).hex()}', file=sys.stderr)
    print(f'swap backend: {memory(backend_address, 32).hex()}; '
          f'vm stats: {memory(stats_address, 24).hex()}', file=sys.stderr)
    print(qmp('human-monitor-command', {'command-line': 'info registers'}),
          file=sys.stderr)
    raise SystemExit('ring-3 INT 0xc2 probe was not observed')
if expect_failure:
    print(f'{mode} /bin/sh was rejected and thread0 remained idle')
elif mode == 'int':
    magic, count, vector, cs, eip, eax, pid, tid = record
    # crt0/libc reaches exit(2) after mmap-backed heap setup and write(2).
    # The probe records registers at interrupt entry, so the final EAX is the
    # exit system-call number rather than the legacy one-shot probe magic.
    if count < 3 or vector != 0xc2 or (cs & 3) != 3 or \
       eax != 1 or pid != 1 or tid <= 0:
        raise SystemExit(f'invalid probe record: {record!r}')
    print(f'INT 0xc2: count={count} cs=0x{cs:x} eip=0x{eip:x} '
          f'eax=0x{eax:x} pid={pid} tid={tid}')
elif mode == 'fault':
    magic, count, vector, cs, eip, error, address, pid, tid = record
    if count < 1 or vector != 6 or (cs & 3) != 3 or pid != 1 or tid <= 0:
        raise SystemExit(f'invalid fault probe record: {record!r}')
    print(f'user fault: vector={vector} count={count} cs=0x{cs:x} '
          f'eip=0x{eip:x} pid={pid} tid={tid}')
elif mode == 'swap':
    magic, count, vector, cs, eip, eax, pid, tid = record
    if count < 2 or vector != 0xc2 or (cs & 3) != 3 or \
       eax not in (0x53574150, 1) or pid != 1 or tid <= 0:
        raise SystemExit(f'invalid swap probe record: {record!r}')
    # The detached init process is reclaimed by a low-priority kernel
    # thread.  Wait for that asynchronous cleanup before checking that all
    # swap slots and VM pages were returned.
    cleanup_deadline = time.monotonic() + 5
    while True:
        backend = memory(backend_address, 32)
        stats = struct.unpack('<IIIIII', memory(stats_address, 24))
        slot_count = int.from_bytes(backend[12:16], 'little')
        free_slots = int.from_bytes(backend[16:20], 'little')
        enabled = int.from_bytes(backend[24:28], 'little')
        if free_slots == slot_count and stats[1] == 0:
            break
        if time.monotonic() >= cleanup_deadline:
            break
        time.sleep(.1)
    if slot_count != 8191 or free_slots != slot_count or enabled != 1 or \
       stats[2] == 0 or stats[3] == 0 or stats[1] != 0:
        raise SystemExit(f'swap was not exercised: backend={backend.hex()} '
                         f'stats={stats!r}')
    print(f'swap pressure completed: count={count} pid={pid} tid={tid}; '
          f'resident={stats[0]} swapped={stats[1]} page-in={stats[2]} '
          f'page-out={stats[3]} reclaims={stats[4]} io-errors={stats[5]}')

# The default AUTOEXEC draws the existing BeUI menu and waits for a key.
# The dedicated low-memory pressure mode checks VM correctness independently;
# Noct has a separately measured nonpageable floor and its own GUI regression.
if require_gui:
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
if require_gui:
    print('GUI remained live and responded after the user test')
PY

if test "$expect_failure" = 1 && kill -0 "$qemu_pid" 2>/dev/null; then
	# An idle guest has no user process left to service a shutdown request.
	# Stop this test-owned QEMU after the rejection was observed through QMP.
	kill "$qemu_pid" 2>/dev/null || true
fi
wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"
echo "zedBSD user $mode test: PASS"
