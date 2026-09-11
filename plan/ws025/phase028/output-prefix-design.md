# Direct output publication: source contract after q241

Status: VM/file prefix entry and writable uaccess view implemented (q243); syscall integration pending.

## Evidence from current owners

`file_io_transfer` may select ordinary backend reads if there is no pinned
coherent cache. Even `vm_object_read_coherent_useful` used raw destination
fallback when miss-run publication/scratch failed. q241's new
`vm_object_read_coherent_prefix` shares the actual cache algorithm but refuses
that fallback after cleanup. The hostile-backend fixture proves why selection
must be explicit. Resident hits copy only held pages; successful miss runs
copy only confirmed scratch prefixes; later errors return earlier completed
bytes, leaving the remaining destination intact.

`file_io_begin_cred` marks synchronous operations only for writing.
`file_io_complete` preserves read results after ending leases/position state;
its post-transfer fsync-error replacement applies to synchronous writes.
Maintain that property in direct-read integration.

## Next bounded integration

1. Implemented in q242: a separate file transfer entry accepting only READ/PREAD transactions
   with internal_flags zero, pinned read_object, held_content_read and
   coherent_read. Validate capability before touching the destination. Share
   normal offset/EOF/readahead/transfer accounting, but invoke only the new VM
   prefix operation. Lost identity/errors must never jump to raw backend.
2. Implemented in q243: a writable uaccess view using the full-unmap lease, with WRITE pin
   captured before file locks (COW already broken by pin validation). All
   aliases stay absent until confirmed transfer and file completion. Preserve
   the untouched suffix through the strict transfer primitive, rather than
   rollback from a second full-size snapshot.
3. Acquire output ownership before file_io_begin. If the eventual file
   transaction lacks strict capability, use ordinary staging/copyout while
   retaining the view lease until completion; never try to acquire VM lease
   while holding file locks. Alternatively decline early by a validated file
   capability, but not by filesystem-name guessing.
4. A strict failure with zero published bytes may be retried using staging
   only if file position remains unchanged. A partial positive result ends
   the syscall normally. Retire borrowed VA then full-unmap lease then pins.
   Mark private output dirty conservatively; no suffix bytes are fabricated.
5. Test real transaction wrappers with hostile short/error backend, absent
   cache, scratch refusal, cold/warm reads and unchanged suffix. Then native
   READ/PREAD, EOF, unaligned fallback, remap/exit and copied-byte/CPU comparison.

This is not output acceptance or a claim that generic backend callbacks have
prefix safety. Input default remains off based on q240 measurements.
