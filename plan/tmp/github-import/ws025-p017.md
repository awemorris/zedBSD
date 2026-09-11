<!-- awesome-plan project=zedbsd record=ws025-p017 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase017/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p017: dirty/error 世代と下位 drain

日付: 2026-09-07

Phase ID: `ws025-p017`

Status: complete (q107); evidence in results.md

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p011、ws025-p013、ws025-p014、ws025-p016

## 目的と境界

write-through を維持しながら dirty owner、error 観測者、再遅延しない内部 I/O を実装する。

## 変更対象

- `src/kern/buf.c`
- `src/kern/vm-object.c`
- `src/kern/file.c`
- `src/kern/disk.c`
- `src/drivers/loop.c`
- `src/drivers/fs/fat.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. dirty 世代と書戻し中世代、inode/mount/disk/media owner を持つ索引を作る。enqueue のたびに全 dirty list をソート走査しない。
2. 実 errno と error sequence を保存し、cursor は open file description ごと、mount sync は独立 observer とする。dup と独立 open を区別する。
3. file→FS→loop→FAT→leaf の内部 context に drain/ordered metadata と必要な claim/世代を引き継ぐ。thread-local だけに依存しない。
4. delayed API の adapter を through に接続し、実効 through と表示する。fsync の内容世代 snapshot と新 error の報告を競合なく処理する。
5. close 後も dirty owner が残る場合の寿命と後続通知を定義する。close を一律 fsync へ変更しない。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- WB01–WB05、FLUSH01–FLUSH06。複数 fd、dup、fsync 中の error、retry failure、close/exit を検証する。
- 上位 drain が下位で dirty queue に戻らず、既存 write-through の返値/永続化を維持する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

error が一つの fd で消えて他の observer に届かない場合は既定化へ進めない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q107 selected design

The inspected production owners are file_io / file_fsync, inode_sync,
vm_object's dirty/write generations, mount_sync and m_write_epoch, buf dirty
flags and the disk BIO completion/persistence frontiers. Current errors are
single transient integers: successful retry can clear them before another open
file description observes the failure. Buffer sync currently rescans all cache
hash buckets for each dirty candidate. Existing pwrite_internal flags carry
stacked content ownership but BIO has no explicit drain context yet.

1. Add a small IRQ-safe error ledger containing a monotonically increasing
   sequence and the actual errno, with coherent snapshots and explicit observer
   cursors. Zero-initialized owners are valid. Saturation stays conservatively
   observable; it cannot turn a later failure into apparent success. A cursor
   advances only to the captured snapshot, never a subsequently arriving error.
2. Store ledgers on authoritative inode/mount/disk owners and a cursor on each
   open file description; dup naturally shares it. Keep mount sync's observer
   independent. Report newly observed historical writeback failure once per
   observer and preserve any current drain/retry failure regardless of cursor.
   A fresh description starts with an unobserved ledger so retained failures
   are not erased by close/reopen. Generic argument/capability errors do not
   become writeback failures. Preserve real lower errno and retained dirty data.
3. Keep existing data owners and generations. Add intrusive dirty membership
   maintained at dirty/clean transitions rather than sorting or rescanning the
   entire clean cache on enqueue. Snapshot/pin candidates before releasing the
   index guard; eviction cannot retire a selected descriptor. Respect existing
   cache -> buffer lock ordering and never acquire a lower rank from the dirty
   index guard. VM page membership remains under its object lock.
4. Add an explicit internal I/O context for through/drain/ordered operations,
   originating owner and observed generation. Pass it through the file/backend,
   loop and disk/BIO boundaries with a validated synchronous adapter. Existing
   claim references and content transactions retain their lifetime; a context
   is not a way to bypass a backing claim. The synchronous adapter completes
   before borrowed context lifetime ends. Async ownership transfer is p019.
5. Delayed entry points remain explicit through adapters; unsupported modes or
   flags return an error. Current FS metadata ordering and device flush behavior
   remain authoritative. No delayed scheduling is enabled in this phase.

Verify primitive snapshot/overflow/concurrent observer races first, then real
file/VM/inode and block retry failures, independent opens/dup, close/reopen,
mount observation and dirty membership lifetime. Verify explicit stacked drain
propagation and ordinary write-through return values. Run the affected host
fixtures, supported x86 builds and the existing FS50/Wi-Fi30/native gate before
closing this queue. No production changes precede q107 authorization; the user
has already authorized successive queues to WS025 completion.
