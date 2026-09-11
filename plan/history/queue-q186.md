# Queue q186: PC98 shared-cache read progress and installation

Date: 2026-09-10
Status: finished
Authorization: standing user approval for autonomous Priority execution and PC98 installation
Timebox: 120 active minutes
Previous: [q185](queue-q185.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p050](../ws019/phase050/phase.md) | completed | Resolve BUG-020 using ownership-safe clean-page reclaim; focused host and PC98 regression; resume two-IDE install and destination-only boot |

Preserve shared read leases and cache identity. Reclaim only when the caller's
cache pin is the sole active owner, with no mapping, concurrent operation or
page hold. Keep dirty/busy/pinned pages. One bounded retry after actual reclaim;
no change to file size limits or swap verification. Host coverage must prove
progress and exclusion, then the 64 MiB graphical reproducer must pass before
another full install. No expansion into exhaustive installer fault injection.

The q186 runtime trace found a second active owner (flags CACHE_REFERENCE,
mappings 0, active 2, refs 3). Track unpublished prefetch operations separately:
they retain only private frames and publish under the object lock, so clean
resident-page retirement may coexist with them. Other readers/faults/mappings
still exclude targeted reclaim. The additional counter is protected by the
registry lock, incremented with prefetch admission and decremented before its
operation/reference release. Host coverage keeps a prefetch pending throughout
a read larger than the cache budget and verifies balanced abort ownership.

Result: host ordinary/ASan/UBSan pressure regression and PC98 isolated read pass.
Actual graphical FAT installation passed on two IDE HDDs; target-only root
login passed with rootfs/data overlay and active swap. User accepts this as
clear; no additional installer abnormal-case campaign. PC98/amd64/PCAT builds
pass. WS019 is completed; see p050 results. Next required work is WS004-p050.
