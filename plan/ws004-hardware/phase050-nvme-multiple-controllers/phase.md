# ws004-p050: multiple NVMe controllers

Status: planned; required within the active Priority goal after WS019 completion
Parent: [WS004](../ws.md)
Authorization: user instruction 2026-09-09; finish the installer first.

Resolve BUG-016. q160-select1 boots the right EFI loader but the kernel accepts
only the first NVMe controller; an earlier auxiliary controller prevents the
installed boot0 PARTUUID from resolving. The current nvme_primary singleton
must not decide which physical disk is usable.

Inventory every singleton user before implementation, including attach, namespace
registration, IRQs, queue ownership, DMA, recovery, detach and shutdown. Define
a per-controller registry with unique namespace names and independent lifecycle
ownership. Partial attach failure must unwind only that controller. A failed or
reset controller must not stop healthy controllers or reuse their DMA/requests.
Keep stable disk identities independent of enumeration-based device names.

After all installer modes and source/tree-copy acceptance complete, detail the
code design and place this phase in a finite queue under the standing autonomous
authorization. Do not drop it into Future or mark the active goal complete while
this requirement remains unfinished. It is not a prerequisite to finish WS019.

Acceptance: reuse q160's two-NVMe reproduction in both enumeration orders;
boot installed root/swap from either controller, inspect both disks, exercise
independent and concurrent read/write/flush with separate sentinels, failed attach,
timeout/reset isolation and normal shutdown/reboot persistence. Verify resource
ownership on failed initialization and release. Preserve existing single-device
acceptance and record practical supported limits without another one-device stub.
