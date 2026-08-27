# WS008 shared test index

Last updated: 2026-08-27

Reusable scripts, sources, expected outputs, and QEMU drivers created while
executing WS008 belong in this directory. Disposable build directories, disk
copies, sockets, and serial logs belong under `plan/ws008-noct/temp/` and stay
untracked. Repository `.internal/` material is not used.

## Preset and target tests

| ID | Phase | Contract |
| --- | --- | --- |
| `NOCT-T001` | p001 | Clean `cmake --preset zedbsd` configure plus actionable invalid-root failure |
| `NOCT-T002` | p001 | `cmake --build --preset zedbsd --parallel 16` and x86-64 zedBSD ELF/host-contamination audit |
| `NOCT-T003` | p001 | Exact artifact installation and deterministic non-JIT amd64 QEMU smoke |

## BeUI backend tests

| ID | Phase | Contract |
| --- | --- | --- |
| `NOCT-T010` | p002 | Mocked backend state/capability/synchronization/detach host corpus |
| `NOCT-T011` | p002 | amd64 QEMU `/dev/graphics` drawing, flush, and teardown probe |
| `NOCT-T012` | p002 | amd64 QEMU evdev keyboard/pointer injection and post-BeUI console coexistence |
| `NOCT-T013` | p002 | Legacy console-event/key-state/drain-input source and object absence audit |

The p002 QEMU probe must report graphics and input observations over serial or
another non-graphical evidence channel so acceptance is not based only on a
screenshot. It discovers event roles by `EVIOCGBIT`/identity data and must vary
event registration order in at least one focused test.

## JIT tests

| ID | Phase | Contract |
| --- | --- | --- |
| `NOCT-T020` | p003 | Direct guest anonymous RW mapping, RX transition, native execution, invalid-case checks, and unmap |
| `NOCT-T021` | p003 | Forced canonical Noct JIT result plus positive `noct-jit: ...: compiled` evidence and no fallback |
| `NOCT-T022` | p003 | Interpreter negative control and second clean forced-JIT lifecycle |

Every QEMU record names the CMake source revision/status, zedBSD configuration,
image path/hash, exact command line, QEMU version, start/end time, expected
markers, fatal scan, and exit classification. Use `qemu-system-x86_64` for the
amd64 runtime gates and disposable image copies for tests that may mutate the
guest filesystem.

## Common build gate

Run `make -j16` after each implementation Phase. Do not use the aggregate
`make check` target.
