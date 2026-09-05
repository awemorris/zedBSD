# Q077 filesystem audit findings and coverage

Date: 2026-09-05. Baseline: `d97e21c`. Status: bounded functional audit/corrections complete; final host/build/runtime gates pass.

Scope: [p014 mounted namespace correction](../phase014-mounted-namespace-protection/phase.md)
and user-added [p015 filesystem-wide audit](../phase015-filesystem-wide-audit/phase.md).
Normal/sanitizer PASS is evidence for the exercised paths, not all filesystem
failure or concurrency behavior. No physical media was modified.

## Findings and disposition

| ID | Functional defect | Correction and evidence |
| --- | --- | --- |
| FS-A01 | Mutation bypassed virtual mount children, reaching rmdir/rename callbacks on covered directories | P014 shared mutation checks include source, replacement target, aliases and ancestors. KA-T121 normal/sanitizer and guest mounted probes pass. Original `rmdir=0 expected=17 callbacks=1`; corrected `rmdir=17 expected=17 callbacks=0`. |
| FS-A02 | Preparation, duplicate bind publication and failure-capable unmount did not reserve attachments across I/O | PREPARING/DYING reservations and sleeping transaction exclusion; 21 controlled host interleavings pass. No filesystem I/O under the namespace spinlock. |
| FS-A03 | A delayed lookup could receive the newer directory sequence at cache insertion | Capture sequence before backend lookup and reject stale insertion; production-linked delayed-publication regression passes. |
| FS-A04 | Initial p014 namespace joining inverted overlay materialization and inode I/O locks | Materialize before content/metadata I/O locking through `prepare_mutation`; already-owned I/O requires a committed upper and does not reacquire namespace. Both generic ordering tests and the actual overlay preparation/copy-up fixture pass. |
| FS-A05 | Separate dinodes sharing a disk block lost concurrent updates | Hold the existing mount metadata mutex across the entire dinode-block read/modify/write. Real host threads contend at that boundary; both values 111/222 survive, and read/write errors release the lock. |
| FS-A06 | Truncate freed blocks before persisted pointers were detached; allocation publication failure could free still-referenced blocks | Detach and flush pointers before bitmap reuse, including direct and depth-1/2/3 indirect blocks. Allocation rollback confirms pointer removal before freeing. Directory rollback, inode discard and final reclaim also flush detachments before reuse. Failed detachment retains allocations. Failure matrices inspect live and flushed references before every tested bitmap release. |
| FS-A07 | The one-block directory writer scanned a larger directory size, and incomplete terminal headers were decoded | Validate the supported writer size/alignment and each header before decoding, plus record alignment/end bounds. Larger existing directory reads remain supported; extending mutation support beyond the existing single block is outside this correction. Oversized, misaligned and four-byte terminal-header cases pass under ASan/UBSan. |
| FS-A08 | Lookup miss/allocate/initialize admitted duplicate in-core UFS objects | Join the existing filesystem namespace gate across lookup and inode admission, including already-owned creation callers. Two controlled production load calls return the same single allocated object. Cache allocation itself is a host adapter in this test. |
| FS-A09 | Overlay removal committed but a subsequent sync error skipped cache retirement; rename did not advance directory sequences on that error | Publish directory sequence, cache invalidation and retirement at the namespace commit, before sync. Actual overlay remove/rename postcommit-EIO regressions pass and retain the reported durability error. |
| FS-A10 | UFS2 xattr publication rollback could fail and still release a potentially referenced allocation | Flush successful pointer removal; if rollback cannot be confirmed, retain allocation and stop new mutations. Two write failures with a committed first write leave the on-disk reference allocated. New/delete xattr write/sync fault cells pass. |
| FS-A11 | Visible upper files omit their hidden lower path, so unlink/rename could reveal the old lower entry | Probe the backing lower directory when deciding the old-name whiteout. Actual overlay unlink and rename tests verify that the old name remains ENOENT, including postcommit sync failure. |
| FS-A12 | tmpfs returned a successfully written prefix before publishing its EOF; a zero-byte backend write could extend EOF | Publish EOF after each copied page. Quota, allocation and VM-commit failure cells preserve the readable prefix; zero-byte write leaves EOF unchanged. The generic VM resize path already compensates for some extending writes, so this is a backend-contract correction, not a claim that every ordinary write previously lost its size. |
| FS-A13 | Several UFS public mutation/sync callbacks ignored the filesystem's error-stop writable flag | Check the stop state at namespace/attribute/sync admission. Post-failure callbacks return EROFS without writing. Read-only inode sync remains non-writing. This does not claim cancellation of every already-running operation. |

