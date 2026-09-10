# q137 progress

2026-09-09. Final q137 status: uncleared; see the final boundary below.

Current-source paired baseline reproduced: `temp/q137-paired-before`.
Actual command: `USB_HID_QEMU_CELLS=paired BOOT_TIMEOUT_SECONDS=90
CELL_TIMEOUT_SECONDS=300 bash tests/qemu-usb-hid-acceptance.sh OUTPUT`
(with repository paths as recorded in the campaign). Private-image build and
source/parser gates pass. USB storage root registration times out before login;
guest ends with `usb0: port 1 enumeration failed (13)`. Campaign/session 88404
was polled terminal exit 1. No QEMU remains from that campaign.

Topology is EHCI storage at port 6 and low-speed keyboard at port 1, with three
companion UHCIs. errno 13 is ENODEV. The core's initial scan logs ENODEV when a
reset/handoff leaves the EHCI port disconnected/disabled, so the port-1 message
alone does not establish why port-6 storage registration stalls. No correction
has yet been made in p010.

Next: capture bounded per-port reset/enumeration/control-transfer stages and
QMP CPU/controller state on a disposable replay, identifying whether port 6 is
entered and which checked operation fails to progress. The read-only QMP query
attempt after baseline completion found its process already terminal and wrote
nothing; no CPU snapshot was obtained. Do not infer live worker state from that
missing snapshot. Preserve q135 shutdown and current retirement contracts.

## Cause established and correction begun

`temp/q137-owner/observations.json` retains three stopped-VM stack snapshots
using target-compiler-derived structure offsets. Bootstrap thread0 is suspended
in sched_yield <- sched_clock_cpu <- timer interrupt <- legacy_root_port_reset
<- drv_usb_hcd_root_hub_changed <- drv_pci_ehci_probe_roots <- platform refresh.
It owns the topology gate while companion UHCI root workers repeatedly yield
waiting for it. thread0 is the idle task, so runnable waiters prevent its return.
This proves a boot/worker ownership starvation, not a failed descriptor result.
The QMP USB keyboard reports full-speed 12Mb/s (USB version 1), correcting the
earlier informal low-speed description. Both devices retain address zero.

Initial EHCI/UHCI/xHCI root probes now only arm the existing root worker; they
never synchronously enumerate from the boot idle task. All controller families
share this ownership requirement. xHCI publishes readiness and pending work
with release/acquire ordering. No scheduler policy, transfer retirement or
console UAPI is changed. Pending: paired acceptance, xHCI regression, host
retirement gates, three builds, halt and p009 GUI acceptance.

## Initial enumeration recovered; blocking boot context corrected

`temp/q137-paired-after` now enumerates both the EHCI storage and companion
UHCI keyboard, publishes sda, resolves the FAT boot UUID and opens both overlay
loops. It still times out before the overlay root is ready; this is not a full
acceptance pass. Session 78507 ended with exit 1.

`temp/q137-owner-after` (observer session 54647, terminal 0) captures the next
stop: CPU0 is in waitq_sleep/mutex_lock from storage_submit during FAT loop
reads, with IF clear. CPU3's storage control worker owns the storage mutex and
waits for reusable URB retirement; other workers sleep. The bootstrap idle task
cannot act as a blocking filesystem waiter. Root mounting and init startup now
run in a detached, ordinary kernel thread using the retained handoff snapshot;
CPU0 enters its real idle loop. The existing thread retirement contract reaps
the one-shot task. This adds no scheduler policy or transfer-completion bypass.
Current-source paired acceptance is running in `temp/q137-paired-worker`.

That campaign now reaches login and passes keyboard routing/events, relative
pointer events, removal/reinsertion, and stale-fd generation reuse. During
64 MiB root-disk reads plus pointer input, the input probe reports PASS but its
shell-return marker times out (8/9). Session 21860 ended with exit 1.

Before termination, bounded read-only QMP commands were atomically sent through
the verified QEMU stdin pipe; replies with `q137-readonly-*` IDs are retained in
`paired/qmp.log`. CPU0 is in libc pointer_block with IF clear. Its stack is
heap_allocator_free <- kern_free <- free_detached_tables <- hal_space_unmap
<- free_vm_page <- vmspace_destroy <- vmspace_reap_pending <- process_reaper.
Other CPUs are idle with IF enabled. A later attempt to save heap bytes found
the campaign already exited and wrote no dump. This is evidence of a heap walk
stall during process retirement, not proof of its allocation/corruption cause.
It supplies a new reproduction lead for WS002-p021, whose earlier missing-login
tests did not reproduce the old invalid free. Do not weaken the I/O acceptance
or claim p010 complete from the earlier input passes.

The updated production-main metadata host fixture passes 85 checks normally and
under ASan/UBSan, including deferred VFS startup, detached boot-task ownership,
and retained handoff identity. The first host compile lacked the host-only tid_t
shim; the maintained runner now supplies the target-width test typedef.

Explicit amd64/pcat/pc98 `make -j16` builds pass (session 23590, exit 0;
`/tmp/zedbsd-q137-{amd64,pcat,pc98}.log`). Production USB core/function model
passes 1902 checks per ordinary/sanitizer variant; HID production driver,
hot-unplug and analyzer runner passes; the standalone legacy retirement model
passes 8328 checks per ordinary/sanitizer variant (session 18096, exit 0).
The legacy shell runner's obsolete source-format/object-path gates were not
claimed as run; current complete-driver compilation is covered by the builds.

`WS002/temp/q137-halt-paired2` passes dirty USB-root halt, three consecutive
all-CPU CLI/HLT observations, next-boot hash retention and real reboot retention.
No USB shutdown errors; source-image hashes unchanged. Session 84788 exited 0.
The first halt replay disabled i8042 and therefore could not perform the
platform's keyboard-controller reset; it passed halt but not reboot. See
[p023 evidence](../../ws002-services/phase023-usb-boot-halt/results.md).

`temp/q137-xhci-worker` passes the full current-source IN-T41 campaign:
keyboard console/events, relative and absolute input, hotplug/stale-fd reuse,
64 MiB USB-root reads with pointer events and input-image integrity. Session
10496 exited 0. Paired's corresponding full workload remains failed above;
xHCI is a passing comparison, not a substitute for paired acceptance.

## Final q137 boundary

Current-source PCAT and PC98 each pass three normal login/pwd/logout/respawn
cycles (`WS002/temp/q137-normal-{pcat,pc98}`, combined session 14676 exit 0),
with original disk hashes unchanged. The shared boot-worker correction thus
has runtime coverage on all three supported x86 configurations.

q137 is finished and p010 remains **uncleared**. Initial paired boot starvation
and boot-time blocking context are corrected; full xHCI and paired dirty halt/
reboot pass. Paired HID/64 MiB I/O remains blocked by the captured heap-walk
stall. Resume with [WS002-p024](../../ws002-services/phase024-retirement-heap-integrity/phase.md)
first-failure capture, then rerun the original paired oracle unchanged. Xzed
GUI acceptance remains p009's next item after this prerequisite; not run here.
