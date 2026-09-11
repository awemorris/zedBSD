<!-- awesome-plan project=zedbsd record=ws004-p050 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws004/phase050/phase.md`

親: [ws004](https://github.com/awemorris/zedBSD/issues/5)

# ws004-p050: multiple NVMe controllers

Status: completed / cleared q187; WS019 completed in q186
Parent: [WS004](https://github.com/awemorris/zedBSD/issues/5)
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

## Preliminary inventory during q185 (read-only)

All nvme_primary references are currently in src/drivers/pci/pci-nvme.c:
namespace probe, the additional-controller attach refusal, detach/shutdown
claims, and controller publication/unpublication. Per-controller command locks
and lifecycle state already exist, and PCI driver_data stores the controller.
The next queue must replace the singleton lookup/publication with a registry
while retaining the existing probe_busy/detach ownership protocol; merely
removing the attach refusal would overwrite ownership. This is inventory,
not implementation or acceptance. Installer/PC98 normal-path work still runs.


## Implementation design inventory after q186 reads (not yet queued)

- `nvme_probe_namespace()` also hard-codes controller 0 in
  `disk_alloc_nvme_name(disk, 0, namespace_id)`. Registry changes alone cannot
  work: each bound controller needs its own index and therefore a unique
  `nvmeCnN` name. Disk allocation already checks names under its lock. Persisted
  boot selection continues to use PARTUUID, not these enumeration indices.
- Add intrusive next/index fields to the allocated controller, replace
  `nvme_primary` with the registry head, and assign indices under the existing
  registry lock. Use explicit exhaustion handling; no new fixed one-controller
  array. Failed attaches may consume an index; name stability across boots is
  not a promised identity mechanism.
- Namespace probing selects one not-yet-started eligible entry while holding
  registry then command_lock, sets probe_started/probe_busy before releasing
  locks, and restarts its registry search after each result. Do not save an
  unlocked next pointer across probe/teardown: detach may retire that node.
  Keep the existing probe-failure quarantine and detach ownership arbitration.
- Detach/shutdown claims locate the controller by PCI device in the registry,
  then claim detach_busy under command_lock before releasing registry. Do not
  replace this with an unprotected driver_data dereference: PCI clears that
  field only after successful driver detach.
- Publication/unpublication must update only the intended node. Preserve
  per-controller command/DMA/IRQ/recovery ownership already present. A sibling
  remains registered when attach/probe/teardown of one controller fails.
- Reuse the existing lifecycle host fixtures and installed-boot QEMU harness;
  add a second distinct NVMe namespace image in both PCI enumeration orders.
  Confirm both names, root identity, independent sentinels/flush/persistence,
  failure isolation and shutdown. This is design preparation during the PC98
  runtime wait, not implementation or an accepted multiple-controller result.


## Completion

[Results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/phase050-nvme-multiple-controllers/results.md) map the acceptance to current-code host ownership/lifecycle
checks and real two-controller QEMU cases. Both enumeration orders, target-only
boot/login/swap, separate and concurrent writes/flush/readback, persisted data,
namespace-probe failure isolation and normal halt pass. Initialization failure
and timeout/reset ownership isolation pass host ordinary/ASan/UBSan checks of
production functions. This does not claim physical lost-completion/reset tests.
All three supported disk-image builds pass. BUG-016's one-controller admission
and boot-order failure are fixed; this phase is cleared.
