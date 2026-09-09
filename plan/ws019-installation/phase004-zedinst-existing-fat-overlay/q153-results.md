# q153 source inspection and nested-mount prerequisite

Date: 2026-09-09
Status: uncleared; host source decoder accepted, native integration blocked by mount path restriction

Added bounded Noct `source.noct` inspection for actual PE32+/amd64 EFI
applications, the current UEFI kernel ELF load geometry and populated 64-bit
UFS superblocks. Capturing a source combines regular-file identity, exact
bounded reads, format checks and repeated SHA-256 through existing commands.
Read helpers close after both successful and failed reads. Source upper and
swap objects are not read. A root image is not required to match an empty
formatter template, and this is not a complete fsck implementation.

Host tests against the actual built BOOTX64.EFI/vmunix/amd64.ufs pass in JIT
and interpreter modes, including malformed headers and 64 exact-read failures.
The current root producer uses 8-KiB blocks and omits the optional summary
area; source validation was corrected to accept that existing supported layout.
Evidence: `/tmp/zedbsd-q153-host.log`, reusable `installer-source.noct`.
The private amd64 fixture builds successfully (`/tmp/zedbsd-q153-fixture.log`).

Native `temp/q153-source/result.json` is **FAIL guest**: the first
`mount -t fat -o ro sda1 /run/q153-esp` returns EINVAL, before source inspection.
The fixture ESP has a credible FAT32 BPB; source inspection finds the decisive
path restriction in `src/kern/mount.c:mount`: it rejects any slash following
the leading slash. `sys_mount_call` calls that bootstrap helper directly.
Thus an ordinary nested mount path cannot reach FAT probing. This failure is
not evidence of malformed source files or a FAT32 decoder failure.

The intended installer workspace remains `/run/zedinst/...`; relocating the
public workflow into unrelated top-level directories would not complete its
ownership design. [p021](../phase021-nested-mount/phase.md) will implement the
normal process-path mount/unmount boundary and test its lifecycle. Then rerun
q153's real source capture and continue p004's discovery, ownership,
confirmation and public installation gates. No native source-capture success,
complete installer or installed boot is claimed in this queue.
