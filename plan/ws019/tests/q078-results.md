# Q078 formatter evidence and remaining acceptance

Recorded: 2026-09-06 — q078 finished; p008/p009 uncleared.

Parent: [test index](README.md); [finished q078](../../history/queue-q078.md).
Phases: [p008](../phase008/phase.md),
[p009](../phase009/phase.md).

The worktree base is `ac4fb7c1a40ff301b28933e728c4eb618374c3c7`.
The implementation has host and build evidence. The fourth and final QEMU
launch passed every guest format, activation and persistence check, then
failed the harness's production-input hash assertion because a simultaneous
build regenerated the production image. This is **guest functional PASS,
harness FAIL**, not an overall passing cell. The first three launches stopped
before either target formatter ran. All four permitted launches are consumed.
P008 and p009 remain **uncleared** because the complete runtime acceptance
includes the failed provenance guard. The finite q079 resume is recorded in
the [current Queue](../../queue.md); it does not change q078's failed outcome.

## Implemented boundary

- `/sbin/mkfs -t ufs1 FILE` and `/sbin/mkswap FILE` accept an existing,
  pre-sized regular file. They do not create or resize the supplied file.
  Syntax errors return 2, operational failures 1, and completed formatting 0.
- The shared frontend checks path/descriptor identity and exact size, acquires
  a descriptor-owned kernel formatting reservation, writes and fsyncs, then
  reopens read-only and verifies while the original reservation is held.
  Both opens use `O_NOFOLLOW`, `O_CLOEXEC` and `O_NONBLOCK`. Cleanup preserves
  the first failure and closes retained descriptors.
- The initial reservation capability supports canonical FAT-backed regular
  files with complete extent coverage. It excludes conflicting mutations,
  swap/loop activation, and retained shared mappings/cache objects. Another
  open descriptor does not inherit the owner's write authority. Unsupported
  containing filesystems fail before writes; final close releases the claim.
- UFS1 uses the maintained two-cylinder-group layout, complete inode tables,
  initial overlay journals, root directories and root marker. Its read-back
  check uses the production UFS1 superblock decoder. The accepted fixed-layout
  range is 4,194,304 through 130,940,928 bytes, aligned to 1,024 bytes.
- ZEDSWAP2 uses the existing 64-byte header and 4,096-byte pages. The accepted
  range is 8,192 through 2,147,479,552 bytes, page aligned; a 64-MiB file has
  16,383 usable slots. The slot allocation bitmap is in kernel memory. The
  production v1/v2 parser was extracted to `src/kern/swap-format.c` and is
  shared with target verification without changing the on-disk format.

These commands do not implement p004 installer publication, UFS2 formatting,
raw partition formatting, filesystem resizing, or partition-table writes.

## Host and build evidence

Run the maintained recipes in [the test index](README.md). Each new formatter
runner includes ordinary and ASan/UBSan executions. The UFS1 and swap results
below cover the corrected metadata-only write/verify strategy, including
nonzero input files and independent checks of every unchanged unused area.

| Gate | Recorded result |
| --- | --- |
| Production file/backing-claim/VM reservation | 583 checks PASS in each ordinary and sanitizer run, including claim lifetime, immutable file bounds, canonical aliases, shared-map admission and retained cache exclusion |
| Shared frontend and production CLI grammar | 2,583 checks PASS in each ordinary and sanitizer run; identity changes, early rejection, reservation held through read-back, nonblocking FIFO replacement, short/error lifecycle paths and cleanup |
| UFS1 generator and production decoder | 12,499 checks PASS in each ordinary and sanitizer run; size bounds, exact maintained 32-MiB Noct image equality on zero input, complete metadata replacement on nonzero input, preservation of every free byte, corruption and descriptor I/O faults |
| ZEDSWAP2 generator and shared production parser | 49,333 checks PASS in each ordinary and sanitizer run; bounds, short/interrupted/failing transfers, read-back/parser failures, v1/labeled-v2 compatibility, exact maintained 64-MiB Noct image equality on zero input, nonzero reserved-page replacement and preservation of every unused slot byte |
| Existing swap manager | `BR-T45 swap source: PASS` after parser extraction |
| Existing VM swap drain | `SWAP-T005: production VM source drain: PASS` after parser extraction |
| Existing backing claims | `SWAP-T003/T004 backing claims: PASS` |
| Existing filesystem identity | 110 checks PASS and generic block-identity source ownership audit PASS |
| Target UAPI layout | amd64 and i386 compile checks PASS: formatting request size 32, `size_bytes` offset 8, `reserved` offset 16; existing record-lock size remains 32 |
| Initial maintained builds | amd64, i386 PC/AT and PC-98 `make -j16` PASS, including the `O_NONBLOCK` frontend change |
| Builds after the generator I/O correction | amd64 and correctly configured i386 PC/AT and PC-98 `make -j16` PASS; both corrected i386 builds exited 0 before the q079 launch |
| Disposable helper fixture | `ws019-formatter-qemu-fixture` build PASS after the I/O correction for the combined fourth launch |

