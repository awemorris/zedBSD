# ws004-p005 QEMU USB-root evidence

Date: 2026-08-26

QEMU: `qemu-system-x86_64` 10.0.11 (Debian package
`1:10.0.11+ds-0+deb13u1`)

## Passing identity/discovery milestone

The production amd64 image boots as the only disk behind q35 `qemu-xhci` in
both firmware paths. The legacy BIOS form was:

```sh
qemu-system-x86_64 -machine q35 -m 512 -smp 2 \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=usbboot,file=build/amd64/hdd-image.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1
```

The UEFI form additionally used read-only
`/usr/share/OVMF/OVMF_CODE_4M.fd` and a disposable copy of
`OVMF_VARS_4M.fd`. OVMF loaded `BOOTX64.EFI` from the xHCI USB device; the
loader emitted `A64 UEFI ENTRY`, `ELF`, `READY`, and `EXIT` before kernel entry.

Both paths observed:

- zero firmware/IDE devices in the platform handoff;
- native xHCI MSI-X startup and USB storage registration as `sda`;
- loader-derived FAT `UUID=hhhh-hhhh` resolution to `/dev/sda1`;
- `ROOTFS.IMG` and `DATA.IMG` loop attachment, overlay mount, init, and
  `login:`.

An existing image-builder bug placed the ESP beyond GPT `last_usable_lba` and
caused OVMF to reject it. `zedimage-host` now bounds the ESP before the 33-sector
backup GPT region, and the Noct GPT checker rejects that overlap.

## Ordering and failure cases

- A raw USB decoy enumerated first as `sda`; the boot image became `sdb`, and
  its UUID resolved to `/dev/sdb1` before `login:`.
- A legacy IDE decoy enumerated first as `sda`; the USB boot image again became
  `sdb` and resolved correctly.
- Two copies with the same boot UUID were both scanned; resolver error 16
  stopped VFS instead of selecting the first disk.
- Removing the boot USB immediately after `A64 ENTRY PASS` produced
  `boot: waiting up to 5 seconds for boot storage`, then
  `boot-storage wait expired` without login or an infinite wait.
- Removing it at the same point and attaching an identical backend 0.25 seconds
  later registered `sda` during the bounded wait and reached `login:`.

## I/O and reboot finding

On a disposable USB image, root login copied `/bin/sh` to the writable overlay.
The currently serialized USB/BOT path took about 44.6 seconds. `cmp` succeeded,
and both `cksum` results were `1245252781 87952`. After QEMU exit and a fresh
boot of the same image, the two checksums still matched. The image does not
currently include a `sync` command; the completed copy and cold restart
initially appeared to provide retained-write evidence.

That observation was superseded for acceptance purposes while the defect was
open. A subsequent ordinary
USB boot reproduced the following immediately after root login:

```text
loop1: write block=48 count=8 flags=2 error=5
loop1: write block=56 count=8 flags=2 error=5
```

This is a write `EIO` on the `DATA.IMG` overlay-upper path. Reaching a prompt or
retaining one copied file does not prove error-free writable-root operation.
`ws004-p006` subsequently completed three fresh writable USB boot/login runs,
explicit overlay copies, cold retained-content verification, and an IDE
control. The exact block-48/block-56 signature did not recur. A read-only USB
backend exposed the generic EIO path: SCSI write protection was ignored and
was not propagated through private mount/loop setup. The corrected driver uses
MODE SENSE(6), preserves sense key/ASC/ASCQ, and the VFS rejects the read-write
data loop early with `EROFS`. That clearance was subsequently withdrawn: user
acceptance with newly generated images reproduced the EIO intermittently; it is
not restricted to one fixed boot point or reliably to the first boot. Added BOT
diagnostics have observed success with a zero caller-visible length for the
31-byte Bulk OUT CBW, the 4,096-byte Bulk OUT data phase, and a 13-byte Bulk IN
CSW. In the CSW case, the expected nonzero tag and successful status were copied
into the zero-initialized destination even though the caller read
`actual_length == 0`.

The xHCI Link/event correction passes a focused wrap model but did not clear the
failure. Inspection of the pre-q009 optimized amd64 object found that the
compiler emitted terminal URB status before `actual_length`; the polling waiter
used no acquire operation. q009 replaced this with a single-owner terminal
transition and release/acquire publication, and corrected xHCI cancellation's
active-request ownership. Focused publication, completion/cancel, xHCI, and
SCSI models pass.

The q009 post-fix gate produced 35 clean USB boots out of 36 attempts, then was
stopped because one run hit a separate SMP kernel-heap fault in `remove_free()`.
Rebooting that retained image reached login, excluding persistent overlay-media
corruption. q010 proved the unlocked libc/kernel heap lock-domain mismatch,
corrected it, and passed the user-revised automatic gate: the accepted first
500 plus one additional boot all passed with zero kernel or storage failure.
See [q009 history](q009-hwt12-evidence.md) and
[q010 evidence](q010-hwt12-evidence.md). Detailed manual acceptance and
physical-media acceptance remain separate; the latter is owned by WS003.

A clean guest `/sbin/reboot` initially was not accepted. The first attempt exposed an
unaligned amd64 signal FXSAVE area, which is fixed by dynamically aligning each
saved signal FPU state. Shutdown then reaches firmware and kernel entry again,
but the second boot stops because allocator `.bss` state survives the warm-reset
path. `ws004-p007` moved ELF64 NOBITS clearing to the native 64-bit loader entry
immediately before the kernel jump. Three consecutive BIOS/IDE guest reboots
and a q35/xHCI USB reboot now reach fresh login; see
[qemu-warm-reset-evidence.md](qemu-warm-reset-evidence.md).

## Build evidence

- `make -j16` passes, including the amd64 kernel, BIOS loaders, UEFI loader,
  corrected GPT image, and Noct image checker.
- The focused i386/PC/AT xHCI kernel links and passes its contract checker.
- The xHCI model fixture passes with `-Wall -Wextra -Werror`.
- `git diff --check` passes. The aggregate `make check` target was not used.
