# q138 progress

Status: in-progress, 2026-09-09.

`temp/q138-paired` repeats the original workload with a second diagnostic QMP
channel and three stopped snapshots after a quiet read interval. The entire
campaign completes (exit 0, wrapper session 87955 terminal 0). All 534 physical
blocks and both allocation/free-list accounting checks are consistent in each
captured heap. CPU0 alternates between USB transfer waiting and ordinary idle.
This is a passing diagnostic replay, not proof that q137's intermittent heap
walk is fixed, nor an unstopped acceptance replacement.

Private `ZEDBSD_KERNEL_HEAP_TRACE` instrumentation adds a linear physical-link
check at the shared kernel/libc lock entry and exit, plus allocation/free and
caller records under that lock. A failure stores its event and stops CPUs with
no recursive allocation/logging; production builds do not enable it. The first
private trace build runs in `temp/q138-trace`. Its ring array was optimized away
because it was not volatile and had no C reader; symbol inspection caught this
fixture defect. The checker and failure flag exist, but this run cannot supply
the intended full ring provenance. Correct the ring visibility after its runtime
finishes, keeping this failed instrumentation attempt distinct from acceptance.

The first trace run ended (session 62568, exit 1). Its storage read completed,
but the absolute-input probe lacked its final PASS marker. The campaign's final
oracle correctly rejected it, despite an intermediate shell helper overwriting
the probe-wait error with a subsequent successful prompt wait; that helper now
returns the first failure explicitly. The diagnostic wrapper separately reports
the missing ring symbol. A read-only sample of the failure flag was zero; do not
label the absolute-input failure a heap diagnosis.

The trace array is now volatile so QMP can observe all 2048 target records.
Invalid-free error counts also trigger the private stop, in addition to physical
link corruption. The focused checker fixture passes ordinary and ASan/UBSan
healthy/aligned/coalescing cases and rejects cycles, foreign links and capacity
overflow. `temp/q138-trace2` runs the corrected private build; nm confirms the
81920-byte ring is present. No production acceptance build enables these checks.

`q138-trace2`'s guest campaign passes all original input/storage oracles without
a checker failure. Its diagnostic wrapper raced the normal QEMU quit and
reported BrokenPipeError (session 82422 exit 1); EOF/reset/broken-pipe at that
normal shutdown boundary now ends monitoring, preserving the campaign result.
No failing heap snapshot was collected, so no root-cause correction is claimed.

The guest record oracle also decremented its budget by the requested poll
timeout even when poll returned immediately. It now uses CLOCK_MONOTONIC for
the same real 15-second budget, avoiding early exhaustion by unrelated reports.
This corrects test timing, not the production event or timeout contract.

`temp/q138-stress` runs the corrected checker/ring, actual-time input oracle and
128 optional native test-only fork/exec/wait cycles alongside the unchanged
64 MiB USB read and pointer events. The original unstressed outcome is retained.

## Final q138 boundary

`temp/q138-stress` completes with campaign exit 0. Its source syntax, QEMU
topology, private trace-image build, paired runtime and input-integrity gates all
pass. The guest reports both `RETIREMENT PASS iterations=128` and the complete
storage, keyboard, relative, absolute and hotplug oracles. The trace monitor
observes no first-failure flag, so no stopped failure snapshot exists.

q138 is finished and p024 remains **uncleared**. Three increasingly diagnostic
replays did not reproduce q137's heap walk: one captured three structurally
valid 534-block heaps, and the final one stressed the exact process-retirement
path with the private validator enabled. No production ownership correction is
justified. Resume when the first-failure flag triggers, then decode the volatile
ring and captured heap with the maintained scripts. Preserve the q137 stack as
an intermittent lead and retain p021's independent historical requirements.

## q140 Daybreak retry

The private trace validator now checks the free list only after proving that
each address is a member of the already validated physical chain. It checks
reverse links, uniqueness and exact free-block membership/counts. Both the
physical and free-list work are bounded by the 512 KiB heap extent. The added
membership check is intentionally diagnostic and has worst-case quadratic cost
in the number of physical/free blocks; production builds do not enable it.