The reservation fixture uses production control flow with controlled host
adapters; it is not a substitute for target mounted-file behavior. The UFS1
runner rebuilds the maintained host backend and pins host `umask 022` so
reference journal modes are deterministic. Assertion counts include repeated
range/transfer checks and are not counts of distinct scenarios. The earlier
whole-file-zero revisions passed 138 UFS1 and 786,603 swap checks per mode;
those historical counts do not describe the final strategy.

Saved transcripts are under ignored `../temp/q078/`:
`ufs1-format-bounded.log`, `swap-format-bounded.log`,
`swap-manager.log`, `swap-drain.log`, `backing-claim.log`,
`filesystem-identity.log`, `build-amd64-bounded.log`,
`build-pcat-bounded-config.log`, `build-pc98-bounded-config.log`, and
`build-fixture-bounded.log`. The initial build passes remain in
`build-amd64-final.log`, `build-pcat-final.log`, and `build-pc98-final.log`.
Other host counts were
reported by the fixture runs in the task tool transcript. The temporary ABI
source and both compiled objects are retained in the same directory.

Build setup and selection failures are retained explicitly:

1. The generated sysroot did not yet contain the new UAPI. The initial targeted
   userland build failed on the missing reservation structure/constants.
   `make -j16 sysroots` resolved that prerequisite; `userland-build.log` and
   `sysroots.log` retain the failure and regeneration evidence.
2. Running PC/AT and PC-98 builds concurrently collided at their shared
   `build/arch-images/i386.ufs` staging tree. PC/AT could not apply a terminfo
   mode after the path disappeared; PC-98's disk-image backend then failed.
   The failed logs are `build-pcat.log` and `build-pc98.log`. Sequential reruns
   passed (`build-pcat-serial.log`, `build-pc98-serial.log`), followed by the
   final sequential builds listed above. No parallel-build correctness claim
   is made.
3. The post-I/O-correction attempts named `build-pcat-bounded.log` and
   `build-pc98-bounded.log` used `MACHINE=pcat` / `MACHINE=pc98`. The top-level
   Makefile selects its target through `ZEDBSD_CONFIG`, so these commands
   rebuilt amd64 instead. Their success is not final i386 build evidence.
   The second rebuild also regenerated `build/amd64/hdd-image.img` while the
   fourth QEMU cell was running and caused its final input-invariance assertion
   to fail. Correct sequential reruns use the `ZEDBSD_CONFIG=...` recipes in
   the test index. PC/AT passed in `build-pcat-bounded-config.log` and PC-98
   passed in `build-pc98-bounded-config.log`, both with exit 0 before q079's
   launch.

## Bounded QEMU attempts — guest PASS, final harness FAIL

Each launch used `qemu-system-x86_64`, a disposable production-image copy,
fresh OVMF variable storage, and independently generated auxiliary GPT/FAT32
and MBR media. The fixture rootfs was copied to the disposable boot image's
resolved payload partition; the combined cell also places it in an auxiliary
native UFS partition for its initial boot. Boot and prompt waits are bounded at
120 seconds, with a 600-second whole-cell ceiling. Guest commands run through
a waitpid exit observer; the harness does not infer exit status from a later
shell input line.

The exact launches were:

```sh
python3 plan/ws019/tests/run-formatter-qemu.py format plan/ws019/temp/q078-format-01
python3 plan/ws019/tests/run-formatter-qemu.py format plan/ws019/temp/q078-format-02
python3 plan/ws019/tests/run-formatter-qemu.py format plan/ws019/temp/q078-format-03
python3 plan/ws019/tests/run-formatter-qemu.py combined plan/ws019/temp/q078-combined-04
```

These are historical commands, not instructions to repeat them. Output
directories must be fresh, and repeated launches count against the same cap.

