# Q077 functional correction results

Date: 2026-09-05. Parent: [Queue](../../queue.md).
Baseline HEAD: `d97e21c`; all work remains uncommitted on top of the inherited
q076/q077 working tree. [Findings and coverage](q077-filesystem-audit.md).

## Host evidence on corrected code

Run from `/home/awe/zedBSD`:

| Command / gate | Result |
| --- | --- |
| `python3 plan/ws018-kernel-architecture/tests/audit-legacy-disk-mount.py` | KA-T120 PASS, 1,406 active files and six manifests |
| `sh plan/ws018-kernel-architecture/tests/run-legacy-disk-mount-test.sh` | KA-T121 1,303 checks each normal/ASan/UBSan; 21 admission schedules; covered rmdir EBUSY with no callback |
| `sh plan/ws018-kernel-architecture/tests/run-ufs-metadata-audit.sh` | UFS1 6,010 / UFS2 6,595 allocation/truncate/xattr/admission checks each normal/sanitized, plus shared-block update and directory bounds |
| `sh plan/ws018-kernel-architecture/tests/run-tmpfs-partial-write-test.sh` | 79 checks each normal/sanitized |
| `make -f plan/ws001-posix/tests/credential-vfs-overlay-fault-host-test.mk run sanitize` | 3,204 checks each; real overlay preparation/copy-up/remove/rename routines |
| `make -f plan/ws001-posix/tests/credential-vfs-ufs-socket-fault-host-test.mk run sanitize` | 78 checks per UFS version/mode |
| `make -f plan/ws001-posix/tests/directory-fsync-host-test.mk run run-sanitize` | Retained fsync cells pass; UFS namespace rollback 79/83 checks normal/sanitized |
| `sh plan/ws018-kernel-architecture/tests/run-fat-native-vfs-host-test.sh` | 441,782 checks each normal/sanitized |
| `sh plan/ws019-installation/tests/run-storage-foundation-test.sh` | 20,376 checks each normal/sanitized; amd64/i386 ABI PASS |
| `sh plan/ws016-swap-control/tests/run-backing-claim-test.sh` | PASS |
| `sh plan/ws018-kernel-architecture/tests/run-filesystem-identity-host-test.sh` | 110 checks and ownership audit PASS |
| `sh plan/ws018-kernel-architecture/tests/run-ufs2-consistency-host-test.sh` | 45 checks and UFS2 ownership PASS |
| WS003 BR-T42/BR-T44 commands from their test index | PASS; binaries under WS018 temp |
| WS004 `run-devfs-block-range-test.sh`, WS006 `run-dynamic-cdev-devfs-test.sh` | PASS; dynamic devfs includes ordinary/sanitizer/analyzer |

Full logs are ignored under `plan/ws018-kernel-architecture/temp/q077-*.log`.
No aggregate `make check` or repository `.internal/` material was used.

During fixture development, linking revealed missing host dependencies after
new production paths became reachable. The overlay fixture also initially
attempted to free a static-pool inode and miscounted its injected sync index;
both harness errors were corrected before the final PASS. No production failure
was waived, and no historical baseline failure was converted to xfail.

## Supported builds

Final supported commands (all `make -j16`):

```sh
make -j16
make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pcat.mk
make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pc98.mk
make -j16 -f Makefile -f plan/ws019-installation/tests/storage-qemu.mk ws019-storage-qemu-fixture
```

amd64, i386 PCAT and PC98 builds passed before the first runtime cell. After the
final overlay rename correction, all three builds passed again for the second
cell. The fixture build also passed. Non-x86
coverage is a six-manifest/source audit, not a build or boot claim.

## Shared runtime

Both cells use the maintained `run-storage-qemu.py combined OUTPUT
--mount-protection` runner, fresh disposable boot/GPT/MBR/OVMF copies, OVMF,
`qemu-system-x86_64 -machine pc -m 512 -smp 4`, IDE boot/MBR and one NVMe GPT disk.
Each cell has 120-second boot and 600-second whole-cell ceilings.

1. `plan/ws019-installation/temp/q077-resume-01`: PASS, including reboot.
   Production input SHA-256:
   `f2e98482be16b446ed5a90ed73ae68080107af248644d7f7325f455ff70072e8`.
   Later final source review diagnosed the rename counterpart of FS-A09/A11:
   directory sequence publication and hidden-lower old-name whiteout. That
   concrete correction justifies the second/final launch under q077's budget.
2. `plan/ws019-installation/temp/q077-resume-02`: PASS on the final source,
   including the same mounted/protection/reboot and image-preservation gates.
   Final production input SHA-256:
   `87e913011a34f008c34443fbc6cdc02f741ba36f90afb3ccd7f8ab43471bb4ec`.
   This is the final-source acceptance; the earlier cell is retained separately.

Both cells passed three 19-check mounted probes (ro, virtual, rw) and the
3-check post-unmount probe. It also passed idle GPT/MBR add/delete/reload,
ro/rw/root disk EBUSY, mounted-add exit 3 with unchanged live devices, post-reboot
p2 discovery, and absence of automatic auxiliary mounts. The runner verifies
production input immutability, complete MBR round-trip and GPT non-table bytes.
Full QEMU command, guest and command logs remain in each output directory.

Two launches are the entire q077 runtime allowance; no third launch is
permitted by this Queue. Q076's four earlier cells remain historical evidence.
No physical medium, installer or target formatter was operated, and no commit
or push was performed in this continuation.
