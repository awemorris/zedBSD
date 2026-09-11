# WS016 Phase 004: runtime-swap QEMU acceptance

Last updated: 2026-08-28

WSID: `ws016`

Phase ID: `p004`

Combined ID: `ws016-p004`

Status: Complete (`q021`)

Parent: [WS016](../ws.md)

Tests: [WS016 test index](../tests/README.md)

## Objective

Prove through the production UAPI and installed commands that runtime add and
remove preserve anonymous memory, source identity, commitment, and the prior
boot-time swap contract.

## Acceptance matrix

Use `qemu-system-x86_64` with disposable copies of the configured amd64 disk
image. A small production-ABI guest exerciser records source enumeration,
commit/stats transitions, deterministic memory patterns, page-out/page-in, and
full readback.

1. Boot with no active swap, run `swapon boot0:swapfile`, force at least 1024
   pages out and back in, and verify content plus nonzero counters.
2. Boot with `swap0=boot0:swapfile`, add a signed raw partition, drive enough
   pressure to use both sources in numeric ID order, and verify both identities
   and aggregate stats.
3. With pages stored on the selected source, run `swapoff`, verify complete
   drain and data readback, and prove subsequent allocations do not use it.
4. Remove and re-add a source ID only after drain; verify no stale token reads
   the new source.
5. Exercise non-root control, duplicate alias, malformed header, root/raw
   overlap, unsupported file backend, unknown removal, and unsafe commit-limit
   reduction. Every failure must preserve the prior usable pool.
6. Re-run representative BR-T46 file/raw/mixed boot cells so runtime-manager
   conversion does not regress boot activation or USB-backed page I/O.

The harness bounds boot and guest-command time, scans for kernel panic/fault,
swap/storage I/O errors, loop errors, and missing completion markers, and saves
its invocation, QEMU/OVMF versions, image hashes, guest logs, and result table
under WS016 `temp/`. No result mutates the source image.

## Completion conditions

- SWAP-T011 and SWAP-T012 pass all positive and negative cells;
- successful `swapoff` has zero target allocations/in-flight I/O before source
  disappearance and preserves every test page;
- rejected operations leave source enumeration, aggregate stats, and commit
  limits coherent;
- representative q015 boot-swap cases still report full page-in/page-out and
  readback success;
- affected host regressions, `make -j16`, and `git diff --check` pass; and
- the Phase records exact evidence hashes without `make check` or
  `.internal/`.

## Reconsideration boundary

If a repeatable kernel/storage failure appears outside the bounded runtime
manager changes, mark this Phase `uncleared`, preserve the reproducer and logs,
and extract the fault to its owning WS rather than broadening this Queue item.

## Execution result

Completed on 2026-08-28. The production-ABI guest exerciser and disposable
amd64 UEFI/q35/xHCI runner cover runtime file, mixed boot-file/runtime-raw, and
native-root negative scenarios. The mixed case proves the first transition to
source 1 occurs only after source 0 is full, verifies and releases a resident
prefix to provide bounded command headroom, runs the installed
`/sbin/swapoff` while source 0 still owns pages, and then reads back every
retained old-generation page. Removed-source skipping, lowest-ID reuse, a new
generation, aggregate free/commit coherence, and the SWAP-T005 in-flight drain
contract are also checked.

`p004-final-002` passed all six selected cells: SWAP-T011/T012 file, mixed, and
native scenarios plus representative BR-T46 amd64 UEFI file/raw/mixed boot
cells. Its `results.tsv` SHA-256 is
`94c36cc82625d8150db69269df50d02a03ba8e0f1233f4a4cdcb284b3ed07f14`.
The runtime guest-log SHA-256 values are
`3dd48c60326e47f353e7518d2e3bca050319cda5ce55fceb6c1f3044dbcff87d`
(file),
`3887777737cb6219dbace492b6b94c4d1800e8d463c8e417eae3bfa817ede79e`
(mixed), and
`ec124bcc3350b31865fcfe5713cea8f8cbe660aaaeeb6b684268ee5bc91dac2f`
(native). Source disk images, `config.mk`, and source OVMF firmware retained
their original hashes.

SWAP-T001--T010 focused regressions, including the p003 strict and ASan/UBSan
command variants, passed against the final tree. `make -j16`, `bash -n`, and
`git diff --check` passed without `make check` or `.internal/`.
