# WS002 Phase 022: intermittent console-login progress

Last updated: 2026-08-31

WSID: `ws002`

Phase ID: `p022`

Combined ID: `ws002-p022`

Status: Complete; `BUG-002` resolved

Parent: [WS002](../ws.md)

## Objective

Find and repair the intermittent boot path in which PID 1 reports
`getty_console` started and `init: system running`, but `/bin/login` never
prints its prompt. Make progress from supervised getty spawn through console
open, terminal/session acquisition, utmp publication, and login exec
deterministic without weakening any existing exact-login acceptance oracle.

## Trigger

The symptom appeared in three q036 Variant cells. A later uninterrupted q047
six-cell run passed, so `ws020-p003` remained complete without retrying a
failed cell. It recurred in the single no-retry `ws020-p006` relocated-GPT run
after USB storage, GPT selection, both partitions, UUID resolution, root/data
overlay, swap, `getty_console` spawn, and `init: system running` had all
succeeded. GPT selection was therefore outside the observed blocking boundary,
but the asynchronous USB completion path remained in scope until diagnosed.

## Work

1. Freeze the failed p006 log, image identities, QEMU topology, timeout, and
   the last observed service markers before changing code.
2. Add bounded temporary checkpoints sufficient to distinguish scheduler
   dispatch, `/dev/console` open, termios setup, `setsid`, `TIOCSCTTY`, fd
   duplication, utmp update/fsync, and `/bin/login` exec. Instrument kernel
   wait/ownership state only if userland checkpoints cannot identify the wait.
3. Reproduce from one fresh disposable copy of the frozen topology. Do not
   rerun an unchanged failed cell until it passes and do not count diagnostic
   runs as acceptance.
4. Repair the smallest demonstrated ownership, wakeup, locking, I/O, or
   lifecycle defect. Preserve PID 1 supervision, foreground getty operation,
   controlling-terminal rules, overlay durability, and exact service output.
5. Remove temporary checkpoints or reduce any retained message to a stable,
   bounded failure diagnostic. Add a focused regression at the lowest layer
   that can deterministically express the defect.
6. Run the focused test normally and with applicable sanitizers/analyzer,
   build amd64 with `make -j16`, then perform one ordinary boot as the initial
   post-fix check. Only after it succeeds, run a final five-consecutive-boot
   QEMU campaign from fresh disposable copies of one frozen image.
7. Rerun `MAC-T022` once with its unchanged exact root/data/swap/init/login
   oracle. Preserve source hashes and failure logs; do not use aggregate
   `make check` or `.internal/`.

## Result

The defect was a same-CPU interrupt self-wait in the USB core's submit-commit
handoff, not a getty, login, utmp, GPT, or missing xHCI completion defect.

- A bounded, unchanged-image silent campaign reproduced the stop on run 3.
  PID 1 had completed its spawn path and getty PID 9 had run. The last BOT
  command was `WRITE(10)` tag `0x733`, LBA `0x29638`, eight blocks; its data
  stage was complete and the transport was submitting the 13-byte CSW
  bulk-IN stage.
- xHCI had matched the CSW transfer event for slot 1/DCI 3 with completion
  code 1 and zero residual. The URB was still `PENDING`, no unmatched event
  had occurred, and the CPU was in `sched_yield()`. Thus hardware completion
  had arrived but USB terminal publication had not finished.
- `drv_usb_urb_submit()` used an atomic exchange to claim its stack-resident
  `usb_submit_commit`, then cleared `submit_commit_pending` in
  `submit_commit_finish()`. A local xHCI completion interrupt in that short
  interval saw the already-cleared pointer and waited for
  `submit_commit_pending`. On one CPU it had preempted the only submitter able
  to clear that flag, so both sides waited forever.
- The repair masks local IRQs only while the submitter claims and finishes
  that commit record. A remote CPU may still win and finish the record; the
  existing wait remains after local IRQ restoration for that SMP case.
- Inspection of the exact failed p006 objects also proved that the p033
  console serialization and the non-TLS static `utmp_result` repair were
  already present. They were therefore excluded as causes of this recurrence.

The deterministic regression
`tests/usb-submit-commit-handoff-test.c` models the old self-wait and the
IRQ-masked handoff, then binds the model to the production ordering. The fixed
source passes; a generated old-order negative control is rejected. The broader
USB binding transaction fixture passes normally, under ASan/UBSan, and under
the compiler analyzer (971 checks in each executable run).

Runtime evidence is retained under `../temp/`:

- `p022-silent-campaign-runs/run-003/`: classified failure and stopped-QEMU
  register/checkpoint capture;
- `p022-focused-handoff/`: deterministic fixed and old-order negative-control
  results;
- `p022-repair-focused-001/`: one instrumented post-fix `MAC-T022` pass;
- `p022-repair-ordinary-initial/`: ordinary, instrumentation-free `MAC-T022`
  pass with its unchanged exact root/data/swap/init/login oracle; and
- `p022-repair-final-five/summary.tsv`: five fresh-copy ordinary boots, all
  `pass` with an independently checked exact `login:` prompt.

All temporary source checkpoints, diagnostic globals, and the temporary
Makefile switch were removed before ordinary acceptance. No failed acceptance
cell was retried into a pass.

## Completion conditions

Met. The exact blocking stage and root cause are recorded; the deterministic
negative control rejects the former ordering; the ordinary build and focused
static gates pass; one initial boot followed by five final fresh-copy boots
reaches exact `login:` without a stall; and the unchanged `MAC-T022` oracle
passes once. No temporary source instrumentation remains and `BUG-002` is
closed.
