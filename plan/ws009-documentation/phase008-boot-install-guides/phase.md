# WS009-p008: boot, root modes and installation status

Date: 2026-09-09
Status: uncleared (q150)
Parent: [WS009](../ws.md)
Timebox: 30 active minutes

Result: [boot/storage guide](../../../docs/howto/boot-and-storage.md) and
[q150 verification](../phase007-current-architecture-uapi/results.md) published.
Existing boot/root evidence is cross-linked. DOC-34's executable public
installation procedure cannot be completed before WS019-p004/p005. DOC-33's
native preparation/acceptance remains bounded by that producer rather than
presenting an empty mkfs image as bootable. Resume after the public installer
and installed-NVMe-only boot pass; reproduce the final guide then. No human
decision is required to continue independent producer implementation.

Document actual BIOS/UEFI flow and diagnostics (DOC-30), private boot filesystem
and file-backed root selection (DOC-32), native UFS root boundaries (DOC-33),
and USB trial/current installation availability (DOC-34). Reuse supported build
commands, retained exact boot parameters and recent QEMU proof. Explain the
independent boot provenance and boot0 role, single UFS format and ZEDSWAP2,
bounded recovery and limits of existing media. Never supply an executable
zedinst recipe before WS019-p004/p005 are accepted.

Cross-link existing formatter, block administration, boot provenance and kernel
parameter references. Verify all relative links and exact current source names.
Retain DOC-34's installed-NVMe procedure as uncleared if its producer is still
incomplete, naming the concrete resume condition. No physical-media writes or
new clean build are required for documentation-only synchronization; retain
the existing clean-build proof and distinguish it from current incremental gates.
