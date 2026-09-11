# q153 source artifact inspection

Date: 2026-09-09
Timebox: 90 active minutes

Implement bounded Noct inspection of actual amd64 EFI loader, kernel and UFS
root image headers. Kernel admission follows the current UEFI ELF64 load plan,
including physical ranges and executable entry containment. EFI inspection
checks PE32+/amd64/EFI application and section file/image bounds. UFS inspection
checks the current little-endian 64-bit superblock geometry; it is not fsck or
a pristine-image comparison (rootfs contains installed files).

Readers close on success and failure, require exact bounded reads, and reject
unsupported source sizes. Keep decoder functions pure for independent malformed
fixtures. Capture each regular-file identity and digest before/after inspection
using existing commands, and bind those records to the q152 manifest.
No source data.img or swapfile is opened.

Validate real built artifacts plus malformed/truncated header fixtures on host
Noct, and actual source FAT files in QEMU through separately owned read-only
mounts. Full discovery, raw GPT snapshots beyond 2 GiB, lifecycle/confirmation
and public entry remain necessary for p004; do not install a partial launcher.
