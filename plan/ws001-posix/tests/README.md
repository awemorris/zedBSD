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
