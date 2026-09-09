# q165 / ws019-p032 results

Completed 2026-09-09. Public `diskpart init` now accepts a complete proposed
GPT layout, reserves the disk, validates every partition, asks for the exact
`ERASE NAME:REGISTRATION` phrase, writes/flushes/readbacks both GPT copies and
PMBR, reloads the live children, and closes the reservation before success.
The command supports zero through sixteen partition groups. Proposed tables
are labelled Proposed rather than falsely labelled On-disk.

Existing add/delete behavior and read-only machine output remain unchanged.
This phase does not format filesystems or complete dedicated installation.

| Acceptance | Result |
| --- | --- |
| Syntax and layout | Incomplete groups, invalid GUID/overflow, zero counts, out-of-range starts, overlap, duplicate partition GUID and control characters refuse before confirmation/writes |
| No-write cases | Busy/stale/readonly admission, wrong identity phrase, EOF, missing newline, embedded CR text and prompt-output failure refuse; reservation closes |
| Publication failures | Old metadata changed at confirmation, negative/short writes, flush failure, close failure and final output failure return failure; no premature reload |
| Reload status | Failed reload after verified GPT returns 3 and reports changed metadata without rollback |
| Existing command regression | Machine record snapshots and old GPT/MBR edit tests pass |
| Host sanitizers | Ordinary and ASan/UBSan/leak modes pass: 188396 codec assertions, 815 production CLI assertions; independent Python GPT inspection passes |
| QEMU existing media | Read-only mounted FAT child and boot-source USB refuse; cancel leaves entire target hash unchanged; full 2-partition creation/reload passes |
| QEMU blank media | Cancel unchanged; empty GPT reload succeeds; subsequent ESP/UFS layout succeeds and both live child geometries match |
| Independent native inspection | Python struct/UUID/zlib verifies actual headers, entry tables, GUIDs, names, ranges and CRCs on both final images |
| Unrelated bytes | All bytes outside the GPT/PMBR metadata regions retain their before/after digest; production image remains unchanged during each QEMU run |
| Target builds | amd64/pcat/pc98 disk-image builds exit 0 |

Reusable fixtures:
- `tests/diskpart-cli-test.c` links the actual command and codec with syscall
  faults; `tests/run-diskpart-table-test.sh` runs both modes.
- `tests/run-diskpart-init-qemu.py` runs the real installed `/sbin/diskpart`
  on disposable media, including interactive confirmation.

Host log `/tmp/zedbsd-q165-host-final.log`. Build logs:
`/tmp/zedbsd-q165-amd64-final.log`, `/tmp/zedbsd-q165-pcat.log`,
`/tmp/zedbsd-q165-pc98.log`. Assertion counts include per-byte sentinels, not
188396 distinct scenarios. No tests or QEMU sessions remain running.

Native artifacts:
- `temp/q165-existing1/{result.json,guest.log,commands.log}`: existing FAT/GPT,
  cancellation, mounted child/source refusal and 2-partition creation PASS.
- `temp/q165-blank1/`: initial blank-to-two-partition acceptance PASS.
- `temp/q165-blank2/`: final proposed-label build, empty GPT and two-partition
  reload PASS. The rerun added actual empty-GPT coverage, not a timeout retry.

Final blank2 target SHA256:
`20013ea0de6a0669541ed21236dc10b7ca938b6dd5d1aea9fd662f7eee0d638c`.
Its non-table region before/after SHA256:
`cb6f92bb8345d37d852a64a2c8731da62e15df2157d1118d690b9da40dfa2815`.
Production image before/after blank2:
`d88edaa73bd72454667012563db8e46441ae3f6519aa8bc61eee11558a889086`.
Existing1 was followed only by the display-label correction; its full command
behavior evidence is preserved, and blank2 exercised the final binary.

Public interface: [block administration](../../../docs/reference/block-administration.md).
Next p006 prerequisites are partition reservation, a target-side FAT32
formatter, and native UFS formatting beyond the existing regular-image 2-GiB
limit with an appropriate initial namespace. Do not cap the native root at
2 GiB to bypass these requirements. Installer automatic layout, p007 native
copy/swap/boot and p029 graphical frontend remain required.
