# Queue: isolated target formatter acceptance

Last updated: 2026-09-06

QID: `q079`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q078](queue-q078.md)

Authorization: the recorded standing instruction authorizes successive finite
Queues. Q078 exhausted its four launches and was closed uncleared with the
diagnosed concurrent host rebuild. This Queue was presented before execution.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws019-p008](ws019-installation/phase008-target-mkfs/phase.md) | completed | Close UFS1 target/persistence acceptance with stable host inputs |
| 2 | [ws019-p009](ws019-installation/phase009-target-mkswap/phase.md) | completed | Close ZEDSWAP2 target activation/boot acceptance in the same cell |

## Finite budget and procedure

One new amd64 QEMU launch, 120-second boot/command waits and a 600-second
whole-cell limit. Review after twenty active minutes. No further correction
launch is part of q079. Use fresh disposable copies and the existing combined
formatter fixture; all production formatter/kernel code is unchanged from the
last q078 guest success.

Complete all builds first. Target selection uses `ZEDBSD_CONFIG`, never the
ignored `MACHINE` variable. PC/AT and PC-98 builds are sequential. Do not build
or otherwise modify source/fixture images while acceptance is running.
Capture and compare the production image hash, GPT/FAT boot/sentinel bytes,
and generated file hashes. The runner now records postprocessing failures
in result.json as well as guest failures.

The same cell checks format success/refusals, 16,383-slot swap activation and
deactivation, generated overlay/swap startup, and persistent writes across
two guest reboots. It begins from a native fixture root and host-allocated
all-zero input files. It does not test installer staging-file creation speed.

Record [q079 results](ws019-installation/tests/q079-results.md) and synchronize
P/W/M before closure. Do not use `make check`, `.internal/`, or real media.

## Closure

The one permitted launch passed all guest and host guards; p008/p009 are
completed. [Q079 results](ws019-installation/tests/q079-results.md) record
commands, final host/build evidence, generated image identities and limits.
P004 still needs its documented provenance/publication prerequisite contracts.
No further QEMU launch is selected by this finished Queue.
