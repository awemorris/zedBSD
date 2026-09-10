# HAL fixed-entry results

## q295: fixed syscall callback implemented

Removed hal_syscall_set_handler, hal_syscall_handler_t and five per-HAL syscall
function pointers. amd64/i386/arm64/m68k/sparcv9 now call kernel_syscall_handler
with existing register extraction, masked IRQ, active user-frame and return
handling. Generic implementation is the former syscall_dispatch body, preserving
accounting, credentials, restart/redispatch and signal policy. syscall_init still
initializes poll/usync/user-atomic/signal services; no dynamic registration.

amd64, PCAT and PC98 image builds PASS in /tmp/zedbsd-q295-{amd64,pcat,pc98}.log.
Source/include search finds no registration typedef/function/pointer. amd64 nm
exports kernel_syscall_handler and has no removed registration symbol. Non-x86
calls source-reviewed only; not cross-built or runtime tested this queue.
No assembly frame layouts changed. Runtime syscall/signal validation remains
for the complete fixed-entry phase; do not infer runtime acceptance from builds.

Pending: normalized user/sys fault entry, OTHER/access semantics, diagnostic
native vector/error ownership, removal of kernel_user_int_handler observers and
trap registration; review architecture-specific return constraints before edits.
I/O performance p028 remains on explicit expert-review hold; no input-span work.

## q296: fixed supervisor fault entry

Removed hal_set_trap_handler, its typedef and all four registration arrays.
All five HALs offer supervisor exceptions to kernel_sys_fault_handler; generic
policy returns FAILED without changing IRQ state, preserving existing diagnostic/
stop behavior (no production registration consumers existed). Native vector/raw
error are passed as diagnostic data. Added OTHER/NONE and arithmetic/protection
vocabulary. Complete user/sys decoder normalization remains next; i386 currently
classifies non-page sys faults OTHER and m68k retains previous decode pending
shared user/sys review. No recovery success is fabricated.

ARM64 current-sync used SAVE_FRAME288 then infinite loop after C returned. It
now uses matching existing RESTORE_FRAME and eret, exactly as current IRQ/user
sync paths do. No saved-layout change; future handled sys fault can return.
C still diagnoses/stops unhandled exceptions. Full architecture exception-slot
identity and user trap policy require follow-up; do not infer runtime coverage.

amd64/PCAT/PC98 builds PASS. Bundled clang lacks AArch64 assembler target, so
that attempt failed; installed /usr/bin/clang successfully assembles trap.S for
aarch64-unknown-none. Non-x86 C/full images and runtime tests not run this queue.
Production src/include has no old trap registration symbol/array.

Next normalize user fault cause/access and native diagnostics on each HAL,
remove legacy user-int observer entry, preserve signal mapping/detail, and run
focused user/sys routing plus appropriate runtime verification. p038 unfinished.

## q297: normalized user fault callback and fixed syscall observation

All five HALs now pass cause/pc/address/mode/detail plus native diagnostic vector
and error value. Generic user-probe policy uses cause and mode for VM faults and
signals, never native vector/error bits. Integer divide/overflow details retained;
unspecified arithmetic uses SI_KERNEL. Added missing FPE_INTOVF UAPI constant.
Invalid page-fault mode refuses recovery rather than silently using READ.

Removed kernel_user_int_handler from HAL contract/call sites. Fixed syscall entry
records once through user_probe_syscall. Legacy observation record remains but
vector/cs/eip are0 (unavailable), not fabricated x86 numbers. Fault probe keeps
native vector/error and unavailable cs0. Existing external probe consumers must
adapt to the diagnostic contract; no production consumer of old location fields
was found. SPARC non-page handled user faults now perform user-return/leave and
return instead of unconditionally stopping after successful signal queueing.

Actual generic-entry fixture PASS ordinary and ASan/UBSan: misleading raw vector
and error values do not affect READ/WRITE VM selection or SIGILL/OTHER policy;
EACCES, INTOVF, invalid mode, IRQ/accounting balance, sys IRQ preservation and
syscall observation tested. Evidence temp/q297-fault. amd64/PCAT/PC98 image builds
PASS; first PCAT compile caught an accidental duplicate decode insertion and was
fixed before retry. ARM64/m68k/SPARC target C syntax checks with installed clang
PASS; not full non-x86 link/runtime tests.

Remaining p038 work: audit detailed architecture decode/diagnostic completeness
(e.g. ARM64 assembly currently supplies0/8 categories rather than precise vector
slot, SPARC initial priming versus active-frame contract), repair old probes/
fixtures as needed, and QEMU syscall/restart/signal/fault runtime gates. No claim
that all architecture exception classes or runtime returns are already verified.
I/O p028 remains on explicit expert-review hold.

## q298: real amd64 fault/signal/restart gate; INT3 gate fix

Added fixed-entry-guest.c and disposable run-fixed-entry-qemu.py. Actual UD2,
null load and divide faults first passed expected child termination signals.
INT3 failed before handler entry: retained user_fault_probe showed vector13,
error0x1a, PC40573e; guest disassembly at that PC is cc (INT3). Both x86 HALs
registered all fault gates DPL0, making user INT3 a protection fault.

