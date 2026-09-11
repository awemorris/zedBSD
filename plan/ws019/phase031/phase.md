# ws019-p031: exclusive whole-disk administration

Status: completed q164; timebox 90 active minutes; [results](results.md)
Parent: [WS019](../ws.md); prerequisite of p006
Authorization: standing autonomous goal; queue presented before implementation.

Add privileged BLKRESERVE on an O_RDWR whole physical block description. Input
is its previously observed BLKGETINFO record; mismatched registration/geometry
or media replacement refuses. No partition/loop-device reservation yet. The
successful description owns the reservation until its final close (dup/fork
share ownership). Reject repeat reservation, readonly flags/media, competing
opens, in-flight/cache users, referenced children and existing swap/loop claims.

Register a raw ADMIN backing claim first, then atomically close new opens and
child-namespace admission under the disk registry lock while rechecking the
sole administrative open and idle children. An earlier writer holds a mutation
guard and excludes the claim; a later writer is excluded by the claim. An
earlier read-only mount/open makes the registry check fail; a later one cannot
open the disk. No claim-only check is mistaken for read-only mount exclusion.
Foreign cache and physical I/O admission is closed too; the owning description
enters a synchronous thread scope for each operation and always leaves it on
return. No thread pointer survives a syscall. Thus a concurrent filesystem
probe cannot repopulate a stale cache line while the owner uses direct I/O.

Keep the claim on struct file, serialized with its existing f_lock. Reserved
reads bypass the cache and writes use explicit disk_write_direct_claimed, not
io_context provenance as implicit authorization. At acquisition invalidate old
clean cache lines; on each write invalidate overlapping clean lines while
foreign admissions are closed. Dirty/in-use invalidation failure releases a
failed acquisition instead of granting partial protection. BLKREREADPART
on the owner description passes the claim through partition/disk admission but
retains the existing synchronous thread-owned reload section. No thread pointer
is retained across userspace calls. Failed reservation releases partial state;
final backend close releases the gate and claim. Removed media fails future I/O
and cannot be replaced beneath an old descriptor.

Acceptance: production-linked host tests for identity, privilege, flags,
competing/child opens, in-flight work, claim failure rollback, owner writes,
reload, duplicate reservation, close release and removed media; real claim
regressions; disposable QEMU block-ioctl probe for busy root/mounted children,
new open refusal, owner write/read/fsync, owned reload and final-close release;
amd64/pcat/pc98 builds. No physical disk writes. This phase does not claim the
native installer or partition formatter complete. Generalize to partition
formatting in its subsequent bounded implementation after auditing ancestry.
