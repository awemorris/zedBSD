# ws025-p018: 既存 data の write-back と throttle

日付: 2026-09-07

Phase ID: `ws025-p018`

Status: completed (q109); evidence in [results.md](results.md)

Parent: [WS025](../ws.md)

依存: ws025-p017

## 目的と境界

既存割当済み通常 file data だけを opt-in で遅延し、小書込みをまとめる。

## 変更対象

- `src/kern/vm-object.c`
- `src/kern/file.c`
- `src/kern/buf.c`
- `src/kern/disk.c`
- `src/drivers/loop.c`
- `src/drivers/fs`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. mount 単位の実効 policy を導入し、既存 size/割当内の通常 data だけを file object に dirty として受理する。新規 allocation/metadata/journal は同期境界を守る。
2. 最初の syncer は同期 I/O worker とし、age・量・fsync で起動する。対象 dirty 世代を固定し、再 dirty と in-flight 完了を区別する。
3. dirty credit を lease 前に予約し、内部再入へ継承する。high/low 候補 40/20% に加え absolute bytes/age/device budget を持つ。
4. DMA 中の内容を snapshot または排他で固定し、完了で新しい dirty を消さない。下位 drain は p017 の context で遅延を迂回する。
5. fsync/unmount/shutdown/disable は対象世代を drain して flush する。disable 失敗時は元 policy を維持し、停止 device の dirty を捨てない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- WB01–WB10、CACHE06–CACHE09、FLUSH01–FLUSH06。小 write 多数+最後 fsync と毎回 fsync を別に比較する。
- credit 枯渇、signal、同期 worker 不足、device failure、書戻し中の再 dirty、途中 disable で deadlock/内容喪失がない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

まだ mount opt-in に留める。新規/metadata 遅延を一緒に有効化せず p021 へ分離する。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。


## q109 selected design

p017 and p019 are complete. Current normal writes use inode content transactions
which revoke mappings, flush overlapping dirty pages, write the backend and
copy confirmed bytes into cached pages. Cache objects keep an independent read
handle. VM dirty pages already survive failed writeback, but there is no ordinary
write admission credit, mount policy, age worker or delayed content commit.

1. Start with explicit mount-path controls and scalar/status observations.
   Default/effective mode remains through unless a supported writable UFS/FAT
   final-content mount has its resources and coherent writer ready. Root-only
   `vfs.writeback.control` accepts an exact mount path and on/off action; status
   lists effective policy, dirty/reserved bytes, limits, work and errors. This
   avoids pretending to change an overlay mount whose final content owner has
   a different policy. Unknown/read-only/unsupported targets fail explicitly.
2. Reserve optional dirty credits before the file position/inode/content lease.
   Initial global high/low limits are 40/20 percent of cache target, capped at
   64 MiB; a physical device may use at most a quarter of the high limit (16 MiB
   maximum). Fixed 64 KiB admission tickets bound each optional transaction;
   unused credit is returned, and only newly retained dirty pages consume it.
   Failed admission wakes pressure work and takes the established synchronous
   path without waiting while holding a content lease. Mandatory mapped/pinned
   storage retains its existing physical accounting; it is never falsely freed
   or rejected after CPU modification.
3. Add a backend eligibility query for existing allocated data and a private
   write-capable cache handle. Exclude append/EOF growth, holes/new allocation,
   internal drain/ordered/claim transfers and O_SYNC/O_DSYNC from delayed
   admission. Validate the final size/allocation while existing mutation locks
   exclude competing change. Bound a transaction to at most sixteen pages;
   oversize/misaligned requests can use the existing through path.
4. Extend the existing content transaction, sharing its inode publication gate,
   rather than introducing another cache. Preload missing old bytes privately,
   revoke mappings and take page ownership before publication. A delayed commit
   copies bytes and marks fresh dirty generations without flushing an older
   dirty image first. Preparation failure restores admission/state and falls
   back or reports the real error. Reads, pins, truncate and ordinary through
   writes continue through the same coherent object. Retain a writeback owner
   independently of the user's file description and notify inode/mount ledgers.
5. Use a bounded per-device synchronous filesystem syncer with reserved transfer
   scratch and a default-priority kernel thread. A filesystem drain must execute
   above the BIO queue and may invoke its synchronous lower adapters; it must
   not depend on borrowing a second slot from its own blocked queue. Device
   workers and credits are independent. Age/quantity/fsync schedule dirty owners;
   process a captured dirty generation, coalesce adjacent pages into bounded
   payloads and retain newer dirty generations. Existing BUSY/PTE revocation or
   private snapshots prevent DMA from observing mutable bytes.
6. fsync, mount sync, unmount, shutdown and policy disable drain the actual VM
   owner before filesystem/device barriers. Disable closes new delayed
   admission, waits for outstanding tickets and captures/drains its target;
   publish through only on success, otherwise restore writeback admission and
   retain dirty/error owners. No close-equals-fsync policy is added. Worker
   resource/metadata allocation failure is reported with dirty ownership kept.