Changed only vector3 gate to DPL3 on amd64/i386; interrupt-gate masking and other
gate privileges remain. After fix native guest PASS: syscall, UD2/SIGILL, page/
SIGSEGV, divide/SIGFPE, returning INT3 handler/sigreturn, alarm EINTR, SIGUSR1
SA_RESTART blocked read, fork/wait and unknown syscall ENOSYS. Evidence
temp/q298-fixed/{result.json,guest.log,argv.json}; source unchanged. Prior failing
guest/stage/handler artifacts retained; first guest compile corrected sa_handler
assignment for zedBSD's integer-address ABI. Instrumented handler markers confirm
actual arrival/return, not just signal registration. Runner detects guest shell
Segmentation fault and captures fault probe on failure; own QEMU cleanup retained.

amd64 image build and PCAT/PC98 image builds PASS. This is amd64 runtime evidence;
i386/non-x86 runtime and ARM64 precise-slot/SPARC priming contract audit remain.
No I/O performance work resumed. p038 remains in-progress.

## q299: ARM64 native exception slot preservation

Vector table now branches non-IRQ slots to per-slot out-of-line stubs. SAVE_FRAME
runs before x0/x1 argument setup; C receives actual0..15 slot, preserving all
interrupted registers and the existing288-byte layout. IRQ entry unchanged.
Supervisor slots0/4 decode synchronous ESR; lower AArch64 slot8 alone can enter
SVC/page-fault decode. FIQ becomes OTHER, SError MACHINE_CHECK, unsupported
AArch32 sync OTHER. Stale/inapplicable ESR cannot dispatch asynchronous slots
as syscall or VM fault. Handled exceptions restore the same saved frame.

Verbatim actual arm64_sync_handler fixture PASS ordinary/sanitizer for all12
non-IRQ slots with contradictory page/SVC ESR, native diagnostic slot, user-frame
enter/return/leave pairing and supervisor breakpoint. Actual AArch64 assembly and
C syntax PASS using installed clang. Evidence temp/q299-gates/source.json and
logs; reusable tests/run-arm64-fixed-entry-host.py. An initial fixture expected
6 sys calls but made7 including explicit breakpoint; fixed expected count after
checking control flow. No kernel change made to satisfy that mistaken count.

No full ARM64 image boot/real exception test yet. p038 still requires non-x86
runtime/return audit, including SPARC initial priming user-frame contract, and
appropriate i386 runtime. No x86 source changed in this queue, so its previous
runtime evidence stands within its scope. I/O performance remains held.

## q300: SPARC initial frame mismatch, user decision pending

Source audit confirms startup calls generic user fault without active user frame,
unlike actual trap save/restore. Saved [review](sparc-startup-review.md) explains
call paths, failure behavior, recommended best-effort prime/real-fault delegation
and complete-frame alternative. Asked user per explicit stack-difference
consultation instruction. No dependent SPARC code changed. Continue independent
architecture verification while waiting; do not treat lack of answer as approval.

## q301: m68k non-memory access mode

Actual trap.c/exception.c fixture reproduced supervisor alignment fault reporting
READ rather than NONE (temp/q301-gates/before.log). Normalize non-page cause
access once before user/sys routing. Saved frames and return paths unchanged.
Actual-source host ordinary and ASan/UBSan PASS, m68k target C syntax PASS;
evidence temp/q301-verified with source hashes. User/sys non-memory routing,
page READ/WRITE/EXEC, syscall argument/result and user frame ownership checked.
This is host dispatch validation, not real m68k exception/return execution.
No x86 source changed; previous image/runtime results retain their limited scope.
SPARC startup remains pending the user's frame decision; p038 not complete.

## q302: PC/AT i386 native fixed-entry PASS

Extended shared x86 guest/runner for explicit --platform pcat, i386 sysroot and
compiler runtime, soft-float compilation, MBR discovery and BIOS IDE boot.
Guest uses 32-bit register instructions valid in both x86 modes. No production
code change needed for this queue. Initial setup attempts exposed missing
compiler-runtime link and GPT-only image parser in fixture; retained temporary
outputs, corrected against current build/sysroot and MBR source layout.

Disposable build/pcat/bios-hdd-image.img clone PASS: actual UD2/page/divide
fault termination, returning INT3 signal, EINTR, SA_RESTART, fork/wait, ENOSYS.
Evidence temp/q302-pcat-bios/{result.json,guest.log,argv.json}; source unchanged.
This adds i386 runtime coverage to q298 amd64 evidence; modified shared runner
has not been rerun in amd64 mode in this queue. SPARC initial-frame question
still pending; non-x86 real exception/return runtime remains unverified.

Final m68k whitespace cleanup revalidated ordinary/sanitizer/target syntax with
updated hashes in temp/q301-final. Focused git diff --check PASS.

## q303: stopped on user request

i386 normalized cause/mode/detail and fixed callback calls rewritten with
explicit branches and named result, preserving classification. PC/AT, PC98 and
amd64 disk-image builds passed (temp/q303-build). Updated shared amd64 guest
run reached login but not Password before user requested stop. Dedicated QEMU
terminated; runner exited and recorded failure/source hash in temp/q303-amd64.
This is an interrupted, incomplete gate, not a runtime PASS or demonstrated HAL
regression. PC/AT rerun was not started. Preserve q298/q302 prior runtime evidence
with its original source scope. Resume these gates after user resumes work.
SPARC startup decision remains pending.
