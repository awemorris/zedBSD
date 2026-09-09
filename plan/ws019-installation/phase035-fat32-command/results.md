# q168 / ws019-p035 results

Completed 2026-09-09. `mkfs -t fat32 DEVICE` now opens and queries one block
description, validates geometry, acquires BLKRESERVE, requires exact identity
confirmation, initializes and verifies FAT32, and releases ownership on close.
The command admits 512-byte logical sectors, matching the kernel FAT driver.
Failure after a write attempt explicitly reports possibly changed media.

Native acceptance exposed a separate command-routing defect: bare `cp` still
used the shell's old builtin, creating mode 0666, while `type cp` reported
/bin/cp. Retired the duplicate dispatch, implementation and unused basename
helper (160 lines). Normal PATH resolution now uses the maintained external
cp with its existing mode, recursive, reporting and error policies. No FAT
driver workaround or cp permission override was introduced.

| Acceptance | Evidence |
| --- | --- |
| Command lifecycle | Ordinary and ASan/UBSan/leak tests: grammar, canonical name, block type, readonly/unsupported geometry, reservation, exact input/EOF/newline/CRLF, every operation failure, final close and output failure; `/tmp/zedbsd-q168-command.log` |
| Existing frontends | UFS/swap regular-file and pristine regression pass; `/tmp/zedbsd-q168-regression.log` |
| Shell dispatch/status | 21 host cases, including a PATH-selected cp with distinct arguments/output/exit 23; `/tmp/zedbsd-q168-shell.log` |
| Native admission | Read-only mounted target and source USB refuse before format; cancellation preserves whole target digest |
| Native formatting | Public mkfs initializes and verifies a 266338304-byte partition, mount and EFI directory creation succeed |
| Native file operations | Bare cp copies /bin/cp; diff compares bytes; copied executable creates a second copy; unmount/read-only remount and comparison succeed |
| Independent read | mtools lists and extracts the copied cp; full bytes match build/amd64/rootfs/bin/cp |
| Unrelated bytes | Whole range outside target partition, including GPT, unchanged; production image unchanged |
| Builds | amd64/pcat/pc98 exit 0; `/tmp/zedbsd-q168-amd64-final.log`, `/tmp/zedbsd-q168-pcat.log`, `/tmp/zedbsd-q168-pc98.log` |

Final native artifacts: `../temp/q168-fat32-final` (relative to WS directory:
`temp/q168-fat32-final`), result.json, guest.log, commands.log and copied-cp.
Production digest during final QEMU:
`13496ebce390b7e8568a06604cfdf26404fe102b2757ead1a5da968c0ea8efd1`.

Earlier terminal experiments are retained: q168-fat32-1 selected nonexistent
/bin/true; q168-fat32-2/3 failed because bare cp was the old builtin. The
initial source-permission hypothesis was disproved by a normalized source.
q168-fat32-diag/diag2 compared the actual FAT creation request (0644) with
the source mode and found the command dispatch mismatch. Temporary FAT/cp
diagnostics were removed, rebuilt, and absent from final acceptance. The final
test uses the original bare cp and source directly, without normalization.

The standalone formatter does not complete automatic provisioning or native
root installation. UFS native geometry/profile and installer integration are
next; BeUI remains required afterwards. No installer screen ran in this codec
acceptance, so the next installer execution still owes a framebuffer capture.
