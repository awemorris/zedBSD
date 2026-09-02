# WS021 Phase 001: LLVM source acquisition and zedBSD target patch

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p001`

Combined ID: `ws021-p001`

Status: planned

Parent: [WS021](../ws.md)

## Objective

Establish one verified LLVM 23.1.0 source identity and a narrow, reviewable
downstream patch which makes zedBSD a first-class LLVM/Clang X86 OS target
without importing LLVM source into Git.

## Work packages

1. Add `toolchain/llvm/version.mk`, acquisition rules, ignore rules, and a
   patch series under `toolchain/llvm/patches/`.
2. Freeze the official release URL, archive byte count, SHA-256, release tag,
   and available official `.sig`/attestation companion. Verify existing cache
   bytes on every use.
3. Reject absolute/parent-traversing members, links and unsupported archive
   member types before publishing the cache or extraction. Download and
   extraction use sibling temporary paths, locks, and atomic rename.
4. Add LLVM Triple `ZedBSD` parsing/printing/normalization, the ELF default,
   and explicit predicates/tests for:

   ```text
   x86_64-unknown-zedbsd
   i386-unknown-zedbsd
   ```

5. Add Clang zedBSD target information and driver selection, including
   `__ZEDBSD__`, the accepted UNIX macros, LLD selection, sysroot search, and
   compiler-rt runtime identification. Do not copy zedBSD public headers into
   LLVM.
6. Make the patch strict and source-version-specific. Record a manifest of the
   release plus patch, and reject dirty, partial, or fuzz-applied extraction.
7. Connect the LLVM archive to top-level `make download` without making an
   ordinary target build acquire it implicitly.

## Verification

- Local acquisition fixtures cover wrong size/digest, unsafe path/type,
  partial download, concurrent acquisition, strict patch failure, and repeated
  offline verification.
- Unit tests prove triple parse/normalize, `UnknownVendor`, omitted
  environment, and ELF object format for both architectures.
- Preprocessor probes prove the zedBSD macro set without defining Linux,
  FreeBSD, GNU-environment, or host macros.
- The patch changes only LLVM/Clang/compiler-rt build/target integration and
  tests; it does not contain zedBSD libc, kernel, Noct, or BeUI code.
- `git diff --check` passes. Do not run QEMU in this Phase.

## Completion conditions

The official archive can be materialized and reconstructed deterministically,
both target triples are represented by a bounded strict patch, and p002 can
configure from the ignored verified extraction without a remaining product
decision.
