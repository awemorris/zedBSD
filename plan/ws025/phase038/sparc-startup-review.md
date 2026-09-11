# SPARC initial user-frame contract: decision pending

2026-09-10, q300. No production changes in this audit.

`context.S:sparcv9_user_task_entry` calls `sparcv9_user_task_prepare` while on
kernel stack, before setting TL=1/TPC/TNPC/TSTATE and retrying into user code.
There is no active_user_frame at this point. `trap.c` attempts prime_mapping,
and if it fails calls the fixed user fault callback anyway. A successful VM
fault can make this appear to work, but failed VM resolution queues a signal
without a saved user return frame; the startup code then panics if priming still
fails. This violates the fixed user fault contract rather than defining a
second equivalent exception frame.

Actual `trap-entry.S:sparcv9_user_trap_entry` saves old_sp/output registers in
kernel trap-stack memory, saves user windows in task-owned storage, switches
windows and kernel SP, and invokes user dispatch with its saved frame. It does
not need to write a valid user stack before the ordinary miss dispatcher.
The entry also recognizes nested misses from user register-window spill.
`space.c:sparcv9_prime_mapping` installs an existing software mapping into TLB;
it does not allocate missing VM pages itself.

Recommended transition: keep initial TLB priming best-effort for already-present
text/stack mappings; never invoke generic user-fault policy from initial kernel
startup. Let actual user text/data faults establish their normal saved frame and
resolve/queue signals. Do not weaken the callback's active-frame requirement and
do not fabricate a callback-success result. User stack/TLS/window first-use and
initial text fault must be exercised on real SPARC emulation before acceptance.

Alternative: construct a complete initial saved user-frame/window context and
route startup through the existing user-return mechanism, including handler PC
and stack edits. A local synthetic C frame alone is insufficient because current
assembly unconditionally returns to its original l0/l1 entry/stack values.

User was asked asynchronously to choose recommended real-fault delegation,
complete initial-frame design, or hold. The earlier explicit instruction requires
consultation for stack-layout differences; dependent production edits are held
until answer. Other HAL validation can proceed. p038 remains unfinished.
