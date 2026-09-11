# Q079 isolated formatter acceptance

Recorded: 2026-09-06 — completed.

Parent: [test index](README.md); [q079](../../queue.md).
Implementation and previous attempts: [q078 evidence](q078-results.md).

P008 and p009 are complete. The sole q079 QEMU launch passed every guest
assertion and the final host identity/protected-metadata checks. No target
source changed after the final q078 guest run. The runner was corrected to
preserve postprocessing failures, and all builds finished before launch.

## Final verification

- Production-linked reservation: 583 checks per normal/sanitizer run.
- Shared frontend/CLI/faults: 2,583 checks per normal/sanitizer run.
- UFS1 generator/decoder: 12,499 checks per normal/sanitizer run.
- Swap generator/parser: 49,333 checks per normal/sanitizer run.
- Fresh zero-file UFS1/swap output matches the maintained Noct backend;
  nonzero input tests compare all defined metadata and preserve free areas.
- Existing swap manager, swap drain, backing claims and filesystem identity
  regressions pass after production parser extraction; fixed-width amd64/i386
  UAPI compilation passes.
- Final amd64, PC/AT and PC-98 builds pass using `make -j16` and the documented
  `ZEDBSD_CONFIG` target selection. See q078's exact logs and correction record.

The final command (historical evidence, not authority for another launch):

```sh
python3 plan/ws019/tests/run-formatter-qemu.py combined plan/ws019/temp/q079-combined-01
```

Exit status 0; `PASS combined`. One QEMU process, two guest reboots, within
the 600-second whole-cell bound. No builds ran concurrently.

The guest formatted independently allocated zero files on FAT32 with real
`mkfs -t ufs1` and `mkswap`: 32 MiB and 64 MiB respectively. It rejected the
unsupported UFS2 selector (status 2), block-device/directory/empty/unaligned
inputs, a symlink, and an active swap file (status 1). `swapon` reported
version two and 16,383 slots, and `swapoff` removed that source.

After a guest-written, fsynced and unmounted disposable configuration change,
the first reboot used `boot1:data.img` as the writable overlay upper and
`boot1:swapfile` as active swap. A file written and fsynced through the ordinary
overlay namespace was read back byte-for-byte after the second reboot, with
swap still active. Boot logs explicitly identify the generated upper.

The input production image, both GPT tables, FAT boot sector and unmanaged
sentinel hashes match before/after. The file outputs below were extracted
after runtime use: data.img includes the persistence test and is not expected
to match the pristine formatter output.

## Recorded image identities

| Object | SHA-256 |
| --- | --- |
| Production input, before and after | `2e6dfb6668a56160ab29a74d6fb667cc77aa1f915c349655b3d5535ca7e6ddd4` |
| Helper rootfs | `2edc33321f8a68d2dd8730d7771bde7ac115168534bac37adb902606aafab5e8` |
| Disposable boot before | `ca74719ee092ad31a78f9c7e7b84bebc672152d4a7d57f1fa909e2cac83f5763` |
| Native fixture root before | `c5e9e0438c189dc9c7d4acac6655d30ecd7b14333503c6566044f04d09abba7e` |
| NVMe before | `84ea5537144094a1f3f9d34c27b6b4ea12886300713114b4f9cb333a16115e6c` |
| NVMe after | `2fc02c84699eeaaef66d2d9b0bf8ff3a9b8f7eb412a4a39939c489c55b3ab74f` |
| Generated data.img after runtime | `d0eeaa25a8a02a4ce1245af2b9968587def76f6d99a2005ecec7ade6a0a209e4` |
| Generated swapfile after runtime | `95e19ea340ad309f50a5d02d9828d6cd90c5d606aa9b024fe394c41aab0cb12c` |

| Protected bytes, identical before/after | SHA-256 |
| --- | --- |
| tables | `8276cde25d856221a0cc769686e961c229e86993c050b9b80c201fe6c12db84b` |
| fat_boot | `6251ba7fee21437ae27d7ac341f495fc969db5258a5e3c9f0026bef3d1f5395a` |
| sentinel | `f887f1ffc5c334cb0f96113334e5b9266340afd4c56196c877053402d6532d27` |

The ignored `temp/q079-combined-01/` directory retains inputs/result JSON,
commands, guest/debug/monitor logs, disposable media and extracted files.
The adjacent `temp/q079-combined-01.log` retains the runner result.

## Limits and next prerequisites

Q079 establishes formatter behavior, not a complete installer. Inputs are
pre-sized files; the earlier slow target FAT growth remains an installer
readiness finding. Formatting initializes required metadata and allocated
contents; it does not erase unused blocks or swap slots. UFS2 formatting and
raw device/partition formatting remain outside the implemented interface.

P004 must resolve loader/config source provenance, Noct/native primitive
bindings, atomic no-replace publication and durable FAT publication before
installer implementation. These findings are recorded in its Phase. No
commit, push, physical installation or firmware-variable modification occurred.
