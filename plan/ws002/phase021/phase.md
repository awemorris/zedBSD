# ws002-p021: missing-login session teardown and crash-loop robustness

Last updated: 2026-09-09

Current decision: **cleared / completed by user acceptance, 2026-09-09**.
[Closure](closure.md) supersedes the historical provenance requirement below.
The old failure remains BUG-012 as likely already corrected, cause unproven.

## q142 bounded baseline diagnosis

Result: [q142 results](q142-results.md). Current gates pass on both platforms;
historical invalid-free provenance remains uncleared.

q141 observed thread/task owners 9 to 8 and a 16 KiB stack decrease between
the pre-child baseline and the first completed getty. q137 introduced a detached
boot worker that starts PID 1 before returning. First observe owners and PID 0
thread count without creating any child. Require 25 consecutive equal samples
20 ms apart, with a 5 second overall retry bound, before fixing the baseline.
Log every transition. This is a measured quiescence condition, not a permitted
per-child decrement: all 100 child comparisons must still match exactly.
Run the maintained fixture on both PCAT and PC98. Preserve historical invalid
free provenance as uncleared unless independently established.

WSID: `ws002`

Phase ID: `p021`

Combined ID: `ws002-p021`

Status: completed (user acceptance; current runtime verified, historical cause unproven)

Parent WS: [WS002](../ws.md)

Related baseline: [ws002-p014](../phase014/phase.md)

## Objective

Make the failed-session path safe when `getty` cannot execute `/bin/login`.
The service must exit, be reaped, and obey init's bounded crash-loop policy
without corrupting console, file-descriptor, task, thread, or VM ownership.

## Origin and current boundary

BR-T46 development accidentally produced a PC-98 root image without
`/bin/login`. One run printed the expected `execve()` `ENOENT` diagnostic and
then reached `fatal: src/kern/entry.c:194: invalid kernel allocation free`
during child exit/reap. An immediate run of the same missing-login condition
reaped all six prescribed respawns without the fatal, so this is a
layout/timing-dependent residual rather than the cause of the normal PC-98
boot failure. Adding the required `login` program restored the production
login path and leaves q015 acceptance unblocked.

The narrow known boundary is after the failed `execve()` copy has already
been freed and before child retirement is complete:

1. getty returns through `_exit`;
2. `process_exit_cleanup()` detaches the controlling terminal and destroys
   descriptors/cwd ownership;
3. scheduler retirement destroys the i386 HAL task/kernel stack and deferred
   VM space; and
4. PID 1 completes `wait`/reap and decides whether to respawn.

The leading hypothesis is getty-specific console/controlling-TTY teardown
when a normally small object falls back to a page-backed kernel allocation.
Thread/HAL-task/VM retirement remains the second candidate. Do not change
either lifecycle based only on that hypothesis.

## Work packages

1. Add a reusable installed-image fixture which intentionally omits
   `/bin/login` while retaining init, getty, and the console.
2. Add test-only allocation provenance/call-site diagnostics and identify the
   first foreign or duplicate page-backed free.
3. Count process, thread, VM space, file table, HAL task, console open, and
   controlling-TTY ownership across every failed session.
4. Repair the proven ownership defect without weakening checked teardown,
   retained-resource, or crash-loop behavior.
5. Run the missing-login fixture repeatedly on PC-98 and on an i386 PC/AT
   counterpart, then regress the normal installed login/logout/respawn path.

## Completion conditions

- the first invalid free has exact allocation/free provenance;
- every failed exec/exit/wait cycle returns all tracked ownership counts to
  baseline;
- init performs no more than the configured six rapid respawns and then
  remains alive without a fatal, hang, or unbounded prompt loop;
- repeated PC-98 and i386 PC/AT missing-login fixtures pass;
- the normal image still reaches login, supports session exit, and respawns
  getty; and
- applicable `KERN-WAIT-01`, `KERN-PTY-01`, `KERN-BOOT-01`, and
  `SVC-GETTY-01` ledger entries link the final evidence.

## Reconsideration boundary

Stop before changing the console/TTY ownership model, scheduler retirement,
or HAL task ABI unless allocation provenance proves that subsystem owns the
first invalid free. A one-off failure without provenance is not authority for
a speculative lifecycle rewrite.

## q136 execution

Use freshly built q135 PC98/PCAT ordinary images and disposable rootfs copies
with /bin/login absent. First confirm current failure/restart behavior without
changing lifetime code. Capture screen/console and monitor CPU/process state.
If the old invalid free recurs, add bounded allocation provenance diagnostics
and fix only the proved owner. If absent, retain the unproven accounting and
provenance gates; a finite runtime smoke does not establish all completion
conditions. No aggregate make check; serial runtime/builds and make -j16.

## q136 result

[Current runtime/accounting evidence and exact resume condition](results.md).
No old invalid free recurred; do not infer a production correction.
