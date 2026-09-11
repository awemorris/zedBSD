# q164 / ws019-p031 results

Completed 2026-09-09. The kernel boundary for whole-disk initialization is
accepted. Command integration, partition formatting and native installation
remain p006/p007 work, not completed by these tests.

Implemented BLKRESERVE with an exact prior BLKGETINFO record, privilege and
O_RDWR checks. ADMIN raw claims exclude competing writers/backing objects;
the registry gate requires one whole-disk open and idle children and excludes
new opens, cache and physical I/O. Loop disks are explicitly file-backed and
cannot enter whole-physical-disk administration. A file owns the claim; each
serialized backend call borrows a thread scope and releases it before return.
Owned reload retains both gates while publishing new child devices.

Reserved reads bypass the cache and writes use explicit claimed direct I/O.
Clean old aliases are invalidated at acquisition and overlapping lines before
writes. Busy/dirty invalidation fails, releasing incomplete acquisition.
Provenance alone is deliberately not treated as raw write authorization.
Final close releases the gate and claim; dup/fork retain the same description.

| Gate | Evidence |
| --- | --- |
| Identity/privilege/access/media | Production devfs host tests cover stale registration/size, EPERM, EBADF, EFAULT, read-only media and file-backed refusal |
| Admission/rollback | Competing parent/child opens, referenced children, opening/inflight/cache users, claim/allocation/invalidation failures reject without leaked claim/gate |
| Owner and foreign I/O | Foreign cache/direct reads and wrong-thread scope refused; owner write/flush/reload succeed; write invalidation and flush errors clear syscall scope |
| Close and removed media | Host final close releases; revoked media refuses writes/query and remains releasable |
| Actual backing registry | Existing swap/loop/mutation suite plus ADMIN owner/foreign write and competing swap tests pass |
| QEMU admission | Read-only mounted target child and active USB root backing reject; child and competing raw opens reject |
| QEMU lifecycle | Owner writes/reads/fsyncs and restores a gap sector, reloads; dup retains until final close; child acquires and is killed; parent can then acquire again |
| Persistence of untouched data | Whole target hash identical before/after; post-release FAT mount and sentinel read succeed |
| Compilation | amd64/pcat/pc98 disk-image builds all exit 0 |

Host `tests/run-storage-foundation-test.sh`, ordinary and ASan/UBSan/leak modes:
41803 assertions each, plus amd64/i386 UAPI syntax PASS. The fixture links
production disk/devfs/partition code, with explicit cache/claim fault stubs;
real backing exclusion is additionally tested in the separate registry suite
and the QEMU run. These counts include reload repetitions, not unique scenarios.
The standalone runner now links the refactored src/kern/io.c and supplies its
host HAL stubs. Initial sandbox LeakSanitizer restriction was resolved by the
authorized unsandboxed run. Async fixture's custom claim-release stub remains
separate through its explicit preprocessor hook.

Logs:
- `/tmp/zedbsd-q164-foundation-complete.log`
- `/tmp/zedbsd-q164-claims.log`
- `/tmp/zedbsd-q164-amd64-final.log`
- `/tmp/zedbsd-q164-pcat.log`
- `/tmp/zedbsd-q164-pc98.log`

Native runner `tests/run-block-admin-qemu.py`, disposable `temp/q164-admin1`:
`result.json`, `commands.log` and `guest.log` retain the commands/results.
`block-admin-probe.c` is a test-only binary copied to /run, not a product command.
Target hash before and after:
`562b7fed5c4a8df7ccfbd978812c354434f26ec970408de631786ae1e77abe00`.
Production image hash before and after:
`c2bc0ba63269cee9014e0184c14a4b3d1305d80b612fb20e43c57002d8601a61`.
QEMU and all build/test handles are terminal. No physical disk was modified.

Public API description: [block administration](../../../docs/reference/block-administration.md).
Next: expose prepared GPT initialization through existing diskpart with one
reservation spanning the complete proposed table, confirmation, writes, flush
and reload. Then generalize admission for partition formatting, implement
FAT32/current-UFS block formatting, and integrate p006/p007.
