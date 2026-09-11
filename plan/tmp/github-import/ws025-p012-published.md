<!-- awesome-plan project=zedbsd record=ws025-p012 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase012/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p012: FAT clean cache と chain generation

日付: 2026-09-07

Phase ID: `ws025-p012`

Status: completed; Queue q098; see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase012-fat-cache-cursor/results.md)

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p001

## 目的と境界

FAT の反復 read と traversal を減らし、別 open の更新に追随する。

## 変更対象

- `src/drivers/fs/fat.c`
- `src/drivers/fs`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. sector cache を bounded 複数 clean slot へ拡張する。dirty slot を操作間に増やす変更は p013 以後に分ける。
2. chain cursor/検証結果に共有 chain generation を付ける。offset/index/cluster の整合を確認し、sequential だけに最適化を適用する。
3. 別 open の append/truncate、free/reuse、rollback、media change で generation を更新する。loop retained mapping と claim を維持する。
4. cache bytes、sector miss、chain 歩数、generation invalidation を計数する。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- META08–META10、IO09。順次 read/write の走査を減らし、seek と別 open 変更で stale cluster を使わない。
- boot、直接 FAT、loop backing、failure fixture の返値・mirror 規則を維持する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

dirty slot を増やさないと効果がない箇所は p013 へ戻し、順序変更を隠して入れない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q098 implementation decisions

Retain the existing single mutable 512-byte sector and add four clean 512-byte
slots per mount. Copy old clean content into a replacement slot before switching;
flush a dirty active sector first, with unchanged error and mirror ordering.
Invalidate clean aliases before mutation and clear all slots on external/cache
invalidation. The fixed additional 2 KiB/mount and counters become p016 budget inputs.

A mount-wide monotonic chain generation invalidates open cursors before every
FAT link update, including failed updates and rollback. Saturation disables reuse.
Persist a validated start/index/cluster/tail only for a successful operation;
reuse requires equal generation/first cluster and exact sequential offset.
Backward/random seek and mutation use the existing complete chain validator.
New mount state and cache invalidation retire old validation. A cursor never
bypasses corruption checking that has not already succeeded for its generation.
