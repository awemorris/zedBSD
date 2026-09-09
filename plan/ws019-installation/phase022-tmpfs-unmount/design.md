# q155 ownership design

Pair an explicit inode namespace-reference count with the existing total count.
Classify only filesystem directory-entry references, not linkcount or returned
lookup handles. Retain takes a total reference before classifying; release
unclassifies before dropping the total. Observers may conservatively see an
external owner, never an unsupported internal allowance. Keep existing namespace
transactions and locks.

Generic unmount rejects mount/path/VM owners, marks DYING, purges namecache,
then checks inode ownership against cache/root plus classified references.
Sync and prepare failures occur before any namespace destruction and can restore
LIVE with all files intact.

Add an infallible inode namespace-retirement callback for final destruction.
First pin every member of the admitted mount under the inode-cache lock. Walk
the bounded cache and invoke callbacks outside its spin lock, then release all
pins in a third pass. This prevents another mount's allocation from evicting
a directory whose parent has retired but whose own callback has not run yet.
Tmpfs detaches/frees directory-entry lists and drops their
classified references. Do not mark linked nodes dead: cache references keep them
until the following purge. No recursive traversal, allocation or fallible work
belongs in this stage. Then release root, purge inodes/pages/xattrs/charges, and
finally free filesystem state. Other filesystems have zero classified references
and no callback; bind mounts never retire their source namespace.

Tests reproduce the q154 defect with actual tmpfs/inode/mount code, cover
create/link/unlink/replacing rename ownership, genuine busy users, failure
preservation and complete resource recovery. Audit all tmpfs reference sites.
