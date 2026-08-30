# WS013 CPAR architecture review and test cases

Parent: [WS013](../ws.md)

The Runtime rows remain design reviews. p002-p004 turn the UEFI boot and
dead-source rows into focused host/build/QEMU fixtures. This index does not
authorize implementation outside an approved Queue.

## Architecture review cases

| Case | Required design result |
| --- | --- |
| `CT-D001` | The threat model states what host resources a container cannot observe or modify |
| `CT-D002` | Mount, image, loop/file backing, exit, crash, and forced-stop lifetimes have no leak or recursion gap |
| `CT-D003` | The read-only base/app composition, read-only config, writable data mounts, and private tmpfs roles are unambiguous |
| `CT-D004` | Traditional services, Runtime CPAR services, and interactive Runtime CPAR instances remain distinguishable |
| `CT-D005` | Package manifests cover dependencies, modes, owners, hashes, licenses, upgrade, and rollback |
| `CT-D006` | PID 1, runtime, service console, networking, and later resource-control ownership do not overlap |
| `CT-D007` | UEFI discovers one required `/zedbsd.cfg` on a same-disk FAT16/FAT32 and boots its single direct configuration without sections, menu, timeout, or keyboard input; legacy PC/AT and PC-98 behavior remains separate |
| `CT-D008` | Boot files remain administrable by direct filesystem operations without a second manifest, ABI registry, or `cpar boot` command |
| `CT-D009` | Runtime `cpar` grammar names base, app, data source and guest path, command, instance, failure, and cleanup behavior |
| `CT-D010` | A later `cpar build` format can reproducibly create the fixed app-image role without changing the two-image runtime contract |
| `CT-D011` | The UEFI loader searches only the loaded physical disk, errors on zero markers, warns and uses the loaded-filesystem-first/firmware-order first marker on multiples, and ignores auxiliary-disk markers |
| `CT-D012` | Installer-style two-partition UEFI boot can load `kernel=` and the configured overlay/native parameters with no zedBSD-created `Boot####`; any firmware menu/file selection used by a test is recorded separately from installer behavior |
| `CT-D013` | Missing `boot0=` and relative overlay/data/swap files expand to the selected FAT identity, while explicit boot slots, raw swap selectors, and native `rootpart=` retain the common kernel contract |
| `CT-D014` | Boot-path source is classified from all production and focused-test build owners before deletion; test-only code is not mislabeled as production-live or globally dead |

## Planned p002 discovery fixtures

| Case | Required evidence |
| --- | --- |
| `CT-T001` | A host device-path fixture proves GPT and MBR same-physical-disk comparison, partition-child stripping, malformed/truncated rejection, and cross-disk exclusion |
| `CT-T002` | The loaded SimpleFS is visited first, its repeated firmware handle is de-duplicated, and remaining handles retain exact firmware enumeration order |
| `CT-T003` | FAT16 and FAT32 BPBs with readable root `zedbsd.cfg` match; 512-byte Block I/O with 512/1024-byte BPB sectors observes exact byte capacity, while non-512 media, unsupported BPB sizes, FAT12, exFAT, non-FAT, malformed BPB, missing media, missing file, and unreadable file do not |
| `CT-T004` | Zero matches prints the not-found diagnostic and stops even when an auxiliary disk contains the marker |
| `CT-T005` | Multiple same-disk matches print a warning and retain the loaded-filesystem match, or the first firmware-order match when the loaded filesystem has none |
| `CT-T006` | The first selected FAT volume serial is formatted as canonical `UUID=XXXX-XXXX` and becomes the loader-origin identity even when `BOOTX64.EFI` came from another FAT |
| `CT-T007` | Handle-count, device-path-byte, allocation, open/read, and media-change boundaries unwind every non-selected file/root/pool resource without a write |

## Planned p003 configuration fixtures

