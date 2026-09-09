# p042 results

Completed q175, 2026-09-09.

Common claim finalization now sorts its private canonical physical ranges with
an in-place heapsort and rejects self-overlap with EINVAL before registry
locking. Adjacent ranges remain valid; the caller's logical mapping is never
reordered. A rejected claim remains preparing and can be retried or released.
No extra allocation or filesystem mutation is introduced.

Production-linked claim fixture: 512 generated layouts including reversed
adjacent runs, physical aliases and overlapping randomized ranges agree with
an independent exhaustive-pair oracle. Rejected claims preserve exclusion;
single-range retry succeeds and final release returns disk references to zero.
Existing mutation, revoked-media and lifetime tests also pass.
Normal: /tmp/zedbsd-q175-claims.log. ASan/UBSan with leak checking:
/tmp/zedbsd-q175-claims-san.log. Both terminal exit 0.

Format reservation: 605 checks each in normal and sanitizer modes,
/tmp/zedbsd-q175-reservation.log, exit 0.
Full swap source/manager regression: /tmp/zedbsd-q175-swap.log, exit 0.
amd64, PCAT and PC98 disk-image builds: /tmp/zedbsd-q175-{amd64,pcat,pc98}.log,
all terminal exit 0. No process remains running.

UFS canonical identity, indirect-metadata ownership and symmetric snapshot
exclusion remain before UFS swap admission. p007 now records the next design:
collect UFS-owned indirect metadata as claim-only ranges so this same overlap
check rejects data/metadata collisions without changing logical swap maps.
This phase does not claim UFS swap or complete native installation.
