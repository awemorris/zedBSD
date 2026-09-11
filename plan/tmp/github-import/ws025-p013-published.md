<!-- awesome-plan project=zedbsd record=ws025-p013 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase013/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p013: FAT 操作内の同期 metadata batch

日付: 2026-09-07

Phase ID: `ws025-p013`

Status: completed (q104); focused fault tests, three x86 builds and native acceptance passed

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p012、ws025-p014

## 目的と境界

FAT table/mirror/directory の更新を一操作内でまとめる。操作をまたぐ遅延は有効化しない。

## 変更対象

- `src/drivers/fs/fat.c`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 既存更新への operation adapter を入れ、bounded な sector 旧像/新像、変更 chain と generation、完了状態を保持する。
2. 同じ sector の重複変更をまとめ、chain-before-directory、directory unlink-before-free、primary/mirror の必要な順序で同期書込みする。
3. slot 上限では順序を守って batch を区切る。mirror の結果不確定や部分更新を診断し、検証できない chain を利用/再割当てしない。
4. sync/unmount/rollback と loop backing の cache coherence を更新する。cross-operation metadata write-back は capability を公開しない。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- META08–META10、META04–META07 の FAT 対応。新規/extend/unlink と mirror 各段の故障を注入する。
- 成功後の checker/再 mount、一操作内の command 減少、失敗後の generation と保留状態を確認する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

crash/recovery 根拠なしに mirror deferred を既定化しない。残る操作間の遅延は p021 の評価結果へ分ける。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。


## q104 implementation selection

Start from explicit bounded metadata transactions, not implicit dirty windows
inside the general sector cache. One transaction owns old/new physical 512-byte
sector images and affected FAT entry values, under the existing mount mutation
lock. No pending table bytes survive a transaction return.

- Add a primitive that stages a bounded list of FAT entry replacements, merging
  duplicate sector updates, including packed FAT12 bytes and all mirrors. Preserve
  each mirror's own old image for rollback. Refuse/partition before exceeding slots.
- Flush prior initialization/directory detach work before publication or freeing;
  write the staged sectors and confirm their flush. Restore the captured sector
  images on any uncertain write/flush, invalidate mutable/clean/cursor generations,
  and stop writes if restoration cannot be confirmed.
- Route single-entry updates through the primitive. Combine a newly initialized
  cluster and its old-tail link when their entries share a sector in each mirror;
  keep a separate ordered new-chain-before-tail transaction when they do not.
  This avoids introducing an intermediate durable tail pointing into a not-yet-
  committed table sector just to reduce request counts.
- Batch the maintained free-chain loop in bounded groups while retaining its
  complete old chain for rollback. On a later batch failure, restore earlier
  completed groups before any caller restores directory reachability.
- Inspect directory update callers and enforce detach-before-free and chain-before-
  directory ordering at their actual publication points. Merge same-sector changes
  only when that ordering remains intact. Do not advertise asynchronous/deferred
  mirror writes or FAT sector atomicity across a power failure.
- Capture deterministic create/extend/unlink request counts, then exercise FAT12,
  FAT16, FAT32, logical-sector scaling, packed-entry crossings, mirror failure,
  batch splitting, rollback failure, retained cursor generations and loop backing.
  Run focused ordinary/sanitizer tests, supported builds and disposable native
  storage acceptance sequentially. Review at 90 active minutes and continue.


Measured refinement during q104: one-cluster transactions reduced write count but
left 65 sync calls per 16 KiB create/append. The final implementation prepares a
bounded zeroed private chain (at most 32 entries, reserving a tail-link entry when
needed, and normally at most 64 KiB initialization), commits it as one same-sector
image per mirror, and links a cross-sector old tail only afterward. This retains
the explicit transaction contract while avoiding per-cluster barriers. Directory
LFN/SFN runs share the sector-image engine, with the SFN publication sector last.
