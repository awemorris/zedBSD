<!-- awesome-plan project=zedbsd record=ws025-p019 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase019/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p019: 実際に非同期な BIO submission

日付: 2026-09-07

Phase ID: `ws025-p019`

Status: complete (q108); evidence in results.md

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p009、ws025-p014、ws025-p015

## 目的と境界

呼出元を device 待ちから分離し、completion/cancel の寿命を一意にする。

## 変更対象

- `include/kern/disk.h`
- `src/kern/disk.c`
- `src/drivers/loop.c`
- `src/drivers/usb-storage.c`
- `src/drivers/pci-nvme.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 同期 submit/wait wrapper と async submission を分ける。最初は preallocated/bounded worker queue で実装し、driver の同期処理を worker が呼ぶ。
2. queue full/未受理と受理後 error の戻り値・callback 規則を固定する。即時 callback/submit return 前完了も扱う。
3. BIO、segment、buffer/page、disk/claim/media/content generation と内部 context を heap/参照で保持する。waiter 解放後の BIO 参照を禁止する。
4. cancel 成功、cancel 不能、detach、遅い完了を一度だけ収束させる。BOT は直列のまま、別 device の worker と需要 I/O の公平性を保つ。
5. flush frontier が out-of-order 完了で飛ばされないよう p014 と統合する。device の native async は別段階で置換可能にする。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- ASYNC01–ASYNC08、FLUSH02–FLUSH06。submit は device latency を待たず、queue/資源が満杯なら規定の結果で返る。
- inline/遅延/他 CPU 完了、取消し race、detach、最終 callback と解放、loop 再入を sanitizer/fixture で検証する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

b_done が存在するだけで async 対応と表示しない。未 retirement DMA は保持する。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。


## q108 selected design and implementation order

Execute p019 before p018. The dependency graph permits this order; independent
per-device workers and owned requests make the stopped-device isolation required
by data writeback concrete before enabling that policy. Existing loop/BOT/NVMe
submit callbacks synchronously wait today, although the BIO completion primitive
also supports deferred callbacks. Keep those driver algorithms and the existing
synchronous caller interface; add a distinct, actually asynchronous interface.

1. Separate disk admission/frontier registration from driver dispatch. An async
   submission reserves a bounded queue position, then admits/pins its BIO and
   write frontier before returning. A full queue or invalid request returns an
   error without callback or partial ownership transfer. Once accepted, even
   driver dispatch rejection terminates through exactly one completion.
2. Use explicitly initialized bounded device endpoints and fixed request/data
   slots, with one worker per endpoint. Start with at most four endpoints and
   four 64 KiB slots each, charged as mandatory I/O resources at initialization;
   no unbounded fallback allocation. Endpoint setup may allocate/start threads;
   submission never allocates or waits for a device. Keep requests FIFO within
   a leaf to preserve write/flush boundaries; independent endpoints progress
   independently. Loop worker lower calls remain synchronous, avoiding recursive
   dependence on another free queue slot.
3. Own each request's BIO and payload until terminal completion and final handle
   release. Writes use the request's private immutable submitted payload; reads
   expose bytes only after terminal success. Keep caller and queue/worker refs
   distinct so inline completion and callbacks releasing the caller's handle
   cannot free storage still used by submit/dispatch. Copy context and retain
   inode/disk/claim owners explicitly. Add claim reference ownership without
   dropping its registry protection before the last owner releases it.
4. A queued cancellation removes the request and completes ECANCELED once.
   Running cancellation returns busy/unsupported and keeps all storage pinned;
   driver/DMA retirement remains authoritative. Detach or epoch mismatch rejects
   queued dispatch; late completion cannot publish success for another media
   lifetime. Do not promise active hardware cancellation. Endpoint disable is
   refused while handles/queued/running requests remain, then stops the worker
   and releases its reserved storage without racing callbacks.
5. Expose prepare/submit/wait/cancel/release with explicit ownership comments and
   terminal result access. Keep legacy synchronous submit/wait through behavior.
   Add focused real disk/frontier/queue tests with host thread scheduling, then
   a QEMU caller that demonstrates return before delayed device work. Check
   resource/ref rollback, queue-full/reject, inline and delayed completion,
   callback final release, cancellation races, detach/media generation, two
   devices and loop reentry. Run affected normal/sanitizer fixtures and the
   supported builds and native/storage gates before closing q108.

The numeric bounds are conservative initial admission limits, not claims of
optimal performance. p027 may replace NVMe worker dispatch with measured native
queue depth. p018 will consume the worker/ownership contract after this phase.

### Worker lifetime refinement

The four bounded worker records are permanent, with threads created lazily on
first endpoint enable. Disable releases an idle endpoint's payload/control
allocation and device reference; the worker remains asleep for reuse. It does
not require unsafe thread termination or a nonexistent kernel join API. No new
worker is created on re-enable. Thread stacks remain ordinary kernel thread
resources, while actual endpoint storage is charged to shared I/O accounting.

Keep a separate device/media epoch from the persistence proof epoch: a cancelled
queued write or ordinary write failure invalidates durability proof but does not
by itself make unrelated requests belong to a different medium. Explicit reset,
replacement and detach invalidation retire both proof and media epochs.