The original FS-A05 diagnostic scheduled B's complete production persist call
between A's block read and write and reproduced `left=111 right=0` in all four
version/mode cells. That result remains historical evidence. Its maintained
[runner](run-ufs-metadata-audit.sh) now uses real host threads and failure
injection instead of treating a reproduced defect as acceptance.

The previous driver review stopped before producing FS-A06/A07/A08 diagnostics.
Those diagnostics and the final correction review were performed during this
functional continuation; they are not attributed to the earlier worker.

## Functional coverage and limits

| Boundary | Reviewed / exercised in this continuation | Remaining evidence limits |
| --- | --- | --- |
| Namespace / lifetime | mount, inode, namei, namecache, cwdinfo, mutation syscalls; attachment reservations, bind identity, ancestor reachability, rollback and stale lookup; KA-T121 plus real guest probes | No new public nested-mount API or per-process mount namespace; controlled interleavings are not all scheduler schedules |
| Descriptors / file content | filedesc reference-before-unlock and detach-before-close/clone; file content/resize begin/commit, append and EOF publication; inode preparation; tmpfs prefix fault cells | No new exhaustive mmap/VM stress campaign; VM integration was reviewed against its production caller, while tmpfs failure injection exercises its backend directly |
| Overlay | lookup visibility, copy-up/materialization, rename/remove commit boundaries, whiteouts, metadata and truncate callbacks; 3,204 normal/sanitizer checks | Host filesystem/I/O adapters provide deterministic faults; no claim of every journal/power-loss cut or physical durability |
| FAT | mount mutex/pool ownership, inode/open-file authority, pending closes/orphans, rename/repath and mount sync; 441,782 native FAT12/16/32 checks in each mode and shared FAT guest | Full physical power-loss and all concurrent raw-writer behavior are outside the campaign |
| UFS1/UFS2 | inode-block publication, admission, direct/indirect allocation/release, directory records, discard/reclaim, xattrs, error-stop admission; normal/sanitizer 6,010 UFS1 and 6,595 UFS2 ordering/admission checks plus shared-block/bounds tests | Fault fixture uses synthetic in-memory geometry and a separate flushed image, not physical disks. UFS2 committed-error injection models the caller-visible replay outcome; KA-T021 separately exercises the journal implementation |
| UFS format / quota / snapshot | superblock divisor/extent/layout validation, quota reserve/transfer/import preflight, snapshot preserve-before-write and journal replay contract reviewed; filesystem identity and retained UFS2 consistency gates pass | No new general quota or snapshot fault matrix; supported on-disk layouts remain unchanged |
| tmpfs / pseudo filesystems | tmpfs page accounting/read/write/truncate and namespace ownership; devfs generation snapshots/close/range arithmetic; console open/close inventory | Device-specific console/HID behavior remains owned by its existing WS. Functional filesystem audit does not expand into a security audit |
| Block / backing / storage | buffer dirty/error retention and sync path, backing-claim canonical ranges, loop/swap ownership, disk-wide reload exclusion; claims/storage/devfs gates and shared reboot cell | Existing boundaries retained; no installer/formatter command, real-media write or forced reload |

## Executed gates

See [q077 results](q077-results.md) for commands, supported build/runtime
identity and Queue accounting. Security auditing, non-x86 runtime and exhaustive
crash/concurrency proofs are not implied by the functional results.

## Scope decision

The 2026-09-05 user request explicitly authorizes filesystem-wide functional
correction and resolves the earlier audit-only question. The fixes above belong
to p014/p015. All matrix rows have a bounded review record; the limitations
above describe untested combinations rather than a claim of universal absence
of defects. The second q077 runtime cell passes on the final source; both cells and their
source identities are recorded in the result book.