| Launch | Observed result and diagnosed stop |
| --- | --- |
| `q078-format-01` | Boot/login and explicit FAT mount passed. The fixture invoked `truncate` before creating the target. Existing `truncate` requires an existing path and returned ENOENT. No formatter invocation. |
| `q078-format-02` | Boot/login and explicit FAT mount passed. `touch` attempted ordinary mode `0666`; blank FAT cannot represent that creation mode and returned EOPNOTSUPP. The fixture was corrected to create with mode `0755` and zero umask. No formatter invocation. |
| `q078-format-03` | Boot/login and explicit FAT mount passed. The corrected helper opened the target, but growing `data.img` to 32 MiB exceeded the 120-second command bound. Read-only host inspection found 10,160 allocated 2-KiB clusters, approximately 20 MiB of progress, while the directory entry still had size zero. This was allocation progress, not evidence of a deadlock. No formatter invocation. |
| `q078-combined-04` | All guest formatter, refusal, swap activation/deactivation, generated-overlay boot, two reboot and persistence checks PASS. Protected GPT/FAT/sentinel equality also passed. The subsequent production-input SHA-256 assertion failed because a simultaneous misselected build regenerated the amd64 input image. Overall harness FAIL; no fifth launch. |

Failure records remain in each cell directory as `result.json`, `inputs.json`,
`commands.log`, `guest.log` and `qemu.log`. The runner stops its child on
failure. The before-image digests recorded for all four actual launches are:

| Launch | Image | SHA-256 before launch |
| --- | --- | --- |
| 01 | Production input | `73cf9abaabeb0c01a3627fec44a6960c74e90fb87f408cebbee2283a87e50856` |
| 01 | Helper rootfs | `031737e62b98ff043198debcf43ebed73fa9327b0f1a25d6dc0ba115c8731b33` |
| 01 | Disposable boot | `fbbd0ced1f780d099e9879364d753dfaafffaee58eb2c0f83785c247333657c9` |
| 01 | Auxiliary GPT/FAT32 | `7d9c7f35a4559ecbe2b368b8843c527a517c1ba015b4e4e5164efce9e64eda1f` |
| 02 | Production input | `73cf9abaabeb0c01a3627fec44a6960c74e90fb87f408cebbee2283a87e50856` |
| 02 | Helper rootfs | `031737e62b98ff043198debcf43ebed73fa9327b0f1a25d6dc0ba115c8731b33` |
| 02 | Disposable boot | `588e8a4100d8e06877a54a13807233de6768cb1af3834fdfbcf324192655288e` |
| 02 | Auxiliary GPT/FAT32 | `9f85654311b315c2282c0f9ee058a0a2c1e44615e995c46253d81f49c0678c48` |
| 03 | Production input | `c7b8e2a96627705f0539846a56fb549d57be5030ead48c506b8eb31824ca7d3c` |
| 03 | Helper rootfs | `bb40270552ca346e1d62825b64e6bc00de6f11020990187939a8d109164d1a77` |
| 03 | Disposable boot | `0232924027a1f23105e713bc54292f509939642a6aea46c2ff87d4715e2ac35f` |
| 03 | Auxiliary GPT/FAT32 | `7ca92d76b087343d0b06225e79813a4fc6613aa52f32e1cf632010f7e0cf54f2` |
| 04 | Production input | `8be54d336dba18ab136c0196c208c79ca19cb3dd1946ef6c2e6c52299a7f6b43` |
| 04 | Helper rootfs | `2edc33321f8a68d2dd8730d7771bde7ac115168534bac37adb902606aafab5e8` |
| 04 | Disposable boot | `306ecdee3d4ca270d31985fb8d658ea8d90a71f76f32b33491ba21217ab433ad` |
| 04 | Initial native UFS disk | `c5e9e0438c189dc9c7d4acac6655d30ecd7b14333503c6566044f04d09abba7e` |
| 04 | Auxiliary GPT/FAT32 | `84e3eaa4132280b16d09e47d0fe9b09d5c9e13cb819ef78692573a9cf742f602` |

The first three failed cells did not reach the protected-metadata/hash gate.
The fourth passed the protected GPT/FAT/sentinel assertion but failed the
following production-input assertion. The before-image hashes do not supply
the missing production-input invariance result.

The fourth cell's trace confirms target `mkfs` exit 0 with 33,554,432 bytes,
`mkswap` exit 0 with 16,383 slots, expected exit 2 for unsupported UFS2 syntax,
and exit 1 for block/directory/empty/unaligned/symlink targets and active swap.
`swapon` and `swapoff` succeeded with production source queries confirming
presence and absence. After the configuration update, both reboots reached
login with active `boot1:swapfile`; the first selected `overlay on / type
overlay (rw)`, and the final read verified the persisted marker.

