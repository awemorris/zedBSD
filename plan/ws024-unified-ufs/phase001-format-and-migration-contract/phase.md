# WS024 Phase 001: unified UFS format and migration contract

Last updated: 2026-09-06

Phase ID: `ws024-p001`

Status: planned; not queued

Parent: [WS024](../ws.md)

## Objective

Turn the settled single-UFS/64-bit direction into a concrete implementation
and migration contract. Start from the current UFS2 codec and inventory
required behavior from both existing drivers and all image consumers.

## Work and acceptance

1. Freeze disk identification/version, metadata layout, address widths,
   geometry and supported limits, including narrower intermediate arithmetic
   on 32-bit targets and backing-store limits.
2. Inventory native/overlay root behavior, extended attributes, filesystem
   journal/snapshot regions and the distinct overlay journal files. Specify
   their initialization and recovery expectations in the unified format.
3. Specify public `ufs` naming, `mkfs -t ufs FILE`, registration and source
   ownership, and any strictly temporary transition alias.
4. Select rebuild/export/import handling for current UFS1/UFS2 images and
   rejection of unsupported legacy inputs. Record retained-data requirements
   before any conversion implementation; no automatic in-place formatting.
5. Define bounded acceptance fixtures and the producer/consumer switch order,
   so a new driver is not paired with incompatible old build artifacts.

Complete when p002/p003 can implement this contract without reopening the
unification decision or guessing about the handling of existing data.
