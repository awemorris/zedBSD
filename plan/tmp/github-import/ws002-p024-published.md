<!-- awesome-plan project=zedbsd record=ws002-p024 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase024/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# WS002-p024: retirement heap integrity

Date: 2026-09-09
Status: completed (q142); q141 heap/UHCI repair and current regression gates pass
Timebox: 90 active minutes
Parent: [WS002](https://github.com/awemorris/zedBSD/issues/3)
Related: [p021](https://github.com/awemorris/zedBSD/issues/63),
[WS006-p010 evidence](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase010-legacy-usb-root-recovery/progress.md)

## Outcome

Closure: [q142 lifecycle results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase021-missing-login-session-teardown/q142-results.md)
establish that the owner decrease occurred before any child, during bootstrap
retirement. Corrected exact comparisons pass on PCAT and PC98.

Identify and correct the first proved heap ownership or structural failure
behind q137's process-reaper stall. Do not equate this newer amd64 observation
with the historical PC98 invalid-free report without evidence connecting them.
This is necessary for the already-authorized USB/input and service acceptance.

## Procedure

1. Replay the existing paired HID/64 MiB root-I/O sequence on a disposable
   current-source image. Add a dedicated diagnostic QMP channel, and save
   registers, stacks, the bounded kernel heap bytes and allocator state at the
   first stall. Resolve addresses from that image's own kernel symbols.
2. Validate physical/free block links off-guest with target-derived layouts.
   Determine whether there is a cycle, overlap, foreign pointer, duplicate free,
   or simply a slow finite walk; do not infer corruption from one stack sample.
3. If required, add test-only bounded allocation/free provenance and integrity
   checkpoints under the existing heap lock. Capture first failure and caller
   before later damage obscures it. No allocation/log recursion inside the
   diagnostic, no production bypass of a failed free, no speculative VM rewrite.
   If the original workload does not recur, optionally run 128 test-only
   fork/exec/wait cycles alongside the same USB read and input sequence. Keep
   both original and stressed outcomes, and do not call a clean replay a fix.
4. Repair only the established cause, with a meaningful reproduction/regression
   covering the relevant allocation/reallocation/alignment/retirement ownership.
   Use ordinary and sanitizer host checks plus explicit three-platform builds.
5. Rerun paired and xHCI input/root-I/O, USB halt, and the applicable normal and
   missing-login lifecycle regressions. Only then return to Xzed GUI acceptance.

## Completion boundary

The established first failure has provenance and a regression that fails before
the correction and passes after it; original workload passes without weakened
oracles. Otherwise mark uncleared with the captured facts and exact next step.
p021 remains open for any unconnected historical invalid-free/TTY ownership
requirements. A timed-out probe is not a successful storage/GUI test.

## Resume execution: Daybreak delegation (user instruction, 2026-09-09)

On the next queued execution of this phase, delegate the bounded heap-integrity
investigation to a sub-agent using `gpt-daybreak-blue-latest`. The user explicitly
authorizes this delegation. Give the sub-agent this phase, the q137/q138 evidence
and maintained fixtures, and require first-failure provenance before any proposed
production correction. Preserve the procedure and completion boundary above.

The parent checks progress once every 3 minutes (180 seconds), using compact
status/results instead of repeatedly reading the full transcript. Avoid extra
polling between these checks to reduce coordination tokens; handle unsolicited
completion, failure or required-input messages when delivered. If a single wait
is limited to less than 180 seconds, maintain the 180-second status-check cadence
without requesting intermediate status. Continue independent work where possible,
while serializing shared-tree edits, builds and QEMU acceptance with the child.

The user's observation is that heap-integrity checking appeared to require
Daybreak. This is a model-selection/execution preference, not a verified finding
that authorization caused q138's outcome. q138 actually ended without reproducing
the first heap failure. Keep this phase `uncleared` until its existing evidence
requirements are met. If the requested model cannot start, record the actual
error and defer this investigation while continuing other ready phases; do not
invent a diagnosis or silently substitute a different model.

This documentation update does not itself start the sub-agent or reopen q138.
Schedule the bounded retry through a subsequent Queue under the standing
execution authorization.

### Daybreak read-only preparation after q139 started

The requested `gpt-daybreak-blue-latest` sub-agent successfully ran a bounded
read-only investigation in this session. It completed before the first scheduled
180-second parent status check and delivered its findings without polling.
This verifies that the requested delegation can start; it does not diagnose
previous model/authorization failures.

The next finite retry should first close two private diagnostic gaps:

1. The q138 lock-boundary validator covers physical links and used-byte totals,
   but not free-list-only corruption. Extend it with range-safe free-list
   membership/link/accounting checks and focused malformed-list host cases.
2. The trace monitor only captures when `kernel_heap_trace_failed` becomes
   nonzero. Add bounded observation of sustained heap traversal/lock hold even
   when that flag stays zero, preserving the distinction between corruption,
   a long finite walk, and an unrelated stall. A private pre-walk record and
   structurally justified traversal bound must not silently skip a failed free
   or become a production timeout policy.

Run a fixed small number of original paired replays before optional 128-cycle
retirement churn; retain separate outcomes because instrumentation changes
scheduling. Production ownership changes still require first-failure evidence.
The read-only review found no demonstrated double-owner defect in current
vmspace queue detachment or amd64 detached-table release. p021 remains separate.

### Next bounded retry contract

Timebox: 90 active minutes. Execute with the Daybreak sub-agent after q139's
shared-tree build/runtime work is quiescent and this retry enters a new Queue.
The parent owns Queue/M/W updates; the child may implement this phase's private
diagnostics, maintained fixtures and evidence log. It must not edit NVMe code
or start unrelated WS work.

- Keep new kernel instrumentation behind `ZEDBSD_KERNEL_HEAP_TRACE`. Validate
  free-list links only after proving their addresses are physical block headers;
  check cycles, both directions, exact free-block membership and counts. Bound
  all diagnostic work by the finite heap extent. Document any extra traversal
  cost; do not allocate or recursively log while holding the heap lock.
- Record target/caller before `pointer_block` traversal and use a heap-extent
  derived structural bound in private builds. A bound failure stops/captures;
  it must not pretend an invalid free succeeded. Retain ordinary behavior when
  instrumentation is disabled.
- The monitor may capture a sustained traversal/lock-hold suspicion independently
  of the failure flag. Save repeated registers and trace sequence with target
  symbols, distinguishing watchdog suspicion from proven structural failure.
  Account for finite progress and idle/normal USB waiting; do not mark every
  quiet console interval a heap failure. The parent's three-minute coordination
  cadence is separate from this local diagnostic sampler.
- Add focused ordinary/sanitizer cases for healthy coalescing/alignment and
  free-list cycles, foreign/fabricated headers, wrong reverse links and missing
  membership, plus the bounded walk path. Complete explicit platform builds
  appropriate to the conditional source changes.
- Run at most two original paired traced campaigns, then at most one existing
  128-cycle churn campaign if both remain clean and the timebox permits. Stop
  repeated replay when first-failure evidence is available and decode it. A
  proved defect can receive a bounded correction/regression in this phase;
  otherwise return `uncleared` with diagnostic facts and a concrete next step.

## q141: exact writer and ownership correction

q140 reproduced structural corruption: next-header capacity at request+224
contains the adjacent live 216-byte UHCI request pointer. Three stopped heaps
and the trace agree; detection is at kern_free entry in the UHCI DMA retirement
path. Allocation reuse is proved, the exact store is not. See progress.md.

Timebox: 90 active minutes. Daybreak remains the execution model; parent checks
progress every three minutes. Use a target data watchpoint through QEMU's GDB
stub or an equally precise private provenance mechanism. Derive addresses and
object/header layouts from the current image and allocation, not a hard-coded
address from q140. Distinguish legitimate allocator writes from the corrupting
store, recording CPU, writer RIP/call chain and request/URB/DMA ownership. If
a stale list owner is suspected, prove its allocation/retirement generation.

Allow at most three bounded attempts to capture the writer, stopping replay
once exact evidence exists. Then add a regression that exposes the proved
ownership failure, correct its narrow owner and verify ordinary/sanitizer host
cases plus explicit supported builds and original paired/xHCI I/O. Repair stale
focused fixture paths if required to exercise the real current code. No private
watchpoint/provenance hook is enabled in ordinary production builds. Document
any observer-induced timing changes and preserve pre-fix evidence.

Production scope is the proved UHCI/DMA/allocator ownership path only. Do not
weaken retirement/quiescence or suppress a failed free. If the writer remains
unproved, retain uncleared with a concrete next capture step and move to ready
Priority work under the standing autonomous instruction.