At the time of launch, postprocessing assertions were outside the runner's
failure-recording `try` block. The failed source-hash assertion therefore
produced a traceback in `../temp/q078/qemu-combined-04.log` after QEMU had
already stopped, without automatically creating `result.json`. The retained
manual `result.json` explicitly marks `recorded_after_failure: true`, records
`FAIL combined host provenance guard`, and separately records all guest
commands and two reboots as PASS. It does not convert the overall cell into
PASS. The retained post-failure hashes are:

| Artifact | SHA-256 after the fourth cell |
| --- | --- |
| Rebuilt production input | `2e6dfb6668a56160ab29a74d6fb667cc77aa1f915c349655b3d5535ca7e6ddd4` |
| Auxiliary GPT/FAT32 image | `fb7822482b225a92291ad4fb01a582597c8275a29a71a055e8eb3b0cf9865a5e` |
| Protected partition-table regions | `8276cde25d856221a0cc769686e961c229e86993c050b9b80c201fe6c12db84b` |
| Protected FAT boot sector | `6251ba7fee21437ae27d7ac341f495fc969db5258a5e3c9f0026bef3d1f5395a` |
| Protected unmanaged sentinel | `2d4b153b6531a6372847f6267dca9be7e62c506f3e2e62c41499052f3d4c41b0` |

## Diagnosed correction and remaining acceptance

The formatter contract starts from a pre-sized file. The corrected fixture
places independently created, all-zero 32-MiB and 64-MiB files onto its disposable
FAT filesystem before boot. This removes guest file growth from the formatter
acceptance cell. They are empty input storage, not formatted templates: target
`mkfs` and `mkswap` must construct their own metadata. Efficient target staging
file creation remains relevant to later installer work and is not proven by
this fixture change.

The first generator revision zeroed the entire file, although free UFS blocks
and unused swap slots do not need initialization for format correctness. The
current syscall path splits transfers into 512-byte chunks, and FAT writes
validate the complete backing chain for each transfer. The large unused-area
write/scan therefore adds avoidable work beyond the diagnosed file-growth
cost. The corrected generators initialize and verify all metadata and allocated
initial content while preserving unused areas:

- UFS1 first invalidates the primary superblock, writes both cylinder groups,
  complete inode tables, reserved regions and initial file content, and
  publishes the primary superblock last. It writes 416 KiB and reads 416 KiB
  during verification, independent of unused capacity in the accepted range.
- ZEDSWAP2 clears its complete reserved 4,096-byte header page, then publishes
  the 64-byte header. It writes 4,160 bytes and verifies all 4,096 reserved
  bytes. No data slot is initialized or interpreted as format metadata.
- Nonzero 32-MiB UFS1 and 64-MiB swap inputs acquire deterministic metadata
  equal to the maintained Noct output, with every free byte preserved. Fresh
  zero inputs retain complete-image equality with the maintained builder.

The production UFS1 allocator zeros a whole new block before returning it
(`src/drivers/fs/ufs1/ufs1-vfs.c:290`). File-backed swap transfers a complete
page (`src/kern/swap-source.c:143`), and VM reclaim writes that page before
publishing the slot for page-in (`src/kern/vm-reclaim.c:936,965`). Those existing
rules justify leaving unused capacity unspecified; no FAT caching or syscall
transfer policy was changed as part of this correction.

The combined fixture starts with the existing rootfs on native UFS `sdb1`,
leaving the boot FAT payload inactive. After the target format/activation
checks, it mounts that payload (`sda2`), updates and fsyncs only its disposable
boot configuration, unmounts, and reboots into the generated UFS1 overlay and
swapfile. It then writes a persistence marker and reboots again to read it.
The initial native root permits the configuration update without competing
root/swap backing claims on a second writable mount of the boot FAT volume.

Those guest checks and protected GPT/FAT/sentinel comparisons passed in the
fourth cell. The failed production-input invariance assertion leaves p008/p009
uncleared in q078. No fifth q078 launch occurred.

Q079 is a separate finite resume with exactly one fresh launch using unchanged
target code, after all correctly configured builds have finished. The
production input and helper fixture must remain stable during that cell so
the provenance guard can produce meaningful evidence. Its outcome belongs in
a separate q079 record; q078's failure remains retained even if the resume
passes.
