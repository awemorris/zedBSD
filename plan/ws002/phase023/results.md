# q135 result: completed (QEMU reproduction and correction)

2026-09-09. This is the bounded user-authorized QEMU correction, not a claim
that the user's physical machine or all USB controllers were tested.

## q137 paired-controller regression

With the current boot-worker fixes from WS006-p010, `temp/q137-halt-paired2`
passes EHCI USB-root overlay plus active swap on four CPUs, with a full-speed
USB mouse on the companion UHCI. After copying mkfs to the overlay and checking
its SHA-256, halt reaches three consecutive QMP observations of all CPUs in
HLT with IF clear, without BOT/shutdown errors. Both a subsequent boot and a
real reboot retain the same independently expected file hash. The runner ended
with exit 0 and `persistence_and_reboot: PASS`; production image hashes match.

The first `temp/q137-halt-paired` run already established halt and next-boot
persistence, but its additional reboot failed because that fixture disabled
i8042, the platform's current reset mechanism. The corrected command fixture
keeps i8042 and uses a USB mouse to exercise the companion without duplicating
keyboard input. The separate USB HID campaign still disables i8042 deliberately.
Do not count the first attempt as a passing reboot test. Physical USB remains
unverified.

## Reproduction

`temp/q135-halt-before3/result.json` (relative to the WS directory) records ordinary amd64 kernel, q35/xHCI,
USB-root overlay and active 16383-slot swap, four CPUs. After init announces
halt, storage detach returns EBUSY for referenced root media. Its control
worker remains alive and repeatedly prints `BOT CBW error=17`. QMP CPU register
samples show only one CPU with interrupts disabled; the other three are in
interruptible scheduler idle. This reproduces continued activity after halt,
not merely an incidental USB message.

The two earlier probe attempts failed before halt because of nonexistent
swapctl/sysctl fixture commands. They provide no shutdown evidence.

## Correction

- USB drivers may provide a checked terminal `quiesce` callback. Core closes
  binding admission, joins the callback, verifies transfer/callback pins have
  drained, and retains class/binding objects until reset. Drivers without the
  callback retain the existing detach path. HCD DMA quiescence remains checked;
  failed stops retain owners, and an HCD-stop retry repeats the class boundary.
- USB storage's callback excludes media revalidation, closes control/BIO
  admission, waits for the serialized BOT command and joins the control worker.
  It does not destroy root/swap-referenced disks or free their URBs. Runtime
  detach's busy refusal is unchanged. Late BIOs fail locally rather than
  submitting BOT requests after HCD shutdown.
- PC/AT halt invokes the existing terminal all-CPU stop broadcast after the
  system's writeback and device barriers. The amd64 NMI receiver now recognizes
  an explicitly published terminal stop request and parks silently; unrelated
  NMIs still follow fault handling. Broadcasting is gated on AP readiness.
  The old local CLI/HLT loop could leave three CPUs scheduling indefinitely.

The normal message `host controller quiesced; resources retained` describes
intentional retention of referenced boot-media objects; it is not an I/O or
worker-stop failure. It appears once, without the previous error/flood.

## Evidence

| Gate | Result |
| --- | --- |
| Actual USB storage host fixture: normal, ASan/UBSan, analyzer | PASS, 257 checks each; busy runtime detach, terminal worker timeout/join failure, retry, retained media/URBs, late BIO refusal |
| Actual USB core model: normal and ASan/UBSan | PASS, 1902 checks each; retained root, failed class stop, HCD stop/retry and existing runtime detach/composite cases |
| amd64, PC/AT, PC-98 `make -j16` with explicit CI config | PASS |
| `temp/q135-halt-after`: four CPUs, overlay and active swap, halt | PASS, three consecutive QMP samples with all CPUs HLT=1 and IF=0; no USB error/fatal |
| `temp/q135-halt-dirty`: write/copy/read immediately before halt | PASS; same CPU stop evidence, no explicit file sync before halt |
| Dirty cell's separate next boot and subsequent reboot | PASS; file SHA256 preserved across both, no USB shutdown error |
| `temp/q135-halt-up`: one CPU, no swap | PASS, checked CPU stop and no USB error |

Logs: `/tmp/zedbsd-q135-storage-host2.log`,
`/tmp/zedbsd-q135-core-host-final.log`,
`/tmp/zedbsd-q135-{amd64,pcat,pc98}.log`. QEMU outputs and exact argv are under
the named WS temp directories. All production boot-image hashes compare
unchanged across each runtime. Disposable boot disks and variable stores only.

The copied marker's target SHA256 is
`a021fb2a00c525d5187945690f9d6e20e7c4c15690f1e3d6197f42073a2bf065`, also equal
to host SHA256 of `build/amd64/bin/mkfs`. This supplies an actual target cksum
SHA256 check for q134; it does not complete the installer transaction.

## Remaining boundary

EHCI/UHCI cannot yet reach login in the WS006 USB-root cell; repeat halt
regression after that independent enumeration repair. Do not count its boot
failure as a halt failure. The physical machine remains untested. WS002-p021,
WS019-p004 and the other Priority residuals remain open; only p023's QEMU
correction is completed here.
