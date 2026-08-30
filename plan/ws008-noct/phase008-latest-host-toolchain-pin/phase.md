# WS008 Phase 008: pin the latest Noct host toolchain

Last updated: 2026-08-30

WSID: `ws008`

Phase ID: `p008`

Combined ID: `ws008-p008`

Status: Selected (`q041`); pending after `ws005-p005`

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Advance the Noct interpreter used by zedBSD's build scripts from the old
immutable revision to the latest published `awemorris/NoctLang` `main`
revision observed at Queue entry, freeze that revision as a commit hash, and
prove that a fresh `make toolchain` build and the existing Noct script smoke
remain reproducible.

This Phase concerns only the host interpreter below `build/NoctLang`. It does
not re-enable the target `/usr/bin/noct` package and does not inspect, move, or
modify the old dirty checkout below `userland/noct/NoctLang`.

## Entry evidence

- The top-level Makefile currently pins host Noct at
  `c1e4e0fcdbb7b8cdf1705601b13d57b787c61621`.
- `build/NoctLang` is a clean detached checkout of that revision.
- At the 2026-08-30 planning audit, the clean authoritative checkout
  `/home/awe/NoctLang` and its locally known `origin/main` both named
  `58bec083fd9926b386b30e02559d79db0178905a`.
- A clean archive of that observed revision passed
  `cmake --preset static` and
  `cmake --build --preset static --parallel 16` during planning. This is
  evidence for Queue readiness, not the revision-selection contract: the
  executing Queue must resolve the remote again and record its result.

## Fixed revision contract

- Resolve `https://github.com/awemorris/NoctLang.git` `main` exactly once at
  Queue entry, after network access succeeds. Record the full 40-character
  commit ID in the Phase result.
- Use that immutable commit ID for `ZEDBSD_HOST_NOCT_REVISION`; neither
  `main`, `HEAD`, a moving tag, nor a date-derived approximation is a
  reproducible pin.
- The source directory remains `build/NoctLang`, the static build directory
  remains `build/NoctLang/build-static`, and the host executable remains
  `build/NoctLang/build-static/noct`.
- The checkout rule leaves the source detached at the pin and rejects local
  tracked changes rather than overwriting them.
- Revision-bearing checkout and build stamps invalidate the old artifact when
  the pin changes. Stale stamps for another revision cannot make
  `make toolchain` report a false success.
- The host and target pins may temporarily differ after this Phase. p009
  restores one accepted revision for both when target integration can resume.

## Work packages

1. Capture the pre-change top-level pin, checkout HEAD, checkout cleanliness,
   host artifact checksum, and existing revision-stamp inventory.
2. Resolve remote `main` once, validate that the object is a commit reachable
   from the fetched upstream branch, and freeze its full commit ID.
3. Update only the top-level host pin and any revision-stamp mechanics needed
   for a clean incremental transition. Do not alter target-package holds or
   userland source paths in this Phase.
4. Make the existing `build/NoctLang` checkout acquire the selected object and
   enter a clean detached state without a destructive reset or clean. If it is
   dirty, stop and preserve it.
5. Build the upstream `static` preset and run the project-owned toolchain smoke
   through `make toolchain`.
6. Run `make toolchain` a second time to prove the pinned incremental path and
   stamp selection are stable.
7. Record the selected SHA, executable checksum, smoke output, and relevant
   stamp names in the Phase result.

## Verification

- `NOCT-T060`: the recorded remote `main` object is a full commit hash and
  exactly matches `ZEDBSD_HOST_NOCT_REVISION`.
- `NOCT-T061`: `build/NoctLang` is clean, detached, and exactly at that hash.
- `NOCT-T062`: `cmake --preset static` and the static build complete from the
  selected source, producing an executable `build-static/noct`.
- `NOCT-T063`: `make toolchain` runs
  `plan/ws010-scripting/tests/toolchain-smoke.noct`, produces its exact success
  marker, and a second invocation succeeds without selecting another source
  revision.
- `NOCT-T064`: source/stamp audit proves no target Noct hold, target package,
  `userland/noct/NoctLang`, or `userland/base/noct/noct` path was modified by
  this Phase.
- Run `git diff --check`. Do not run `make check` and do not consume
  `.internal/`.

## Completion conditions

- The latest upstream commit resolved at Queue entry is recorded and pinned
  immutably for the host toolchain.
- The clean detached checkout and both clean/incremental toolchain invocations
  pass with identical source identity.
- The Noct-based zedBSD script bootstrap remains operational.
- No target package or maintainer checkout was touched.

## Failure and resume rules

If remote `main` cannot be resolved, the selected commit is unavailable, the
existing host checkout is dirty, or the upstream static preset/smoke fails,
mark the Phase `uncleared` with the exact SHA and first failing command. Do not
fall back silently to the old revision or a host-installed Noct binary.

The observed planning candidate is not a mandatory fallback. Resume by
resolving remote `main` afresh in a later approved Queue unless the user
explicitly selects a particular revision.

## Authorization boundary

This P book is Queue-ready, but planning does not authorize implementation.
Execution may fetch and pin the public Noct repository and edit the zedBSD
host-toolchain integration. It does not authorize changes, commits, or pushes
in `awemorris/NoctLang`, and it does not authorize target-package re-enable.
