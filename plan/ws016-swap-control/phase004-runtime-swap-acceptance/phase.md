# WS016 Phase 004: runtime-swap QEMU acceptance

Last updated: 2026-08-27

WSID: `ws016`

Phase ID: `p004`

Combined ID: `ws016-p004`

Status: Planned; Queue-ready after `ws016-p001`--`p003`

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
- the Phase records exact evidence hashes without `make check`, `.internal/`,
  or a repository commit.

## Reconsideration boundary

If a repeatable kernel/storage failure appears outside the bounded runtime
manager changes, mark this Phase `uncleared`, preserve the reproducer and logs,
and extract the fault to its owning WS rather than broadening this Queue item.
