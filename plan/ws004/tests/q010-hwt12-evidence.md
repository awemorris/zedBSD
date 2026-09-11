# q010 HW-T12 heap and USB acceptance evidence

Date: 2026-08-26

Phases: `ws004-p008`, resumed `ws004-p006`

Disposition: **automatic QEMU gate passed; detailed manual acceptance pending**

## Root cause and correction

The q009 fault occurred immediately after init started syslogd. At startup,
syslogd requests `kern.msgbuf`, whose output is larger than the syscall's
256-byte stack buffer. The amd64 kernel ELF showed that this one syscall path
called libc `malloc/free`. `heap_active_set(&kernel_heap)` pointed those calls
at the same allocator used by `kern_malloc/free`, but libc's weak
`__libc_heap_lock/unlock` hooks were no-ops in the kernel. The syslogd syscall
could therefore update the free list concurrently with locked kernel
allocations on another CPU. This directly explains the invalid `next_free` seen
in q009's `remove_free()` fault.

The syscall now uses `kern_malloc/free`, removing all standard `malloc/free`
call sites from the linked amd64 kernel. Strong kernel implementations of the
libc heap hooks also join any future compatibility allocation to the same
IRQ-safe kernel heap lock domain.

The focused allocator test exposed a second deterministic defect: when an
aligned allocation had a nonzero prefix too small to hold a block header, the
allocator advanced by only one alignment unit. For 16-byte alignment the
prefix could remain too small, placing the aligned block header inside its
predecessor header and underflowing capacity. The fixed calculation advances by
enough whole alignment units to hold the header and a minimum payload. This bug
was not present in the q009 kernel ELF's live call graph, but was corrected as
part of the p008 allocator audit.

## Focused and control results

- deterministic/randomized allocator, alignment, realloc, invalid-free, and
  shared lock-domain model: PASS;
- 100,000 randomized allocator operations: PASS;
- eight concurrent workers × 20,000 allocation/free operations: PASS;
- SMP=1 q35/xHCI USB control: 10/10 pass;
- SMP=4 q35/xHCI USB characterization: 10/10 pass; and
- SMP=4 i440FX IDE control: 10/10 pass.

An attempted q35 IDE control was excluded: zedBSD has no matching q35 IDE/AHCI
driver, so it correctly found zero physical disks. The valid existing IDE
control uses `-machine pc` and the native PC/AT IDE driver.

## Revised repeated-boot gate

The user revised HW-T12 from 1,000 to 500 automatic boots on 2026-08-26, with a
more detailed manual acceptance test to follow. The already-running 1,000-count
harness was stopped after run 500; run 501 completed before QEMU termination.
All 501 recorded runs passed, so the accepted set is runs 1–500 and run 501 is
additional corroboration.

```text
base_sha256=2440fedd71d00a0fbd6b296c6844b75874e92fc2c3076b09729f1904293af0aa
qemu=QEMU emulator version 10.0.11 (Debian 1:10.0.11+ds-0+deb13u1)
fingerprint=q009-release-acquire-v1
topology=-machine q35 -m 512 -smp 4 -device qemu-xhci,id=xhci \
         -device usb-storage,bus=xhci.0 -netdev user,id=net0 \
         -device ne2k_isa,netdev=net0,iobase=0x300,irq=10
accepted_runs=500
recorded_runs=501
pass=501
usb_storage_failure=0
kernel_failure=0
harness_failure=0
```

Each run used a new copy of the same pristine image, required the build
fingerprint and `login:`, and completed the post-login settling interval. A
full rescan found no `loop1`, BOT/SCSI, xHCI, syslog I/O, amd64 fault, fatal, or
panic marker. The pristine image digest still matched after the interrupted
harness, providing the end check which the harness itself could not print after
it was stopped. The machine-readable record is
[q010-hwt12-results.tsv](q010-hwt12-results.tsv).

This clears the revised automatic QEMU gate. It does not claim that the user's
later interactive write/readback, service, reboot, or other detailed manual
acceptance has been performed.

## Manual acceptance handoff

The later manual run should use the same final image and record at least:

- startup through login with no kernel, BOT/SCSI, `loop1`, or syslog I/O marker;
- root login followed by a multi-block overlay file copy and checksum;
- a `logger` message reaching the configured log without an I/O error, thereby
  exercising the syslog path related to the original heap race;
- graceful reboot or shutdown, cold restart of the same disposable image, and
  checksum persistence; and
- ordinary console commands, including `/sbin/ifconfig`, without the earlier
  prompt-loop symptom.

Any failure reopens the owning Phase even though the 500-run automatic
threshold passed.
