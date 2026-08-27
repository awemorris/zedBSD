# WS008 Phase 003: amd64 Noct mmap/mprotect JIT acceptance

Last updated: 2026-08-27

WSID: `ws008`

Phase ID: `p003`

Combined ID: `ws008-p003`

Status: Planned; dependency-blocked by uncleared `ws008-p002`

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Prove that canonical Noct on amd64 zedBSD allocates writable JIT memory with
`mmap`, changes it to read/execute with `mprotect`, executes generated x86-64
code, and cleans it up with `munmap`, without mistaking interpreter fallback
for JIT success.

## Fixed acceptance model

- Runtime verification uses `qemu-system-x86_64` and the Noct artifact built by
  the `zedbsd` CMake preset and installed by p001/p002.
- Executable memory follows W^X: code is writable while generated and becomes
  readable/executable, not RWX, before the first call. A write through the old
  mapping after RX protection must not be treated as supported behavior.
- Page alignment, range rounding, partial/invalid requests, and cleanup follow
  the existing zedBSD `mmap`/`mprotect`/`munmap` contract. This Phase does not
  invent a Noct-private executable-memory syscall.
- Correct program output is necessary but insufficient. Positive stderr or an
  equivalent machine-readable hook must contain
  `noct-jit: ...: compiled`; any `fallback`, mapping failure, protection
  failure, user fault, or kernel fault fails the gate.
- The test forces eager JIT with `-j` (or the canonical equivalent) and uses a
  function whose result is checked independently. It also runs the same source
  with JIT disabled as a semantic reference, but the reference run cannot
  satisfy the JIT gate.
- A narrow defect in zedBSD VM protection transitions, libc wrappers,
  instruction-cache synchronization, or canonical Noct's zedBSD JIT branch is
  in scope. Broad VM policy redesign is not.

## Work packages

- [ ] Add a small amd64 executable-memory probe that writes a bounded native
      function into an anonymous RW mapping, transitions it to RX, executes and
      validates the result, then unmaps it.
- [ ] Add a deterministic Noct script with a forced-JIT function and known
      stdout/status, plus log assertions for compiled versus fallback paths.
- [ ] Instrument only where existing `NOCT_JIT_DEBUG` evidence is insufficient
      to distinguish `mmap`, `mprotect`, native entry, and interpreter fallback;
      keep any permanent observability opt-in.
- [ ] Run the direct VM probe first, then the Noct JIT probe in one disposable
      amd64 QEMU image and retain serial logs and exact image/config metadata.
- [ ] Fix bounded defects within the declared VM/libc/Noct target surfaces and
      rerun both probes from a freshly built image.
- [ ] Run `make -j16`, focused canonical Noct JIT tests relevant to x86-64, and
      the WS008 source/provenance audit.

## Acceptance

- `NOCT-T020`: the direct guest probe demonstrates RW allocation, RX
  transition, execution of a known x86-64 return value, rejection of invalid
  protection/range cases expected by the public contract, and successful
  unmap with no leaked mapping.
- `NOCT-T021`: the installed canonical `/usr/bin/noct` runs the deterministic
  source with forced JIT, emits its expected result/status, emits at least one
  matching `noct-jit: ...: compiled`, emits no `fallback`, and exits cleanly.
- `NOCT-T022`: the interpreter reference produces the same semantic result but
  no positive JIT record, proving the evidence checker distinguishes the two
  paths. A second forced-JIT invocation succeeds after prior VM teardown.
- Focused upstream x86-64 JIT tests and `make -j16` pass. `make check` is not
  used.

## Completion conditions

- amd64 QEMU evidence proves generated Noct native code, not the interpreter,
  executed after the supported RW-to-RX transition.
- No W+X mapping is required, no host Noct binary is tested accidentally, and
  protection failures are surfaced rather than downgraded to an accepted
  fallback.
- Teardown permits a second clean JIT run and leaves the guest operational.
- p001--p003 evidence jointly satisfies the WS completion conditions.

## Failure and resume rules

If the direct VM probe fails, stop interpreting Noct symptoms and resume at the
lowest failing syscall/VM transition. If the direct probe passes but Noct
falls back, resume in the canonical target/JIT adapter with the positive and
negative logs. If resolution requires weakening W^X, changing public VM
policy, or redesigning executable mappings, mark p003 `uncleared` and request
human review rather than enabling permanent RWX memory.
