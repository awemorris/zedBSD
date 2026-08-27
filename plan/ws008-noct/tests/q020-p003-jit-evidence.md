# q020 ws008-p003 amd64 Noct JIT completion evidence

Date: 2026-08-28

Phase: [`ws008-p003`](../phase003-amd64-jit/phase.md)

Result: PASS

## Acceptance summary

| Test | Result | Evidence |
| --- | --- | --- |
| `NOCT-T020` | PASS | Direct guest RW mapping, RX transition, known native result, invalid-case rejection, and unmap |
| `NOCT-T021` | PASS | Canonical forced-JIT output plus mmap, publish, named native-entry, and teardown records |
| `NOCT-T022` | PASS | Semantically identical `-j0` run with an empty JIT debug stream, followed by a second clean forced-JIT lifecycle |

The runner's build, artifact identity, guest artifact identity, guest fatal
scan, QEMU fatal scan, and input-integrity checks also all passed. The final
project gate was `make -j16`; `make check` was not used.

## Host evidence

The following focused gates passed in the integration tree and, where
applicable, in the canonical `/home/awe/NoctLang` tree:

- static host configure/build and JIT slab isolation;
- injected `mprotect` failure with stable error propagation, invalid native
  entry suppression, and safe interpreter execution;
- injected `munmap` failure propagated by VM destruction;
- long-branch JIT compilation plus complete mmap/publish/native-entry/unmap
  lifecycle evidence;
- a JIT-disabled build and the existing CLI `NOCT_JIT_DEBUG=1 -j0` empty-output
  negative control;
- MinGW x86-64 and zedBSD preset builds;
- canonical host acceptance/build and byte-for-byte parity over all 31 changed
  canonical/integration paths.

The PC-98 bootstrap preset reached neither the changed JIT source nor the link
gate because the host lacks 32-bit libc development headers. The DOS preset
could not configure because `wcl386` is absent. Both are outside p003's amd64
scope; the implementation nevertheless confines `errno` to POSIX branches and
the MinGW build proves the Windows signature/protection path compiles.

## QEMU evidence

The reusable runner was:

```sh
plan/ws008-noct/tests/qemu-noct-jit.sh
```

It used QEMU 10.0.11, an amd64 PC/AT private configuration, a disposable copy
of `build/amd64/hdd-image.img`, and the canonical Noct artifact produced by the
`zedbsd` preset. The direct probe reported:

```text
NOCT-T020-RW-OK
NOCT-T020-RX-OK
NOCT-T020-EXEC-OK
NOCT-T020-INVALID-OK
NOCT-T020-UNMAP-OK
NOCT-T020-PASS
NOCT-T020-STATUS-0
```

The forced-JIT run returned `NOCT-JIT-RESULT-4242` and recorded the complete
supported lifecycle for the named `jit_target` function:

```text
noct-jit-memory: mmap-rw size=16777216 status=ok
noct-jit: jit_target: compiled
noct-jit-memory: mprotect-rx size=4096 status=ok
noct-jit-lifecycle: publish status=ok
noct-jit: jit_target: native-entry
noct-jit-memory: munmap size=16777216 status=ok
noct-jit-lifecycle: destroy status=ok
NOCT-T021-STATUS-0
```

The `-j0` run returned the same value with no `noct-jit`,
`noct-jit-memory`, or `noct-jit-lifecycle` output. A second forced-JIT run in
the same guest repeated the successful lifecycle and exited with status zero,
proving that teardown left the process environment and guest operational.

The canonical CMake, packaged, and staged Noct artifacts were byte-identical;
their SHA-256 was
`bccf19dbde856aeb4a0f0f2faffdc536a8a8d68155e2d6b2a2dda782fad3df8c`.
The direct probe artifact and staged copy shared SHA-256
`bf3d622d630192f80180953b967654eeb3cbf4d00842b22c517dfa78fe1d89e9`,
and the tested production image SHA-256 was
`c9de1333de0863631910afa8bbf7a5a2546324ae714bf2531fa8d8544ec2b593`.

## Evidence lifetime

The successful raw run was recorded temporarily at
`plan/ws008-noct/temp/q020-p003-jit.XFHvQv/`. Its `results.tsv` records every
case as `pass`; `run-metadata.txt` retains the exact commands, revisions,
timestamps, hashes, QEMU version, and integrity results, while the individual
test logs retain the complete markers above. The directory is ignored and
disposable. The checked-in runner, probe, Noct fixture, this result summary,
and the shared test index form the durable reproduction record.