Implement and verify credit/control/lifetime first, then coherent delayed commit,
then bounded worker/drain and policy transactions. Test WB01–WB10 and CACHE06–09
with mapped/pinned pages, first/cold and repeated small writes, failed retry,
close/reopen, concurrent overwrite/truncate, exhausted credits, disabled worker,
multiple devices, and failed/off retry. Compare repeated small writes followed
by one fsync with fsync after every write; record actual lower calls and bytes.
Run focused ordinary/sanitizer fixtures, supported builds, native cache/writeback
and FS50/Wi-Fi30 regression before completing q109. Metadata/allocation delay
remains p021; this queue does not silently enable it.

### Worker/control refinement after native drain validation

The physical domain resolver follows explicit loop backing references while
retaining intermediate disk-cache admissions, and normalizes partitions with
existing disk-cache ancestry. It does not change `d_parent` or logical offsets.
This preserves independent logical lifetime guards while budgets are shared by
the actual leaf. Sixteen layers bound resolution; cycles or detach reject admission.

Implement a bounded policy table (at most MOUNT_MAX entries) and four physical
worker records. A process-context control mutex serializes policy transactions;
a short registry spinlock publishes states and work requests. Neither is held
across filesystem I/O by a worker. Each worker keeps an independent 64 KiB payload
plus one control page, charged by actual HAL allocation size. Kernel threads are
lazy/permanent and reused; scratch is returned after the last successful disable.
The control page holds the bounded mount snapshot for each pass.

Admission finds an enabled mount and reserves its device ticket under the policy
publication guard, before any file/content lease. An off transaction quiesces the
shared physical budget, waits for outstanding tickets and an active worker pass,
and drains the target mount using its reserved scratch. Other mounts on the same
device temporarily take through admission during this transaction. On failure,
restore admission and retain the policy, dirty owners and resources. On success,
remove only the target policy; keep the shared worker while any sibling policy
remains. Resource free failure must also retain a retryable effective policy.

Normal writes schedule pressure work only at the selected threshold; age work
runs at a bounded interval (initially two seconds). Scheduling every small write
as immediate work would defeat the intended coalescing. Worker failure retains
data and records failure; independent physical workers keep progressing. A worker
uses `vm_object_sync_mount_buffer` above the BIO layer and its own scratch, never a
second slot from its own BIO worker. Full mount synchronization retains VM drain
and public error observation; overlay internal filesystem barriers stay on
`mount_sync_backend` and cannot recursively reenter that drain.

### Remaining lifecycle transaction boundary

Do not implement unmount by simply calling successful policy off before the
ordinary reference checks: a later EBUSY would silently disable a still-mounted
filesystem. Prepare a reversible unmount token which pauses the device and drains
its target while retaining policy/resources. Account for the one policy mount
reference in namespace checks. Failure restores admission without allocating;
commit removes the policy only after failure-capable filesystem checks succeed.
Avoid holding the sleeping policy-control mutex across filesystem callbacks;
explicit paused/unmounting state must reject competing same-device controls while
the token is active. Preserve independent-device control/worker progress.

The file_io tests prove O_SYNC/O_DSYNC bypass of delayed admission. They do not
prove general synchronous-write syscall durability. Current static search found
no other O_SYNC/O_DSYNC handling in src/kern; validate the public open/write
contract and provide an outer completion/error path if these accepted flags need
a final durability barrier. Keep this distinction explicit in acceptance evidence.

### Lifecycle implementation refinement after opt-in native gates

Native USB and NVMe currently prove explicit disable before unmount. Next add a
caller-owned reversible unmount token: pause the physical worker and its credits,
wait for tickets/active passes, and drain with its reserved scratch while keeping
the policy reference. Do not hold the control mutex across filesystem callbacks.
A marked worker rejects competing controls on it, including sibling enable/off;
independent physical workers remain usable. Count the retained policy reference
in both namespace and filesystem teardown checks. Every refusal restores the
original policy without allocation; commit releases policy ownership only after
all failure-capable teardown checks. A post-commit resource free failure may
retain an idle, charged, clean orphan for reuse; ensure its quiesced budget is
reopened before any later enable. Never restore an active worker onto a dead mount.

Shutdown currently calls network/USB/PCI quiesce without its own mount drain.
Add an error-returning preparation boundary before device teardown, with policy
admission closed and actual VM/filesystem barriers completed. Failure must leave
storage available for retry and report to init rather than certify poweroff.
Review concurrent preparation callers and initialization rollback explicitly.
Do not infer shutdown durability from the native explicit-off tests.

The file I/O end API currently returns void. O_SYNC/O_DSYNC are excluded from
optional delay, but syscall completion still needs a checked durability boundary
covering all scalar/vector writes, after releasing content/position leases.
Design an outer completion result that can return a flush error without losing
an earlier partial-transfer error or changing internal DRAIN recursion. Test
failure, retry and multi-chunk writes before claiming synchronous-open support.
