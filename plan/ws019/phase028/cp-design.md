# Attribute-preserving cp and completion records

Implementation design for q161; no implementation-complete claim.

Keep plain regular-file cp and its -T, -n, --update=none-fail,
--attributes-only and --preserve=mode staging behavior. Add recursive -R/-r,
preserve -p and archive -a. Archive preserves ownership, mode, atime/mtime,
symbolic links and hard-link relationships using one context for all operands.
Do not follow symlinks while recursively copying the immutable root image.
Explicitly reject unsupported special types rather than treating them as empty
regular files. Audit the actual rootfs inventory and supply all types it uses.

Regular-file copying keeps checked read/write loops and descriptor identity
checks. Set owner/group before mode because chown can clear set-id. Apply
timestamps after content writes; check output close before emitting completion.
Directories are created with enough temporary owner access to populate them,
then finalized after their children. Preserve an existing destination's ordinary
non-archive behavior. Copying into the same object or beneath the source must
fail without unbounded recursion. Do not replace a destination symlink by
following it into an unrelated tree.

For archive hard links, record source dev/ino and the copied destination's
identity. A later name links to that verified copied object and counts only
after successful link/metadata verification. Do not keep linking to a path whose
destination object was replaced. Allocation/error unwind releases the map and
all handles; failures must not invent completion records.

Add an opt-in --report-file=PATH capability to the existing cp command. Create
the report exclusively under the caller's owned workspace, outside both trees.
Use an ASCII version header followed by records containing type and hex-encoded
source pathname bytes, one record per completed object. File and directory
records are distinct; no raw filename or diagnostic is a protocol line. Check
every report write and its final close. Keep ordinary stdout/stderr diagnostics
separate. A report error fails the operation; previously copied files remain
visible and the installer must report incomplete work, not roll back arbitrarily.

Noct runs find -fprint0 into a regular-file manifest before cp, checks the child
status, then counts/compares filename records as bytes (or hex), not by splitting
UTF-8 text lines. It displays the frozen non-directory total first. While cp is
alive it reads the growing report through File APIs, buffers partial records,
and rejects duplicate, unknown or malformed records before advancing progress.
Process output uses a PTY, so neither binary manifest nor completion records
travel through Process.read. Process status and the report's complete final
records must agree. Final tree/metadata verification and sync follow the count;
total/total does not itself mean installation complete.

Test default option regressions, nested/empty directories, empty/large files,
hidden and unusual names, symlinks, cross-directory hard links, restrictive
modes, ownership and nanosecond timestamps. Inject read/write/metadata/close/
report failures. Verify late failures leave an accurate successful prefix and
never advance the failed file. Native UFS acceptance must check real metadata
and links rather than inferring them from host success or syscall declarations.

Remaining reconciliation design: extend the existing diff command with physical
recursive comparison (-r, brief output -q) and opt-in --metadata. Compare both
directory entry sets, complete regular-file bytes, symlink target bytes and
special-device identities. Metadata comparison covers type, permissions,
owner/group, and atime/mtime including nanoseconds; ctime and raw inode numbers
cannot equal across copies. Maintain a bidirectional source/destination inode
mapping to detect both broken hard-link groups and accidental merging of
independent files. Directory sizes/allocation counts may differ legitimately.
Check traversal/read/close errors, distinguish differences (1) from inspection
failure (2), and never follow symlink cycles. Preserve the existing nonrecursive
text comparison interface. This is an existing general Unix command extension,
not an installer-only helper. Add focused difference/error tests before using
its status as final tree proof.

The installer performs content/metadata reconciliation through read-only mounts
of the synced source/destination filesystems, before subsequent deliberate boot
configuration changes. Ordinary reads can advance cached atime; verification
must not write those access-time changes back into the accepted copied tree.
Progress report equality alone remains insufficient. Final integration and
read-only verification mount ownership belong to p007; p028 supplies and tests
the reusable comparison capability and callback-based copy runner first.