Private frees record event 12 with the target and caller before `pointer_block`.
That walk has an extent-derived step bound in trace builds, so a cycle becomes
an allocator error and triggers the existing first-failure stop rather than
looping forever. Ordinary allocator behavior is unchanged. The QMP monitor also
samples target-derived `pointer_block` and validator address ranges: four
samples spanning at least 1.5 seconds with an unchanged trace sequence capture
three stopped heaps/stacks as a watchdog suspicion, separately from a nonzero
structural-failure flag. Normal idle and USB wait PCs do not satisfy that rule.

`temp/q140-host` passes the focused validator normally and under ASan/UBSan.
Cases cover healthy alignment/coalescing, physical cycle/foreign/capacity,
free-list cycle, an out-of-heap fabricated header, wrong reverse link, missing
membership and the bounded pointer walk. Explicit `make -j16` builds pass with
`config/ci/config-amd64.mk`, `config/ci/config-pcat.mk` and
`config/ci/config-pc98.mk`. The maintained legacy-HCD host runner was also
attempted but is currently stale: it names removed `src/kern/io-stats.c`; it is
not claimed as validation and was not changed in this phase.

`temp/q140-trace1` passes the unchanged paired IN-T41 campaign (exit 0), with no
failure flag and no sustained-walk observation. Its private image contains the
81920-byte trace ring, the pre-walk hook, bounded `pointer_block`, and expanded
validator symbols.

`temp/q140-trace2` reproduces a first structural failure. The flag is event 5,
the entry check for `kern_free`, at trace sequence 87737; no watchdog suspicion
caused the capture. Three stopped snapshots are byte-stable. The offline heap
decoder finds 544 physical blocks and identifies the last header at
`0xffffffff804c4570`: its magic/state, predecessor and free-list links remain
plausible, but its capacity is the impossible pointer
`0xffffffff804c4498`, so its computed extent exceeds the heap. The unchanged
paired oracle fails at shell command 9/10 after the private all-CPU stop.

The overwritten value is the payload address of a live 216-byte UHCI request
immediately before the damaged header. The ring proves that address was first a
48-byte coherent-DMA allocation record freed by `drv_dma_free_coherent`
(sequences 86859-86861), then reused for the UHCI request allocated by
`uhci_urb_enqueue` (sequences 86873-86875). The failure is discovered on CPU1
while `drv_dma_free_coherent` runs from `uhci_request_free` <-
`uhci_finish_completion` <- `uhci_retirement_process`; the preceding successful
heap check is CPU2's 48-byte `kern_calloc` used by the user-copy pin path. Thus
the header overwrite occurred between checks 87736 and 87737. This connects the
heap damage to active paired UHCI retirement and explains how a later unrelated
free, including process retirement, can be the first observer. It does not yet
prove which CPU instruction stored the request pointer at request+224.

q140 stops after the second original campaign as required once first-failure
evidence exists. The optional 128-cycle churn replay is not run. No production
ownership correction is made because the exact stale owner/write is not yet
proved and no regression can distinguish a proposed repair. p024 remains
**uncleared**. The concrete next step is one bounded paired trace replay with a
target data watchpoint on the deterministic next-header capacity word after the
UHCI request allocation, or equivalent request-adjacent redzone provenance,
recording the writing RIP/CPU and request/URB/DMA identities. Then add the
smallest UHCI/DMA ownership regression that fails on that exact writer before
changing production code. The q137 amd64 process-reaper stack remains a later
observer of heap damage; it is still not the historical p021 PC98 invalid free.

## q141 exact writer and repair

