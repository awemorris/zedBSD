# WS009-p008: boot, root modes and installation status

Date: 2026-09-09
Status: completed / cleared q190
Parent: [WS009](../ws.md)
Timebox: 30 active minutes

Result: [boot/storage guide](../../../docs/howto/boot-and-storage.md) and
[q150 verification](../phase007/results.md) published.
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

## Producer handoff for the next documentation queue

WS019-p004/p005 are no longer the live implementation blocker. The public text
installer's coexistence/dedicated flows and source-free native boots passed in
q182; native UFS paging passed q183. q184 adds `/sbin/zedinst-graphic` using the
same Noct backend and relocates the text entry point to `/sbin/zedinst` while
retaining `/bin/noct`. Its graphical dedicated installation, two source-free
boots, active swap, persistence and halt have passed; remaining graphical
acceptance is still running. Resume the final guide after the p029 result is
closed, and use its final source/behavior rather than the old no-format-only
recipe. Cover source-disk admission, HTTP as unavailable, dedicated automatic
GPT/EFI/UFS layout and swap file, file/byte progress, default NO/YES, console
shell/recovery and the graphic boot mode. Distinguish RGB24 assets from actual
UEFI XRGB32 storage. The PC98 physical-boot phase remains independently open.

Producer update q186: p029 is cleared and the extra PC98 p050 normal-path
FAT installation now passes target-only boot/root login. WS019 is completed.
The installer producer no longer blocks this documentation phase. Reflect
PC98 pre-existing FAT16/bootstrap prerequisites, <=4 GiB partitions and
<=2 GiB files; native/GPT remains amd64-only. Update the guide in the next
documentation queue using these final public entry points and evidence.

## q190 final documentation acceptance (2026-09-10)

WS019 producer dependencies are complete. The guide now documents actual
/sbin/zedinst and /sbin/zedinst-graphic using /bin/noct, source-disk admission,
the unavailable HTTP option, amd64 coexistence and destructive dedicated modes,
confirmation differences, progress, shell/recovery and target-only login.
PC98 explicitly requires existing FAT16/bootstrap and its partition/file limits.
The formatter reference includes current native/FAT32 block modes and their
confirmation boundary. See [results](results.md).

Validation reuses accepted producer executions rather than launching another
installation campaign. Current source/command checks and all product relative
links pass. This closes DOC-30/32/33/34 at the accepted normal-path boundary;
physical PC98 boot recovery remains WS025-p032.
