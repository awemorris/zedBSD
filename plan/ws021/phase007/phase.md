# WS021 Phase 007: permanent x86_64 Linux host LLVM release

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p007`

Combined ID: `ws021-p007`

Status: complete (`q064`)

Parent: [WS021](../ws.md)

Depends on: `ws021-p002`

## Objective

Package the accepted `build/llvm/` host installation as a reproducible
x86_64 Linux archive, retain it permanently on GitHub release `rev-0`, and let
CI consume that exact archive without rebuilding LLVM on every run.

## Contract

- Asset name: `zedbsd-llvm-23.1.0-x86_64-linux.tar.gz`.
- Archive content is rooted at `llvm/` and expands into `build/llvm/` without
  changing the installed prefix layout.
- The tracked repository pins the archive SHA-256. CI fails closed on a digest
  mismatch and validates the installed identity plus required executable set.
- `make toolchain` remains the source-bootstrap path. The release asset is a CI
  acceleration path, not an unverified silent fallback.
- Release `rev-0` is permanent CI infrastructure. Upload uses `gh`; replacing
  an asset is allowed only when its tracked digest and provenance are updated
  together in this Phase.

## Work packages

1. Add deterministic archive construction from the recognized generated LLVM
   install, with stable ordering, ownership and timestamps.
2. Extract a disposable copy and rerun version, triple, ELF and required-tool
   validation against that copy.
3. Record SHA-256 and archive metadata in the tracked toolchain integration.
4. Inspect `rev-0`, upload the non-colliding asset with `gh`, and verify the
   remote asset name and size.
5. Update GitHub Actions to download the pinned asset, verify SHA-256, extract
   it below `build/llvm/`, then build sysroots and target images normally.

## Completion conditions

The remote `rev-0` asset and tracked digest agree, a disposable extraction
passes the same host-tool probes as the local installation, and CI has no need
to compile LLVM while retaining a fully verified source-build escape path.
