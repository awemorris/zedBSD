# WS001 shared test index

Parent: [WS001](../ws.md)

This directory owns the POSIX workstream's test-case catalog. Executable tests
remain under repository `/tests` because the top-level Makefile and tooling use
those paths.

| Phase(s) | Test cases / executable evidence |
| --- | --- |
| `ws001-p000` | `tests/posix-2024-utilities.csv`, matrix/import/check tooling, build/provenance gates |
| `ws001-p001` | `tests/test-deferred-stubs-host.sh`, `tests/deferred-stub-rootfs-test.py`, `tests/deferred-stub-qemu-test.py` and fixtures |
| `ws001-p002` | `tests/test-posix-phase2a-host.sh`, `phase2b`, `phase2c-priority`, shell-builtin host tests, `tests/posix-phase2-qemu-test.py` and fixtures |
| `ws001-p003` | locale/gencat/localedef host tests, catalog/locale fixtures, `tests/posix-phase3-qemu-test.py` |
| `ws001-p004` | `test-posix-{bc,ed,find,m4,pax}-host.sh`, Phase 4 fixtures, `tests/posix-phase4-qemu-test.py` |
| `ws001-p005` | credential/priority/IPC focused tests and `tests/posix-phase5-qemu-test.py` |
| `ws001-p006` | `tests/test-posix-development-host.sh`, development fixtures, `tests/posix-phase6-qemu-test.py` |
| `ws001-p007` | `tests/test-posix-compress-host.sh`, compression fixture, `tests/posix-phase7-qemu-test.py` |
| `ws001-p008` | `tests/test-posix-sccs-host.sh`, SCCS fixtures, `tests/posix-phase8-qemu-test.py` |
| `ws001-p085` | terminal stack/tools host tests, curses/terminfo fixtures, `tests/posix-phase85-qemu-test.py` |
| `ws001-p009` | utility matrix check, source/test audit, format/provenance review evidence recorded in the Phase report |
| `ws001-p010` | `tests/test-phase10-local-source.sh`, local bc/ed/m4 host tests, standalone installs, top build, Phase 10 QEMU target |
| `ws001-p014` | `shell-job-control-test.sh` plus instrumented shell hooks/PTY probe; `qemu-shell-job-control.sh` against the installed amd64 `/bin/sh` |
| `ws001-p015` | `base-c-style-audit-test.sh`, `base-c-style-inventory.sh`, and `run-base-command-host-test.sh` |
| `ws001-p016` | `run-lpd-host-test.sh` plus `fake-lpd-test.py`; `qemu-base-utility-smoke.sh` proves the native guest binary/error path while guest networking remains a handoff |
| `ws001-p017` | `run-cmp-host-test.sh` and the shared `qemu-base-utility-smoke.sh` native guest cell |
| `ws001-p018` | `run-tee-host-test.sh` and the shared `qemu-base-utility-smoke.sh` native guest cell |
| `ws001-p019` | `userland-file-header-audit.py`, `userland-file-header-audit-test.sh`, `migrate-userland-file-headers.py`, the deterministic 269-file inventory and body hashes, assembler preprocessing, and the configured build gate |
| `ws001-p020` | `userland-c-style-audit.py`, `userland-c-style-audit-test.sh`, `userland-c-body-audit.py`, three idempotent refactoring tools, the deterministic 258-file review ledger, Phase 19 header regression, declaration-after-statement compiler gate, and configured build; Phase is honestly uncleared by recorded body/semantic residuals |
| `ws001-p021` | `refactor-userland-ansi-c.py`, `refactor-userland-semantic-layout.py`, the extended zero-residual body audit and `userland-c-body-audit-test.sh`, the 269-file header gate, structural/header fixtures, the 258-row review ledger, configured `make -j16`, and whitespace validation; q005 adds loop-preparation paragraphs, mandatory comment gaps, decision/return comments, and explicit call results across all implementations |

When a new Phase fixes a ledger item, add its normative case, failure case,
executable path, and environment here before marking the row reviewed.

## ws001-p011 basename

`basename-test.sh` covers empty, all-slash, double-slash, trailing-slash,
suffix-equal, suffix-removal-to-empty, `--`, usage, and output-failure cases:

```sh
sh plan/ws001-posix/tests/basename-test.sh
make -j16 build/amd64/bin/basename
```

## ws001-p012 dirname

`dirname-test.sh` covers empty/no-slash, root/all-slash/double-slash, repeated
and trailing slashes, a long operand, `--`, usage, and output failure:

```sh
sh plan/ws001-posix/tests/dirname-test.sh
make -j16 build/amd64/bin/dirname
```

## ws001-p013 link and unlink

```sh
sh plan/ws001-posix/tests/link-unlink-test.sh
make -j16 build/amd64/bin/link build/amd64/bin/unlink
```

## ws001-p014 shell foreground job-control ordering

[`shell-job-control-test.sh`](./shell-job-control-test.sh) builds a host copy of
the production shell with Phase-owned linker wrappers; production shell source
is not replaced or patched by the fixture. On each pipeline `fork()`, the
parent is held until its child reports either entry into the pre-execution gate
or the erroneous arrival at `posix_spawn()`. The test therefore proves that
both children are waiting before the TTY handoff and that the two-byte gate
release occurs afterward, without relying on a favorable scheduler race.

The second case runs a stopped terminal-reading helper through `fg`. Its event
trace requires the last successful operation before `SIGCONT` to be
`tcsetpgrp()` for that job and requires the final handoff to restore the shell.
An injected first-`fg` handoff failure must emit no `SIGCONT`, retain the same
stopped job, and allow a second `fg` to hand off, continue, complete, and
restore the shell.
The external probe independently records `tcgetpgrp(0) == getpgrp()` immediately
before each first terminal read:

```sh
plan/ws001-posix/tests/shell-job-control-test.sh
```

The same runner also makes a background probe attempt a real terminal read,
observes its Linux host state become stopped before issuing `fg`, and verifies
that it never owned the TTY while backgrounded. A non-TTY pipeline must show
two direct execution checkpoints and no gate release or `tcsetpgrp()` event.
Finally, one-shot failures are injected independently into pipeline
`tcsetpgrp()`, gate release, and the parent wait; after each failure the same
shell must run a second foreground pipeline, regain its TTY, and leave no
fixture child or spawned command alive.

An optional new output-directory argument retains compiler logs, PTY
transcripts, call-order traces, and process-group checkpoints. Default evidence
is written below the ignored `plan/ws001-posix/temp/` directory. The test does
not use the aggregate `make check` target or repository `.internal/` material.

[`qemu-shell-job-control.sh`](./qemu-shell-job-control.sh) boots a disposable
copy of `build/amd64/hdd-image.img` with `qemu-system-x86_64` and exercises the
installed `/bin/sh`. It covers a terminal-reading foreground pipeline, a
direct Ctrl-Z/`fg` cycle, a background terminal reader followed by `fg`, an
inner non-TTY shell, final prompt usability, fatal diagnostics, and production
image integrity:

```sh
plan/ws001-posix/tests/qemu-shell-job-control.sh
```

Its optional output-directory argument must name a new path. The default is a
new ignored directory below `plan/ws001-posix/temp/`; only a disposable image
copy is writable.
