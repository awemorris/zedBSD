# WS002 shared test index

Parent: [WS002](../ws.md)

Executable system tests remain under repository `/tests`; this file assigns
their planning ownership and shared acceptance role.

| Phase(s) | Test cases / executable evidence |
| --- | --- |
| `ws002-p011`–`p018` | Focused host/parser tests and component targets recorded in the legacy Phase 11–19 plan |
| `ws002-p019` | `tests/phase19-qemu-test.py`, `phase19-rc.conf`, `phase19-service`, `phase19-smoke.sh` |
| `ws002-p020` | `tests/phase20-contract-host-test.py`, `phase20-qemu-test.py`, `phase20-interactive-shell-qemu-test.py`, rc.conf/service/smoke fixtures |
| `ws002-p022` | `usb-submit-commit-handoff-test.c` old-order executable model plus production source-order gate; unchanged `MAC-T022`; ordinary initial plus five fresh-copy exact-login boots |
| Shared DHCP/network logic | `tests/dhcp-host-test.c`, `dns-host-test.c`, `inet-stack-host-test.c`, `net-device-host-test.c`, `net-sync-host-stubs.c` |
| Shell support | `tests/sh-*-host-test.c` and the installed interactive QEMU scenario owned by `ws002-p020` |

Future WS002 maintenance Phases add cases here. WLAN, physical networking, and
expanded DHCP lifecycle cases belong primarily to WS005 and may reference the
same repository executable where ownership is stated explicitly.

## USB boot halt (q135 / p023)

`run-usb-shutdown-host.sh` builds the current full USB core and consolidated
I/O source with the retained-root/failure/retry model, ordinary and sanitizers.
The storage worker cases are in WS004 `run-usb-storage-no-media-test.sh`.

Build ordinary amd64 with `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk`,
then the reused command wrapper fixture with
`make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk -f Makefile -f plan/ws019-installation/tests/installer-qemu.mk ws019-installer-qemu-fixture`.
Run `python3 plan/ws002-services/tests/run-usb-halt-qemu.py plan/ws002-services/temp/NEW`,
add `--dirty` for write/halt/next-boot/reboot persistence or `--cpus 1 --no-swap`
for UP/no-swap. Each run requires a fresh output directory. Images and UEFI
variables are disposable copies. QMP requires all selected CPUs HLT=1, IF=0
in three consecutive samples, and rejects USB errors and fatal diagnostics.
The helper is not an installer test or a hardware acceptance substitute.

## Missing login / p021 (q136)

`run-missing-login-qemu.py OUTPUT --platform pcat|pc98` validates and modifies
only disposable UFS/media copies, observes three independent ordinary-init
boots with six getty failures each. `--normal --runs 1` instead verifies three
normal login/pwd/logout/respawn cycles. All output directories must be new.

Build `missing-login-lifecycle.c` with
`make -j16 ZEDBSD_CONFIG=config/ci/config-pcat.mk -f Makefile -f plan/ws002-services/tests/missing-login-lifecycle.mk ws002-lifecycle-fixture`
(or pc98), then use `--lifecycle build/pcat/ws002-lifecycle.elf --runs 1`.
This test-only PID 1 exercises 100 real getty exec failures and owner snapshots;
ordinary images never install it. Explicitly prepared getty/utmp read caches
are separated from per-session owners. See p021 results for evidence and the
historical invalid-free provenance which remains unproven.
