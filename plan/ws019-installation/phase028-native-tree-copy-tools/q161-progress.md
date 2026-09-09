# q161 progress

Status: completed q161. [Final acceptance](results.md). The entries below retain
the historical sequence and their then-current remaining work.

find now appends implicit print exactly once based on parsed actions, supports
-print0/-fprint0, checks readdir EOF/error, records action-output failures even
when boolean evaluation continues, and checks buffered close failures. Owned
output streams also close on parse failure. Host find-manifest-test.py passes
19 cases including unusual names, implicit/action semantics, cycle/depth limits,
/dev/full and link-time readdir/closedir failures.

amd64 and pcat disk-image builds passed in /tmp/zedbsd-q161-find-{amd64,pcat}.log.
pc98 build also passed, /tmp/zedbsd-q161-find-pc98.log. run-find-manifest-qemu.py
is live as session 8729, temp/q161-find1, /tmp/q161-find1.log, using the ordinary
image. It checks default census and byte-exact NUL manifests, then captures the actual
installer confirmation and cancels with unchanged whole-target hash. Show the
saved PNG to the user as requested. Poll this same runner until terminal.

Session 8729 is terminal exit 0: temp/q161-find1/result.json reports PASS find
manifest and installer capture. Default enumeration lists every path once;
the NUL manifest's SHA-256 matches complete expected filename records; a missing
source returns failure. The installer confirmation was captured and cancelled,
with whole destination and production hashes unchanged. The PNG was inspected:
temp/q161-find1/installer-confirmation.png. It shows the real current coexistence
confirmation, before the planned mode/source screens. A literal `4;2m` prefix
from terminal control-sequence handling is visible; track it with p027 screen
integration rather than presenting the current UI as visually finished.

The remaining cp/archive/report design is in cp-design.md. No cp implementation
change has yet been made. p028 and q161 remain in-progress, not complete from
the successful find work. No QEMU/build is live.

Subsequent implementation: regular-file cp now supports -p (owner/group, mode
and nanosecond atime/mtime). Ownership is restored before mode to preserve
set-id bits, and times after content writes. A reusable descriptor metadata
helper checks each failure. Existing --preserve=mode and exclusive/attribute-only
staging options retain their behavior. cp-attributes-test.py passes 13 host
cases: existing/new destinations, data/attribute-only paths, same-file refusal,
non-clobber and injected chown/chmod/utimens/output-close failures. This is a
regular-file prerequisite, not recursive/archive or completion-report support.
amd64 build passed (session 27218 terminal exit 0),
/tmp/zedbsd-q161-cp-amd64.log. No build or QEMU is live. Native cp metadata
acceptance, recursive/archive support and completion reporting remain required.

Metadata audit for subsequent cp: libc exports fchown/lchown/futimens/utimensat,
symlink/readlink and link operations; the kernel implements no-follow timestamp
and ownership updates through inode_setattr. stat has nanosecond atime/mtime.
Apply ownership before final mode because chown clears set-id; finalize directory
times after copying children. Existence of these APIs is not native UFS metadata
acceptance: verify actual destination behavior in p028 tests.

Next step implemented: cp -R/-r physical recursion, postorder directory metadata,
symlink/FIFO/device node recreation, same-tree admission and child-path errors.
Trailing source slashes retain the directory basename; source/. copies contents.
cp-attributes-test.py still passes 13 cases. cp-tree-test.py passes 14 cases,
including nanosecond directory/link metadata, cycles as links, same-tree and
destination-symlink refusal, FIFO, socket refusal, and injected traversal/node
metadata errors. The socket test required escalated local execution because the
sandbox denied AF_UNIX bind; the test itself passed unchanged. Recursive changes
have not yet passed target builds/native acceptance. Archive hard-link groups,
completion records and Noct progress integration are still unimplemented.
No build/QEMU process is live; resume these code changes under q161.

Archive step: -a/--archive enables recursive metadata/link preservation. One
invocation-owned registry spans directories and separate source operands. Only
completed copies become representatives; skipped -n files cannot redirect later
links to foreign contents. Reused representatives are checked for identity and
content-related metadata before and after link creation, and the registry is
released on all main-loop outcomes. cp-attributes-test.py passes 13 cases and
cp-tree-test.py passes 17 cases, now including cross-directory/cross-operand
link groups, copied-inode isolation and skipped-representative handling.
The pre-archive recursive amd64 build passed (16779). The archive amd64 build
passed (session 95967 terminal exit 0), /tmp/zedbsd-q161-archive-amd64.log.
No build or QEMU is live. Completion reports, Noct
progress and native archive acceptance remain; p028 is not complete.

Completion channel implemented: cp --report-file=PATH creates an exclusive
report outside both operand trees, emits byte-safe F/D records only after
successful copy/attributes/close (directories postorder), and checks report
writes/close. END requires successful processing, and consumers must also check
the child exit status. cp-report-test.py passes 9 cases; cp-attributes-test.py
passes 13 and cp-tree-test.py passes 17 after this change.

treeprogress.noct freezes byte-safe find census records and independently
validates completion records against them. Both JIT and interpreter runs of
installer-treeprogress.noct pass 25 cases: arbitrary read splits, newline and
non-UTF8 pathname bytes, separate directory/file totals, duplicate/unknown/wrong
kind records, truncated census/report, early/extra END, and child failure even
with a complete END. It is packaged as a shared module; the actual command/file
monitoring adapter and UI are not connected yet. Native metadata verification,
all maintained builds for these latest changes, and final tree reconciliation
remain required. These focused passes do not complete p028.

