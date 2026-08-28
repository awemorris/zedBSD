# WS008 shared test index

Last updated: 2026-08-28

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

### Phase 001 host runner

[`noct-p001-host-acceptance.sh`](./noct-p001-host-acceptance.sh) implements
the reusable host portion of `NOCT-T001`--`NOCT-T003`. It works from clean
temporary copies of the canonical working tree, so negative configure checks
and the literal accepted commands do not reuse a prior CMake cache:

```sh
plan/ws008-noct/tests/noct-p001-host-acceptance.sh --mode all
```

The runner checks both unset and existing-but-invalid `ZEDBSD_SOURCE_DIR`
preflight failures, invokes the literal configure/build commands with the
repository root as the valid default, and audits the canonical result with
`file`, `readelf`, and `nm`. It also audits zedBSD `_start`/segment/stack
layout, generated compile and link inputs, Linux-host macro suppression, and
the absence of host headers, crt objects, and libraries. The default canonical artifact is
`build-zedbsd/noct`; use `NOCT_CANONICAL_ARTIFACT_REL` or
`--canonical-artifact-relative` only if the canonical preset deliberately
names another output.

The package-side identity check always runs when `build/amd64/bin/noct`
exists. After the amd64 package/image build, make its absence an error and
prove that installation consumed the exact canonical artifact with:

```sh
plan/ws008-noct/tests/noct-p001-host-acceptance.sh \
  --mode build --require-package-artifact
```

Parity can be regenerated without rebuilding. For Phase completion, require a
non-empty changed-path set because p001 does not commit either Noct tree:

```sh
plan/ws008-noct/tests/noct-p001-host-acceptance.sh \
  --mode parity --require-parity-changes
```

Logs, ELF reports, artifact hashes, and `noct-path-parity.tsv` are written to a
new directory below `plan/ws008-noct/temp/` by default. Override the canonical
and integration trees with `NOCT_OFFICIAL_DIR` and `NOCT_INTEGRATION_DIR`.
This runner does not perform the guest smoke; the QEMU portion of `NOCT-T003`
remains a separate acceptance record.

### Phase 001 QEMU runner

[`qemu-noct-smoke.sh`](./qemu-noct-smoke.sh) copies `config.mk` to an ignored
evidence directory, requires the copied configuration to select amd64/PC-AT,
adds only `noct` and `cksum`, and runs `make -j16` with that private config. It
boots a disposable copy of `build/amd64/hdd-image.img` and runs:

```sh
/usr/bin/noct -j0 -e 'print("NOCT-P001-SMOKE")'
```

The runner requires exact CMake/package/staging SHA-256 identity and compares
the host POSIX checksum with `cksum /usr/bin/noct` inside the booted image. It
also records source status, config/image hashes, QEMU commands and version,
timestamps, expected output/status, and a fatal-log scan. Run it with no
argument for a fresh ignored evidence directory:

```sh
plan/ws008-noct/tests/qemu-noct-smoke.sh
```

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

### Phase 002 host runners

The canonical Noct tree contains the reusable sanitized evdev state and wiring
runner. From the integration checkout, run:

```sh
userland/noct/NoctLang/tests/testcases/run-beui-zedbsd.sh /home/awe/zedBSD
```

It compiles the production state engine with `-Wall -Wextra -Werror` and
ASan/UBSan, then covers capability-derived roles, key/button state, relative
and absolute motion, packet boundaries, `SYN_DROPPED`, resynchronization,
detach, cleanup, and the legacy-console source audit. The existing canonical
`run-beui.sh` suite remains the regression gate for the generic core, PC-98
GDC, PC-98 Cirrus, and SDL2 dummy backends.

### Phase 002 QEMU runner

[`qemu-beui-zedbsd.sh`](./qemu-beui-zedbsd.sh) creates a private amd64 config,
builds with `make -j16`, boots a disposable disk copy with
`qemu-system-x86_64`, injects Shift and pointer activity through the QEMU
monitor, and checks the result through the debug console. It also captures a
PPM screendump and validates deterministic background, pattern, line/glyph,
and BMP pixels:

```sh
plan/ws008-noct/tests/qemu-beui-zedbsd.sh
```

The completed `q020` result is summarized in
[`q020-p002-beui-evidence.md`](./q020-p002-beui-evidence.md). The runner's raw
logs, disk copy, screenshot, and generated binaries remain under an ignored
`plan/ws008-noct/temp/q020-p002-beui.XXXXXX/` directory and may be deleted; the
checked-in runner and summary are the durable test contract.

## JIT tests

| ID | Phase | Contract |
| --- | --- | --- |
| `NOCT-T020` | p003 | Direct guest anonymous RW mapping, RX transition, native execution, invalid-case checks, and unmap |
| `NOCT-T021` | p003 | Forced canonical Noct JIT result plus positive `noct-jit: ...: compiled` evidence and no fallback |
| `NOCT-T022` | p003 | Interpreter negative control and second clean forced-JIT lifecycle |

