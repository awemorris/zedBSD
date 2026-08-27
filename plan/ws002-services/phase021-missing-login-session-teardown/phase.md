# ws002-p021: missing-login session teardown and crash-loop robustness

Last updated: 2026-08-27

WSID: `ws002`

Phase ID: `p021`

Combined ID: `ws002-p021`

Status: Planned corrective; non-blocking

Parent WS: [WS002](../ws.md)

Related baseline: [ws002-p014](../phase014-sessions/phase.md)

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