Latest maintained disk-image builds all passed:
/tmp/zedbsd-q161-report-{amd64,pcat,pc98}.log (sessions 50706, 2526, 52824,
all terminal exit 0). git diff --check passed.

Native run-cp-archive-qemu.py uses the ordinary packaged image with a separate
disposable UFS USB disk. temp/q161-archive1 failed before copying because the
test key map lacked '!'; both protected hashes stayed unchanged. The harness
now handles that key and explicitly invokes /bin/stat instead of the shell
builtin. Fresh temp/q161-archive2 passed, session 52008 terminal exit 0:
owner/group 17:23, mode 6751, source/destination timestamp seconds, hard-link
identity shared only within the copied tree, symbolic links, file contents,
exact completion path set including postorder directories, exclusive report
refusal, sync/unmount/remount persistence, and 25 Noct parser cases on-target.
Production and unused NVMe destination hashes remain unchanged. Native timestamp
nanoseconds were not asserted by the public stat output and remain open.

No build/QEMU process is live. q161/p028 remain in-progress: connect the actual
census/report monitoring adapter and progress UI, perform final tree/metadata
reconciliation, and cover remaining native attributes/errors before closure.

Command adapter step: treecopy.noct now runs one checked find traversal into
separate byte-safe file/directory manifests in an empty private workspace,
freezes the census, notifies the frontend before spawning cp, reads bounded
chunks from the growing report outside the PTY, reconciles all completion names
with the frozen census, and requires both END and child status. A callback error
or malformed report sends TERM to timeout, whose -k timer relays/escalates and
reaps the copy child. The result explicitly describes copying only; final
destination verification and sync remain separate requirements.

tree-copy-test.py passes 12 actual host adapter cases across JIT/interpreter:
successful unusual-name/hard-link copy, failed command, early END, nonzero child
status after complete reporting, UI failure with child cleanup, and missing
report. The amd64 build passed /tmp/zedbsd-q161-monitor-amd64.log (27576).
temp/q161-archive3 passed the native adapter's fixed-total-before-copy and final
progress checks as well as the earlier archive assertions; session 67496 is
terminal exit 0 and both protected hashes stayed unchanged.

Follow-up static finding: ufs_getattr and generic inode_getattr render seconds
but omit their in-core nanoseconds. A seeded nonzero timestamp fixture is now
running against the unchanged binary (temp/q161-time1, session 55451) before
fixing that precision loss. Do not count second-only stat output as covering
this requirement. No production changes/builds while that QEMU run is live.

temp/q161-time1 terminated with the expected defect: all three copied objects
(regular file, directory, symlink) have the correct seconds but zero atime/mtime
nanoseconds rather than seeded 123456789/987654321. Added the missing timespec
fractions in UFS getattr and generic inode_getattr. All three maintained builds
passed /tmp/zedbsd-q161-time-{amd64,pcat,pc98}.log (21386, 3301, 60167 terminal).
temp/q161-time2 failed before filesystem testing: additional xHCI port 2 device
enumeration error 42. Recorded separately as BUG-017; production/protected
images unchanged. temp/q161-time3 (session 57853) runs the same precision check
on one disposable raw NVMe UFS to isolate this contract. No build or production
mutation while it remains live.

temp/q161-time3 terminated and still showed zero nanoseconds. The missing
fractions were only part of the defect: legacy ZEDBSD_SYS_STAT_H guards no
longer match libc's LIBC_SYS_STAT_H, so the old UFS setter and generic inode
setattr paths also discarded fractions. Removed that obsolete ABI conditional
from getter/setter/UTIME_NOW handling rather than retaining a seconds-only
fallback. Current stat exposes timespec fields on all maintained targets.

All maintained builds passed again, /tmp/zedbsd-q161-time-abi-{amd64,pcat,pc98}.log
(13960, 47167, 34551 terminal). temp/q161-time4 passed the same nonzero precision
fixture on native NVMe UFS (session 95653 terminal exit 0). After sync/unmount,
the independent on-disk decoder observes atime 1700000001.123456789 and mtime
1700000002.987654321 for directory, regular file and symlink. Production hash
unchanged; the disposable raw NVMe is intentionally the written test target.
All q161 QEMU/build handles are now terminal. This resolves the demonstrated
UFS timestamp-copy loss; it does not claim whole-tree installer acceptance.

Remaining p028 work is the independent recursive content/metadata/link-group
comparator, then host/native acceptance of that verifier and final shared-runner
integration. Its design is appended to cp-design.md. The actual copy runner and
its fixed-total progress callback already pass host and target tests; source,
mode and graphical frontend integration remain p027/p006/p007/p029 as planned.

Final comparison step completed: diff -r/-q/--metadata and the shared Noct
instTreeVerify adapter independently compare complete directory sets, bytes,
types/owners/modes/nanosecond timestamps, and a bidirectional hard-link mapping.
diff-tree-test.py passes 24 cases, including I/O failures and a 1-MiB stack depth
guard. cp-report-test.py now passes 10 including a three-file late failure.
Final maintained builds passed as session 73258; verify2 native session 49285
passed Noct verification on read-only UFS, intentional permission mismatch and
independent on-disk timestamp checks. All handles terminal. q161/p028 completed;
q162/p027 is next. No full dedicated-installation acceptance is claimed.
