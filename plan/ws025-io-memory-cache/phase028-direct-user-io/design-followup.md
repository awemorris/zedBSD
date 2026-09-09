# p028 implementation preparation after q139

Date: 2026-09-09
Status: design preparation only; no p028 implementation/acceptance claim.

## Current owners

- `src/kern/uaccess.c` already captures backing identities for the whole syscall
  before `file_io_begin`. `copyin_pinned`/`copyout_pinned` walk those identities,
  not later virtual mappings. Preserve this remap/exit behavior.
- `vmspace_pin_user_pages` validates/pins under VM metadata locks. Private page
  `pin_count` excludes reclaim/COW metadata operations but does not revoke a
  writable user PTE. Treating this as immutable content is incorrect.
- `src/kern/vm.c` already has private-page BUSY ownership, lifetime references,
  reverse mappings, operation/pin exclusion and retryable fork/COW paths.
  `vmspace_object_page_revoke` provides an example of metadata reservation and
  lock-free PTE shootdown, but removes object mappings: it cannot be copied
  blindly for an anonymous/private-page content lease.
- amd64 `space-vmap.inc` reserves VA and populates newly owned frames. Its release
  frees those frames. A user-page view must explicitly borrow held frames; never
  install user frames into the existing owned-frame release path.
- `file_io_transfer` takes a contiguous kernel buffer. A per-page loop would
  reintroduce small backend calls. A bounded borrowed VA view can retain the
  existing large-transfer and coherent file-I/O path while removing one staging
  copy. It is distinct from direct device DMA, which still has driver/cache
  constraints.

## Bounded staging proposal

1. Add a content-lease capability with explicit unsupported/busy fallback,
   separate from the existing pin lifetime. First eligibility is a complete
   aligned request within 64 KiB, all pages privately backed/resident and no
   shared object pages. Larger/unaligned/shared/unsupported requests retain
   their existing path. Acquire all content ownership before `file_io_begin`;
   never wait for a lease while retaining file/inode transaction locks.
2. For an already captured pin, deduplicate backing owners and account for this
   request's pin multiplicity. A try-upgrade may succeed only when those are
   all existing pins and there is no competing operation/BUSY owner. It keeps
   the caller's pins and references; it must not call the zero-pin I/O acquire
   routine and wait for its own pins. On any refusal, undo all upgrades before
   returning to staging. Validate this protocol before connecting a syscall.
3. Revoke user PTEs for every eligible alias while the content owner prevents
   new pin/COW/reclaim publication. Hold mapping/region metadata only around
   each reservation; shoot down without VM locks and revalidate before clearing
   reservation. Preserve mapping descriptors and COW/protection semantics for
   refault. Fork/unmap/protect races need explicit fixtures, not assumptions.
4. Add an amd64 borrowed-frame vmap operation with a separate ownership mode,
   bounded rollback, cache attributes and shootdown-before-unpin. Other HALs
   report unsupported and use staging. A borrowed view cannot free the frames.
5. Connect an input/write snapshot first through existing `file_io_begin`,
   `file_io_transfer`, growth-limit/O_APPEND and completion logic. Hold the
   immutable view until synchronous file completion; retire view, then content
   lease, then ordinary pins. This removes the user-to-staging copy while
   retaining cache/driver copies whose contracts still require them.
6. Output/read direct publication needs a separate proof: a backend must not
   expose unconfirmed bytes on error/short read. Do not enable it merely because
   the input snapshot works. Audit the eligible coherent read operation's
   exact prefix behavior; either prove it preserves the untouched suffix or
   retain staging until a proper publication/rollback mechanism is available.
   A stub/unsupported output capability is an intermediate step, not completion
   of all p028 requirements or a zero-copy/DMA claim.

## Acceptance and decision boundary

Use production functions in host fixtures for repeated aliases, foreign pins,
partial acquisition/PTE-map rollback, concurrent fork/COW, unmap/remap, exit,
short/error/cancel and map-retirement ordering. Ordinary and sanitizer gates
must verify final owner/pin counts and actual content, including unchanged
unconfirmed output. Native amd64 compares copied-byte counters for the same
eligible request and file/cache semantics; PCAT/PC98 exercise fallback builds
and functional paths. CPU improvement is a measurement result, never inferred
solely from removal of a copy.

This staged proposal supplies concrete implementation questions for the next
p028 Queue. It is not authorization to change shared source while Daybreak owns
q140's build/runtime slot, and is not a substitute for resolving the lease/PTE
and output-publication contracts before enabling those capabilities.
