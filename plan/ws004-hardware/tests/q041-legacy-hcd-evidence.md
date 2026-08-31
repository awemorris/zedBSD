# q041 legacy-HCD checked-retirement evidence

Date: 2026-08-31

Owner: `ws004-p016`

## Focused and build gates

The phase-owned runner completed with these results:

```text
legacy HCD retirement model: 8189 checks PASS
legacy HCD retirement model: 8189 checks PASS  (ASan/UBSan)
GCC -fanalyzer: PASS
legacy HCD checked retirement source/build gate: PASS
amd64 UEFI UHCI/EHCI/USB/storage objects: PASS
i386 PC/AT UHCI/EHCI/USB/storage objects: PASS
xHCI concurrent URB regression: PASS
USB binding transaction regression: PASS
system shutdown ordering: PASS
make -j16: PASS
git diff --check: PASS
```

The focused model includes cancellation/IRQ terminal races, timeout retention,
stale and matching IAA, duplicate/late events, worker callback re-entry, raw
UHCI register-health rejection, and normal/partial/error/cancel toggle
continuity. It does not claim that the pre-existing UHCI short-IN semantic gap
is repaired.

## QEMU lifecycle gate

The candidate image was produced and exercised with:

```sh
TMPDIR="$PWD/build/q041-tmp" make -j16 \
  ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk \
  disk-image
TMPDIR="$PWD/build/q041-tmp" \
  plan/ws004-hardware/tests/run-legacy-hcd-qemu.sh \
  build/amd64/hdd-image.img build/data.img build/q041-p016-qemu
```

Environment and immutable inputs:

```text
QEMU emulator version 10.0.11 (Debian 1:10.0.11+ds-0+deb13u1)
hdd-image.img sha256=663bcba189ef69daeb4a065a219a0ee1daa88f1f73cf06a5c7b28611cf2e0440
data.img      sha256=90238b68d79ff20d38232f416c4dd1570a0c3f72e366d294a04d6ae36377b41f
```

Both cells used OVMF/q35, an IDE root disk, and one read-only USB storage
device. The UHCI cell used `piix3-usb-uhci`; the EHCI cell used `usb-ehci`.
Both produced their checked-retirement marker, enumerated the storage device,
completed `8+0` 512-byte records through `/bin/dd`, and reached
`init: executing system action reboot` without a retirement, retained-DMA,
panic, or shutdown diagnostic:

```text
legacy HCD QEMU uhci lifecycle: PASS
legacy HCD QEMU ehci lifecycle: PASS
legacy HCD QEMU lifecycle: PASS
```

## Evidence boundary

QEMU did not inject legacy hot-unplug/cancellation, a frozen UHCI FRNUM, or a
stale, duplicate, or missing EHCI IAA. The runner records those exact absences
in each cell's metadata. Runtime legacy root-port dispatch is separately
recorded as `BUG-004`; fault and cancellation ownership remain model evidence
rather than being mislabeled as hardware evidence.
