# WS024 Phase 002: single 64-bit UFS driver

Last updated: 2026-09-06

Phase ID: `ws024-p002`

Status: planned; follows p001, not queued

Parent: [WS024](../ws.md)

## Objective

Implement the p001 contract under one UFS driver owner, using the current
UFS2 codec as the baseline and preserving required behavior from both drivers.

## Work and acceptance

- Consolidate disk decoding, inode and block mapping, allocation, directory
  operations, endian helpers, metadata, sync/unmount and error handling.
- Preserve journal, snapshot, persistent quota and extended-attribute
  functionality under the unified owner; keep overlay journal semantics
  distinct. Retain q077 namespace protection, inode lifetime and rollback.
- Audit address/size narrowing at generic disk, buffer and file boundaries.
  Use checked arithmetic on 32-bit CPUs and document any lower-layer limits.
- Integrate one public registration/header contract with VFS and native-root
  probing. Coordinate removal of obsolete consumers with p003/p004.
- Pass production-linked functional, lifecycle, high-address and malformed
  image fixtures. Temporary migration code must have the p001 removal gate.

This Phase does not introduce an unrelated filesystem feature, generic cache
redesign or new block-device formatter.
