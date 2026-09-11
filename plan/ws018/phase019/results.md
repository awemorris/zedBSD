# q086 / ws018-p019 acceptance results

Date: 2026-09-06
Result: **50/50 PASS for the defined q086 scope.**
All four selected phases are complete. No commit and no aggregate make check.

The [implementation plan](../../old/fs-report-implementation-1.md) and
[50 definitions](../../old/fs-acceptance-50.md) were saved before implementation.
This is the first bounded correction cycle, not closure of all 43 review findings.
Unimplemented findings remain in the [explicit follow-up matrix](../../old/fs-report-followups.md).

## Corrections and evidence

Finite USB caller ownership, independent EP0 inflight admission, CSW/reset/sense
recovery before flush latch, retained loop logical maps inside the file/VM
transaction, coherent parent-cache writes, syscall batching and full-block UFS
overwrite read avoidance are implemented.
No write-back policy or unchecked readonly/DMA-state reset was introduced.

The complete runner:
`python3 plan/ws018/tests/run-storage-acceptance.py OUTPUT --native`
produced [results.json](../temp/q086-acceptance-final2/results.json), individual
ordinary/sanitizer logs and exact [commands](../temp/q086-acceptance-final2/commands.json).
Native faulting wifi-store runs the real production store and actual filesystem;
its explicit test failure point injects EIO before temporary-file fsync.
Fake-HCD faults are not physical USB errors.

| Scenario | Result | Execution boundary |
| --- | --- | --- |
| S01 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S02 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S03 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S04 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S05 | PASS | owned: production USB core, fake HCD; ordinary + ASan/UBSan |
| S06 | PASS | owned: production USB core, fake HCD; ordinary + ASan/UBSan |
| S07 | PASS | owned: production USB core, fake HCD; ordinary + ASan/UBSan |
| S08 | PASS | owned: production USB core, fake HCD; ordinary + ASan/UBSan |
| S09 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S10 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S11 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S12 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S13 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S14 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S15 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S16 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S17 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S18 | PASS | bot: production usb-storage, wire double; ordinary + ASan/UBSan |
| S19 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S20 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S21 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S22 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S23 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S24 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S25 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S26 | PASS | production backing-claim regression (common write/truncate admission) |
| S27 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S28 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S29 | PASS | ufs: production UFS1, injected medium failures; ordinary + ASan/UBSan |
| S30 | PASS | ufs: production UFS1, injected medium failures; ordinary + ASan/UBSan |
| S31 | PASS | ufs: production UFS1, injected medium failures; ordinary + ASan/UBSan |
| S32 | PASS | fat: production FAT + loop + buf, memory disk; ordinary + ASan/UBSan |
| S33 | PASS | ufs: production UFS1, injected medium failures; ordinary + ASan/UBSan |
| S34 | PASS | actual zedimage producer + production/current-image checker |
| S35 | PASS | actual zedimage producer + production/current-image checker |
| S36 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S37 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S38 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S39 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S40 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S41 | PASS | verbatim production syscall functions, injected copy/allocation errors; ASan/UBSan |
| S42 | PASS | verbatim production syscall functions, injected copy/allocation errors; ASan/UBSan |
| S43 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S44 | PASS | four host operation-count cells + four actual kernel/QEMU USB-root cells |
| S45 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S46 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S47 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S48 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S49 | PASS | actual amd64 kernel, FAT/loop/UFS/overlay on QEMU xHCI USB root |
| S50 | PASS | all 30 Q085 command/daemon stories; ordinary + ASan/UBSan |

S22 additionally invokes actual loop_attach_file with a malformed emitter and a
full registry, proving claim/map cleanup. Claim allocation is a double there;
S26 separately runs production claim conflict, inheritance, alias and release.
S24 executes the real buffer cache with a 4KiB line, and checks unchanged adjacent
sectors. S47 uses the same production store loader as networkd; daemon process
ordering itself is covered by S50. S43 is actual native MAP_SHARED coherence.

## Final runtime and build gates

Final configured builds, serialized with `make -j16`:
`config/ci/config-amd64.mk`, `config/ci/config-pcat.mk`,
`config/ci/config-pc98.mk`: all exit 0.

The final default kernel/fixture also passes a fresh grouped two-boot xHCI
USB-root run after the builds:
[final native transcript](../temp/q086-final-gates/q086-final-native2.log),
[guest logs and source-image checksum](../temp/q086-final-native2/).
S45–S48, S36–S40 and S43 run in one boot; S49 reads persistent data/profile
after a second boot on the same disposable disk. No physical machine was changed.

Four real experimental kernel/USB-root cells and restored production defaults:
[AB results](../temp/q086-native-ab2/results.json).
256KiB overwrite + final fsync: 440/70/450/70 ms for
(512,4000)/(4096,4000)/(512,0)/(4096,0). One sample per cell, guest timer
granularity, no physical IRQ/CPU histogram. IMOD remains 4000.
Host production syscall calls: 128 → 16 per 64KiB.
Host production FAT/buf medium writes: 16 → 4 for fragmented 8192-byte overwrite.

Maintained regressions: FAT12/16/32 ordinary/sanitizer (441782 checks each);
UFS1/UFS2 metadata concurrency and failed publication/rollback;
USB core/recovery/binding/unregister, production source/object gates;
xHCI lifecycle model; Wi-Fi30 ordinary/sanitizer.

[Gate logs and exact source/artifact manifests](../temp/q086-final-gates/).
Source-manifest SHA-256:
`35a3ccdb37804dd2bb3d632d479cef2937316f62e9cf50f8b1ec3b9ac94283ad`.

| Final artifact | SHA-256 |
| --- | --- |
| amd64 kernel | 7af2945eca47cecbb05b916a0b42f8e98b90c33ef91c0edbe65b31f4a9665072 |
| amd64 ordinary disk image | f28d860937761a56487c1ee183fbb5257db6f2b25eeaab26fbe0682f62a8fec6 |
| native acceptance UFS image | 72996d1eb6ddcabb0392de454093d9a46b8aaf8464a12317e570cf8165e48181 |
| PCAT kernel | eda2d1f6cd97a25b81c8967bfd05a4d99615b8ca5f049c5fd0f1eac4031be3a0 |
| PC98 kernel | 75f20758f5469438160373359079a661f0f752cbde6d77554cf110529799f505 |

## Corrections to test execution

The first aggregate attempt failed to link the UFS sanitizer fixture because
ASan global registration retained unrelated VFS operations. The runner now uses
the maintained UFS audit's `asan-globals=0` setting; the rerun passes.
Existing timeout tests were updated to require finite caller return while keeping
reuse/EP0 exclusion closed until actual HCD retirement. That gate found and drove
the independent EP0 marker correction; it was not relaxed to allow overlapping EP0.
GCC's impossible bus->ports allocation/null-branch leak path is documented in
P049; its exact allocation ledger still passes.

One final native attempt overlapped a build and correctly failed its unchanged
source-image checksum assertion; it is excluded from acceptance. The successful
final-native2 run was started after all configured builds finished and preserves
the source image hash. The initial AB invocation used a nonexistent make target,
then was corrected to disk-image; AB2 completes all cells and restores defaults.

