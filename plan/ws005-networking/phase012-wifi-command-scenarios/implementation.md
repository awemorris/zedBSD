# Q085 implementation checkpoint

Current result: P048 stop integration and the full P012 acceptance campaign are
complete. All 30 ordinary/sanitized stories and maintained regressions pass; see
[results.md](results.md). The checkpoint below is retained as implementation
history before the user released acceptance, not a current instruction to stop.

Status: structural rewrite and second external-review corrections implemented;
stopped before acceptance on 2026-09-06. The user authorized source corrections
after supplying `net-wifi-report-2.md`; acceptance remains pending.

The user moved the saved 30 stories after the structural repair. No story has
been reported as passing. Earlier primitive checks preceded the broad rewrite
and are not evidence that the current integrated implementation is accepted.

| Report direction | Implemented structural correction |
| --- | --- |
| Client/server deadlines | Shared request budgets, absolute timed frame reads, one actor work context, subordinate child/DHCP deadlines, reserved cleanup interval |
| Excess connect records | Machine selection announcement emitted once; direct nonblocking list preserved |
| EOF before reap | Short bounded reap polling even after pipe EOF; actual EOF-then-hang still governed by deadline |
| Explicit connect during recovery | Profile/topology preflight, then retirement of any retained identity before the new connection transaction |
| Cleanup pins the policy | Explicit retiring state and target; no carrier recovery of retiring work; release externally replaced claims; retain and retry real OS failures |
| Partial radio failure | Per-radio preparation and list outcomes, healthy partial output retained with diagnostic/status space reserved, failed scans isolated |
| Child wait lies about success | Non-EINTR wait errors retained; operation timeout remains ETIMEDOUT; no success inferred from an unwritten wait status |
| Background work blocks control | Read-only cached observations and notifications serviced in child waits; authorized background stop requests deferred until checked cancellation cleanup |

Further consistency changes: asynchronous enable intent, idempotent reuse of a
healthy owned connection, reuse of running scans, no recursive event-triggered
child work, one L2/DHCP failure exit, pre-DHCP baseline retained for partial-failure
reconciliation, removed-device cleanup never targeting a replacement interface,
and frozen/fair candidate waves under the four-attempt bound.

The second review led to explicit oversized/unowned resolver snapshots,
retirement backoff without abandonment, nonblocking partial control input,
responsive wired SHOW and deferred wired mutations through the common admission
path, corresponding wired response budgets, common fresh/cached list metadata,
explicit output truncation, a bounded dynamic observation cache and smaller
observation/conversion stack frames. The enable help documents asynchronous
association. See [review dispositions and objections](review2-response.md).

The current sources pass serialized `make -j16` with each of
`ZEDBSD_CONFIG=config/ci/config-amd64.mk`,
`ZEDBSD_CONFIG=config/ci/config-pcat.mk`, and
`ZEDBSD_CONFIG=config/ci/config-pc98.mk` (all exit 0). Disposable logs are under
`plan/ws005-networking/temp/q085-wifi-scenarios/review2-*-build.log`.
These are the final three configured builds after the second-review corrections.
An intermediate amd64 compilation caught two char/unsigned-char pointer warnings
in the new output adapter; those were corrected before all final builds passed.
`git diff --check` also passes. No test suite was run in this review cycle.

The audit target is the uncommitted working-tree diff from `a3f1ea3`, principally
`userland/base/networkd/main.c`, `managed-wlan.c/.h`, `wifi-child.c/.h`,
`userland/base/net/main.c`, `protocol.c/.h`, and `userland/base/wifi/main.c`.
See `rewrite.md` for the intended lifecycle and deadline contracts. The supplied
`plan/net-wifi-report.md` is preserved unchanged.

No post-rewrite acceptance scenario has run. The harness boundary is designed
in `acceptance-boundaries.md` but the 30-story harness is not implemented yet.
Earlier primitive fixtures remain in the diff; some still need adaptation to
the rewritten private selector and retirement interfaces. Their earlier passes
do not establish current regression success. No new runtime or RF test was run
at this checkpoint, and no commit was made.

Next: await the user's instruction to resume acceptance or further review.
The subsequent driver-review implementation is owned by
[WS004 p047](../../ws004-hardware/phase047-wlan-driver-lifecycle-review/phase.md),
with [per-finding dispositions](../../ws004-hardware/phase047-wlan-driver-lifecycle-review/review-response.md).
Then adapt and execute the 30 acceptance stories and focused regressions,
including the second-review boundary cases. Q/P/W/M remain in progress; this is
an implementation checkpoint, not phase completion or acceptance.
