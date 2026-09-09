# Exclusive block administration

`<zedbsd/block.h>` provides `BLKGETINFO`, `BLKRESERVE` and `BLKREREADPART`.
`BLKGETINFO` returns a live registration identity and logical-sector geometry;
the registration number is not an on-disk GUID and must not be persisted as a
boot selector. Initialize `version`, `struct_size` and zero reserved fields.

For whole-disk destructive administration, open the physical disk `O_RDWR`,
obtain and review its `BLKGETINFO` record, then pass that exact record to
`BLKRESERVE`. Reservation requires superuser privilege and refuses stale
identity/geometry, readonly media, file-backed loops, competing
opens, mounted children (including read-only/private mounts), backing claims,
in-flight/cache users and referenced children. The admitted disk remains the
same object even if hardware is removed; later I/O fails rather than switching
to a newly registered disk.

The reservation belongs to the open file description. `dup` and `fork` share
it; closing one descriptor does not release it while another reference exists.
Final close, including process exit, releases it. There is no force mode or
explicit unlock. A second reservation on the description returns `EBUSY`.
Reservation failure may invalidate clean cache copies but does not initialize
or format media. Dirty or busy invalidation refuses acquisition.

The kernel combines a raw backing claim with a registry admission gate. New
opens and foreign cache/physical I/O overlapping the reserved range are excluded. Each owner operation enters
a synchronous kernel thread scope, leaving it on every return; no thread
pointer survives a syscall. Reserved reads use direct I/O, and writes use an
explicit claim after invalidating overlapping cache lines. I/O context
provenance alone does not grant write authority. New filesystem probes cannot
populate stale aliases during a direct write.

Write, check every returned byte count, and `fsync` before using
`BLKREREADPART` on the same reserved description. Owner reload retains the
reservation while replacing the partition-device registry. The disk's data
write/flush outcome and reload outcome are distinct: a reload error does not
restore previous metadata. A writer must report partial state after any
attempted write, never claim crash atomicity or rollback from this API.

## diskpart init

`diskpart init DISK DISKUUID [START COUNT TYPE PARTUUID NAME]...` replaces a
whole physical disk's table with GPT. Each five-argument group defines a
partition, numbered in argument order; zero to sixteen groups are accepted.
START and COUNT are decimal logical-sector units, TYPE and PARTUUID are GUIDs,
and NAME is one shell argument. DISKUUID is a nonzero GPT disk GUID. The command
checks all bounds, overlap and names before asking for confirmation.

The proposed complete table and disk registration are displayed while one
reservation is held. Enter the exact displayed `ERASE NAME:REGISTRATION`
phrase to continue. Wrong input or EOF cancels without writing partition
metadata. No force option or machine mutation mode is provided. An installer
may send this phrase only after its own explicit confirmation, using the
reviewed device registration so a changed device cannot silently be accepted.

On success both GPT copies and the protective MBR are written, flushed and
read back, and the live partition devices are reloaded. Exit 0 means all those
steps and descriptor closure succeeded; exit 1 is refusal or operation failure;
exit 2 is invalid command syntax; exit 3 means the table was written/verified
but kernel reload failed. Any error after writing may leave changed metadata.
Neither initialization nor rollback is crash-atomic. Existing filesystem data
sectors are not wiped, and the command does not format a filesystem.

## Partition reservations

`BLKRESERVE` also accepts a direct partition of a physical disk. It compares
canonical physical sector ranges: whole-parent and overlapping aliases are
excluded, while nonoverlapping sibling mounts and I/O remain usable. Two
disjoint partitions may have separate reservations. Nested partitions and
file-backed disks remain unsupported. The target must have only its owning
open description, with no overlapping opens, mounts or backing claims.

Acquisition conservatively requires an idle physical I/O/cache frontier, so
transient sibling activity may return `EBUSY`; it does not permanently exclude
an idle mounted sibling. Owner reads and writes stay within the target range.
`BLKREREADPART` on a partition returns `EINVAL`: only a whole-disk owner may
replace partition mappings. Partition filesystem formatting and installer
integration remain separate implementation work.