### Phase 003 host and QEMU runners

The canonical host regression gates cover successful JIT allocation and
publication, forced `mprotect`/`munmap` failures, invalid-entry suppression,
interpreter fallback safety, and the complete opt-in lifecycle record:

```sh
cd userland/noct/NoctLang
cmake --build --preset static --parallel 16
tests/test.sh jit-slab build-static
tests/test.sh jit-branch /home/awe/zedBSD/userland/noct/NoctLang/build-static/noct
NOCT=/home/awe/zedBSD/userland/noct/NoctLang/build-static/noct tests/test.sh cli
```

The JIT-disabled host build, MinGW x86-64 build, `zedbsd` preset build,
canonical host build/acceptance, and exact canonical/integration path parity
are additional p003 build gates.

[`qemu-noct-jit.sh`](./qemu-noct-jit.sh) builds the direct
[`noct-jit-vm-probe.c`](./noct-jit-vm-probe.c), installs the deterministic
[`noct-jit-qemu.noct`](./noct-jit-qemu.noct), and evaluates
`NOCT-T020`--`NOCT-T022` in one disposable amd64 QEMU image:

```sh
plan/ws008-noct/tests/qemu-noct-jit.sh
```

The completed `q020` result is summarized in
[`q020-p003-jit-evidence.md`](./q020-p003-jit-evidence.md). Raw metadata,
serial transcripts, and the disposable image live under an ignored
`plan/ws008-noct/temp/q020-p003-jit.XXXXXX/` directory and may be deleted; the
checked-in runner, fixtures, and summary are the durable test contract.

Every QEMU record names the CMake source revision/status, zedBSD configuration,
image path/hash, exact command line, QEMU version, start/end time, expected
markers, fatal scan, and exit classification. Use `qemu-system-x86_64` for the
amd64 runtime gates and disposable image copies for tests that may mutate the
guest filesystem.

## Maintainer-review correction tests

| ID | Phase | Contract |
| --- | --- | --- |
| `NOCT-T030` | p004 | Makefile-only source delivery clones the pinned official revision and is idempotent |
| `NOCT-T031` | p004 | One zedBSD BeUI source and one public target dispatcher remain; split input and CMake Platform files are absent |
| `NOCT-T032` | p004 | Clean static/zedBSD presets derive the target from `__ZEDBSD__` without Linux-host leakage |
| `NOCT-T033` | p004 | Merged BeUI host/sanitizer plus SDL/PC-98 regressions pass |
| `NOCT-T034` | p004 | All JIT backends retain Boolean failure propagation and amd64 RW-to-RX QEMU acceptance passes |

The p004 runner must begin from a disposable parent layout containing only the
tracked `userland/noct/Makefile`; it must not rely on a pre-existing submodule
or consume `.internal/`. Existing p001--p003 runners are reused after their
source-root defaults are updated to `userland/noct/NoctLang`.

[`noct-p004-review.sh`](./noct-p004-review.sh) implements the host portion. It
performs a fresh pinned checkout in an ignored disposable parent layout,
checks idempotence and negative repository/revision cases, rebuilds the static
and zedBSD presets, audits the merged source/dispatcher/target macros and
Boolean JIT boundary, and runs the zedBSD evdev plus JIT failure corpus:

```sh
plan/ws008-noct/tests/noct-p004-review.sh
```

`NOCT_TEST_REPOSITORY` may name a verified local mirror when the official
GitHub transport is unavailable. The SDL2 dummy-window gate and the existing
p001--p003 QEMU runners remain explicit companion tests.

## Independent platform implementation tests

| ID | Phase | Contract |
| --- | --- | --- |
| `NOCT-T040` | p005 | Shared dispatcher/backend sources are absent and each configured target selects exactly one platform-owned public registrar |
| `NOCT-T041` | p005 | Independent SDL2 implementation passes canonical static/generic/dummy-window behavior |
| `NOCT-T042` | p005 | Independent PC-98 implementation passes GDC and Cirrus regressions |
| `NOCT-T043` | p005 | Independent zedBSD implementation passes host sanitizer, evdev, and source audits |
| `NOCT-T044` | p005 | amd64 zedBSD BeUI QEMU acceptance passes from the newly published and pinned canonical revision |

The p005 runner may extend `noct-p004-review.sh` or add a focused companion,
but its assertions must describe independent platform ownership rather than
the p004 dispatcher/shared-backend layout. Existing p001--p003 QEMU runners
remain the runtime regression gates.

## Common build gate

Run `make -j16` after each implementation Phase. Do not use the aggregate
`make check` target.
