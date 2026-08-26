# q009 HW-T12 interrupted acceptance evidence

Date: 2026-08-26

Phase: `ws004-p006`

Disposition: **Uncleared**; USB correction implemented, 1,000-run acceptance
blocked by planned `ws004-p008` SMP heap diagnosis

## Build and topology

```text
base_sha256=c59db2d2f5367735130ed77f2b0c4f499cdd0c4d40bc64a8969c4304113545bc
qemu=QEMU emulator version 10.0.11 (Debian 1:10.0.11+ds-0+deb13u1)
fingerprint=q009-release-acquire-v1
topology=-machine q35 -m 512 -smp 4 -device qemu-xhci,id=xhci \
         -device usb-storage,bus=xhci.0 -netdev user,id=net0 \
         -device ne2k_isa,netdev=net0,iobase=0x300,irq=10
```

The harness made a fresh raw copy of the byte-identical base for every run,
captured port `0xe9`, required the fingerprint and `login:`, and kept every
failed image and log. Runs were sequential.

## Result

The intended 1,000-run gate was deliberately interrupted after attempt 36:

- 35 runs reached login and completed the settling interval;
- no completed run contained a `loop1`, BOT/SCSI, xHCI, or syslog storage-error
  marker;
- run 26 hit an independent kernel fault and never reached login; and
- the retained run-26 image reached login when booted again with the same
  topology, so the fault was not persistent disk/overlay corruption.

The aggregate record is
[q009-hwt12-results.tsv](q009-hwt12-results.tsv). Run 26 was originally emitted
as `boot-timeout` because the first harness version classified storage errors
but not kernel faults. Symbolization and log inspection reclassified it as
`kernel-failure`, and the harness now performs that classification online.

The first reliable failure was:

```text
init: started syslogd pid 3
amd64 fault v=13 rip=FFFFFFFF:8026A0DF err=00000000 cr2=00000000:00000000
fatal: src/hal/amd64/int.c:129: unhandled amd64 fault
```

`addr2line -e build/amd64/vmunix -f -C 0xffffffff8026a0df` resolves the fault
to `remove_free()` in `libc/heap.c`. Disassembly identifies the faulting
instruction as the store through `block->next_free` while unlinking a free
block. Later interleaved faults in `hal_cpu_idle` and framebuffer output
occurred during concurrent fatal reporting and are secondary evidence only.

This result neither clears HW-T12 nor disproves the USB correction. It gives 35
clean observations for the repaired USB publication path, but a 1,000-run gate
requires 1,000 classified passes. [`ws004-p008`](../phase008-smp-heap-integrity/phase.md)
owns the heap fault; after its correction, HW-T12 restarts from run 1.

## Focused verification

The following completed after the interrupted gate:

```text
usb URB publication model: PASS
xHCI model test: PASS
usb-storage SCSI model: PASS
make -j16: Nothing to be done for 'disk-image'.
```

The publication model includes a deterministic legacy stale-zero case,
200,000 corrected release/acquire iterations, and 2,000 competing terminal
claims. `make check` and `.internal/` were not used.

After the final accessor hardening rebuilt the image, a separate three-run
smoke gate passed 3/3 using the same topology and fresh copies. That final image
had SHA-256
`eef3a708d1ebf5cfcb5fee4545623a69cfdef4ef15db29274c7fa6f27d0ef4fc`.
The smoke result establishes that the final artifact still boots; it is not
combined with the earlier sample and does not replace the 1,000-run gate.
