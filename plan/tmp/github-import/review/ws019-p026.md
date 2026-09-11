# ws019-p026: Noct-managed image copy and progress

Status: completed; q159, user instruction 2026-09-09

Authoritative outcome: [results](results.md). The run observations below are
historical; session 9572 is terminal and native cancel/install/rerun passed.

Replace artifact-byte cp with bounded Noct File read/write loops: at most
64 KiB chunks, 64-bit counters, partial-I/O handling, early EOF/growth refusal,
close on every path and per-file byte progress. Do not buffer entire images
or merely extend the former timeout. Preserve identity/size/digest checks,
sync, no-replace publication and owned staging cleanup. Empty-file attribute
creation can retain cp; native tree copying explicitly uses cp under p007.

Test chunk boundaries, short I/O, zero progress, early EOF, changed size,
open/read/write/close/progress failures and handle cleanup with injected ops.
Test real File APIs on host and QEMU, then resume public cancel/install/rerun/
conflict acceptance. p004 stays incomplete until its full acceptance passes.

Implemented copy.noct with 64 KiB maximum chunks and 1 MiB/final progress.
Host injected-operation tests pass 73 cases in both JIT and interpreter modes;
real binary copying compares equal in both modes, and /dev/full reports failure.
The destination subset/conflict suite and 102 transaction scenarios still pass.
The amd64 disk-image build and actual UFS package-content check pass.
Native full integration is live in temp/q159-public6, session 9572. No completion
claim until that runner is terminal and its results are inspected.

The native run reported all loader/kernel/rootfs bytes copied, including
33554432/33554432 rootfs bytes, then published all six managed files and reported
`Installation complete.` with status 0. At 09:50 UTC the same runner and QEMU
were confirmed live, checking the already-complete destination rerun. Keep
polling this same handle; do not restart based on an observation timeout.

The subsequent conflict test in the running Python instance still contains an
unqualified cp invocation which selects the shell builtin. The reusable runner
now explicitly uses /bin/cp, and run-install-conflict-qemu.py can continue only
that case on a copy of a terminal run's target. It requires recorded successful
cancel/install/rerun cases and unchanged protected/source hashes. This prepared
continuation has passed Python syntax validation but has not yet run in QEMU.
