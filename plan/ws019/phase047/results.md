# q180 result

Status: completed 2026-09-10

Implemented shared bounded fstab reader, mount -a swap skipping, swapon -a
with noauto/nofail and idempotent activation, and init invocation after mounts
before services. Init checks fork/wait failures and retries wait after EINTR.
Default empty swap configuration continues without opening the control device.

Host production parser/swapon tests pass in normal and ASan/UBSan builds:
`/tmp/zedbsd-q180-fstab.log`. Covers escaped fields, final unterminated line,
malformed/overlong/NUL records, continued processing, options, mixed outcomes,
duplicate activation and open/ioctl/close failures. Existing explicit swapon/
swapoff regressions pass `/tmp/zedbsd-q180-swap-existing.log`.
amd64 image build passes `/tmp/zedbsd-q180-amd64.log`.

Native source-free fixture passes: `temp/q180-native1`,
`/tmp/zedbsd-q180-native1.log`, exit 0. Public native mkfs, full immutable cp -a,
swap formatting, two source-free native boots, initial active-file mutation
refusal before manual activation, swapoff, repeated swapon -a and both normal
halts verified. The second boot reports a missing fstab source but still
activates the valid file and reaches login. QMP halt evidence records all four
CPUs halted with interrupts disabled. Production image unchanged; independently
extracted source rootfs SHA256 remains
02f271d7071d0f9acf69df8ac8fbd4647471d738a417b19e9270b069fb303bf1.

PCAT build `/tmp/zedbsd-q180-pcat.log` passed. Final help-text addition initially
failed the existing exact usage-string test; updated only its expected swapon
usage to include -a, retaining all status/no-side-effect assertions. PC98 and
final amd64 packaging checks remain. QEMU evidence predates only that usage
text and fstab comments, not functional startup/parser/activation changes.

Final PC98 /tmp/zedbsd-q180-pc98.log and amd64
/tmp/zedbsd-q180-amd64-final.log builds passed. Updated exact-usage regression
passes both modes in /tmp/zedbsd-q180-swap-existing-final2.log. Targeted diff
check clean. All phase gates satisfied; full installer transaction and actual
paging stress remain p006/p007, and graphic frontend remains p029.