| Case | Required evidence |
| --- | --- |
| `CT-T008` | LF, CRLF, a final unterminated line, and ignored empty lines parse; BOM/non-ASCII, whitespace tokens, comments, continuations, sections, empty names/values, and malformed lines fail |
| `CT-T009` | Each independent bound is exercised without truncation: a 4096-byte file passes the file-size gate and may then fail the smaller final-record gate, 4097 bytes fails as file-too-long, and the 64-line, 511-byte line, 255-byte normalized kernel path, and 3071-byte final-record edges pass exactly at their own valid boundary and fail immediately beyond it |
| `CT-T010` | Exactly one `kernel=` is required, its optional leading slash is normalized, safe subdirectories work, and empty/dot/dot-dot/backslash/device/volume paths fail |
| `CT-T011` | The kernel is opened only from p002's selected FAT; missing, directory, unreadable, oversized, and invalid-ELF targets fail without fixed-name or later-candidate fallback |
| `CT-T012` | Missing `boot0=` prepends the selected FAT UUID, explicit `boot0=` suppresses synthesis, configuration token order is otherwise preserved, and `kernel=` is absent from the handoff text |
| `CT-T013` | Bare `overlay-root`, `overlay-data`, and all four `swapN` file values receive `boot0:`; explicit `boot0:`--`boot3:` references stay exact |
| `CT-T014` | `/dev/NAME`, `UUID=`, `LABEL=`, `PARTUUID=`, and `PARTLABEL=` swap selectors stay exact, while an unqualified ambiguous swap value is treated as a selected-FAT file |
| `CT-T015` | Native-UFS `rootpart=`, `init=`, other boot-slot definitions, and unknown future kernel parameters pass unchanged for common-parser validation |
| `CT-T016` | Valid UEFI `LoadOptions` is ignored, no embedded parameters are merged, and missing/invalid `zedbsd.cfg` stops visibly |
| `CT-T017` | OVMF immediately boots configured overlay and native-UFS cells without a menu/countdown/key read and reports the exact expected common parameter string; the GOP descriptor and complete framebuffer mapping span pass checked length/arithmetic bounds first |
| `CT-T018` | A two-marker OVMF image warns and boots the contractually first configuration; corruption of that selected file stops instead of retrying the second |

## Planned p004 source-audit fixtures

| Case | Required evidence |
| --- | --- |
| `CT-T019` | Tracked-source, declaration, documentation, and build-rule scans find no `startup.c`, `startup_menu`, `startup_config_file`, or startup-only state after the confirmed removal |
| `CT-T020` | Every supported platform and focused boot/test image has an entry/source-list inventory; each deletion has no production/test owner and each retained candidate names its owner |
| `CT-T021` | `sched-stub.c` and `test-fault.c` receive explicit unreferenced/test/incomplete classifications instead of deletion by filename or one-target link behavior |
| `CT-T022` | `shell.c` and `device.c` are recorded as test-only while PC-98 M9 and shutdown-order fixtures consume them, and stale production-object filters are distinguished from real object ownership |
| `CT-T023` | Post-p003 UEFI LoadOptions helpers and every other new dead-source candidate have a retain/delete evidence row covering build owner, symbol references, tests, and docs |
| `CT-T024` | The supported `make -j16` gate, affected host tests, p002/p003 OVMF boot, representative legacy x86 smoke, and retained PC-98 M9 link checks pass without aggregate `make check` |

## Planned p005/p006 BIOS configuration fixtures

| Case | Required evidence |
| --- | --- |
| `CT-T025` | Source inventory proves that current PC/AT and PC-98 `BOOTZBSD.EXE` paths use fixed `VMUNIX` plus an embedded parameter record and do not implement a legacy `boot.cfg` reader |
| `CT-T026` | i386 PC/AT BIOS reaches overlay and native roots through active MBR entry, payload FAT PBR, `BOOTZBSD.EXE`, and `/zedbsd.cfg`, with exact p003 parameter text |
| `CT-T027` | One amd64 hybrid image has distinct ESP and payload FAT; SeaBIOS reaches PBR/`BOOTZBSD.EXE` and OVMF reaches `BOOTX64.EFI`, both consume the payload `/zedbsd.cfg`, kernel, and FAT UUID, and both reach the same init |
| `CT-T028` | i386 PC/AT and amd64 BIOS missing/invalid config and missing/invalid configured kernel fail visibly without embedded or fixed-name fallback; each MZ payload remains within the PBR load ceiling |
| `CT-T029` | The hybrid image validator proves ESP/payload separation, active hybrid-MBR payload selection, payload PBR, and correct placement of `BOOTX64.EFI`, `BOOTZBSD.EXE`, `/zedbsd.cfg`, kernel, and images; amd64 BIOS cannot fall back to the direct-kernel Stage 2 |
| `CT-T030` | PC-98 `BOOTZBSD.EXE` consumes root `BOOTZBSD.CFG`, matches the p003 corpus and exact record, and boots both overlay and native-root QEMU cells |
| `CT-T031` | PC-98 missing/invalid `BOOTZBSD.CFG` and kernel stop visibly; every obsolete fixed-loader entry path is either removed or proven to use the common configured path |

## Fixture rules

- Host discovery fixtures use synthetic bounded UEFI handle/device-path/BPB
  records and do not depend on host enumeration order.
- QEMU/OVMF storage cases use disposable image copies. Any fixture that writes
  a partition or injects corruption names the disposable target first.
- Debug-console evidence must include the selected candidate/order, warning or
  fatal diagnostic, loaded kernel path, and final kernel parameter text needed
  by the case; screenshots alone are insufficient for exact-string checks.
- A multiple-marker warning is a successful path only when the first candidate
  matches the defined order. A zero marker or invalid selected configuration
  never counts as success merely because some other disk could boot.
- Source-audit evidence distinguishes production, test-only, unreferenced, and
  uncertain. Uncertain files are retained and recorded as follow-up work.