`temp/q141-checkpoint1` reproduces the corruption on the first bounded capture
attempt. A private, non-allocating checkpoint immediately after each candidate
UHCI/DMA pointer store stops CPU0 at trace sequence 94564, event 102, directly
after `uhci_schedule_unlink_locked`. The captured request is
`0xffffffff8048e728`; its software predecessor is `0xffffffff804c3a48` and its
software successor is `0xffffffff804c3ab0`. The event-102 identity record at
sequence 94563 preserves the request and successor, while the stopped CPU0
stack/register record preserves all three addresses and the call chain through
`uhci_retirement_begin_locked`. All three snapshots are identical. The offline
decoder in `heap-analysis.json` finds 491 blocks and rejects the capacity word
written through the stale successor because it crosses the heap extent.

This proves the production ownership defect. Removing a non-head UHCI request
updated the predecessor QH's hardware link and the successor's reverse software
link, but did not update the predecessor's `schedule_next`. After the removed
request was freed and its address reused, a later unlink followed that stale
software edge and executed
`request->schedule_next->schedule_previous = request->schedule_previous`,
storing the request-valued predecessor into allocator metadata. The q140
request-plus-224 observation was therefore a reused stale `uhci_request` target,
not a DMA list write. The q137 process-reaper stack was a later observer.

`src/drivers/pci/pci-uhci.c` now assigns the predecessor's `schedule_next` to
the removed request's successor beside the existing hardware-link update. The
removed request and successor links continue to be cleared exactly as before;
retirement and invalid-free policy are unchanged. The temporary q141 pointer
store checkpoints were removed from source after capture. The retained private
heap validator's nested physical membership walk now also has its own
heap-extent-derived step bound.

The maintained legacy-HCD regression now extracts the production unlink body
and requires both predecessor updates. Its three-node behavioral case removes a
middle request and verifies the predecessor hardware successor, predecessor
software successor, successor reverse link, and removed-node clearing. The old
production body fails the new source contract because the software predecessor
assignment is absent. `run-legacy-hcd-concurrent-hotplug-test.sh` passes both
ordinary and ASan/UBSan executions after removing its stale reference to the
deleted, unused `src/kern/io-stats.c` translation unit. The focused private heap
fixture remains passing ordinary and under ASan/UBSan.

Explicit `make -j16` builds pass with `config/ci/config-amd64.mk`,
`config/ci/config-pcat.mk`, and `config/ci/config-pc98.mk`. Post-fix
`temp/q141-fixed-paired` passes the original paired input/root-I/O campaign with
campaign exit 0, no first-failure sample, and no sustained-walk observation.
`temp/q141-fixed-xhci` passes the independent xHCI input/root-I/O cell.
`temp/q141-fixed-halt-paired/result.json` establishes terminal CLI/HLT on all
four CPUs with no USB shutdown error. `temp/q141-fixed-normal-pcat/result.json`
passes three normal login/logout/respawn cycles.

The additional missing-login lifecycle rerun is recorded separately because it
does not exercise UHCI. `temp/q141-fixed-lifecycle-pcat/result.json` fails its
cycle-1 owner equality: task/thread counts fall from the cached baseline of 9
to 8 after the first missing-login child. There is no allocator failure, panic,
or UHCI path in that PCAT result. This is an independently actionable p021
lifecycle regression and is not evidence against the proved p024 repair, but it
means phase024's stated full regression boundary is not green.

q141 is finished. The intermittent heap corruption's exact writer, stale owner,
and narrow production correction are proved, and all heap/USB-specific p024
gates pass. p024 nevertheless remains **uncleared** solely because the required
applicable missing-login lifecycle gate is red. Do not reopen the heap writer
investigation unless a repaired lifecycle baseline later reproduces a heap
failure; route the 9-to-8 owner mismatch back to p021 and rerun that one gate
before marking p024 complete.

## q142 closure

The corrected fixture records the same 9-to-8 decrease before creating any
child, including a PID 0 thread decrease from 8 to 7. This matches detached
bootstrap retirement, independently of getty. After bounded observed settling,
PCAT passes 400 exact child comparisons and PC98 passes 100. See
[q142 results](../phase021/q142-results.md).
p024 is completed; q141's historical uncleared status above is superseded.
The unrelated historical p021 invalid-free provenance remains unproven.
