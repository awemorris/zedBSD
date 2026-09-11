# WS024 Phase 004: UFS acceptance and retired-path removal

Last updated: 2026-09-06

Phase ID: `ws024-p004`

Status: completed (q102); see [results](results.md)

Parent: [WS024](../ws.md)

## Objective

Establish end-to-end unified UFS behavior and finish removing the separate
production UFS1/UFS2 implementations and migration scaffolding.

## Work and acceptance

- Exercise high block/file offsets, arithmetic boundaries and invalid input
  on 32-bit and 64-bit ABIs without requiring equally large physical media.
- Retain file/directory/metadata, extended-attribute, quota, journal, snapshot,
  sync/unmount and recovery coverage with production code.
- Run supported configured `make -j16` builds sequentially where outputs are
  shared. Select a finite disposable runtime matrix before execution, covering
  amd64 and maintained i386 PC/AT and PC-98 consumers.
- Verify target formatting, native and overlay root startup, and persistent
  data across unmount/remount and reboot. Compare protected container bytes.
- Remove obsolete active drivers, headers, registrations, format choices,
  builders and temporary aliases/readers once migration acceptance passes.
  Audit active references without erasing historical Queue evidence.

Complete when one UFS production path remains and all selected acceptance
evidence is recorded in P/W/M. Do not use aggregate `make check`, `.internal/`
fixtures, real-media reformatting or an unbounded retry campaign.
