# ws025-p026: 統合受け入れと既定化

日付: 2026-09-07

Phase ID: `ws025-p026`

Status: completed in q122; [results and acceptance](results.md).

Parent: [WS025](../ws.md)

依存: ws025-p005–p025 の必須成果。条件付き p027–p030 は独立

## 目的と境界

全 RAM・cache・write-back・USB-root を組合せ、機能ごとに既定化して互換路を整理する。

## 変更対象

- `plan/ws025-io-memory-cache`
- `plan/ws024-unified-ufs`
- `src/kern`
- `src/drivers`
- `platform`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. acceptance.md の mandatory 行列を source/config/hash 固定で実行する。p005 高位 RAM と p018/p021 write-back、p020 先読み、p024 SG を順に組み合わせる。
2. new/append/overwrite/small-write、cold/warm、fsync 境界、memory pressure/USB/WLAN/HID を反復し、性能と失敗率・resident/dirty/quarantined を記録する。
3. 機能・mount・device ごとに既定 ON を選ぶ。FAT metadata は同期 batch、非対応 HCD は小 transfer、writable exec は copy など実効 policy を明示する。
4. disable/drain/unmount/shutdown/crash/replay と image migration を最終確認し、必要な実機セルを fresh artifact で実施する。
5. q087 の暫定 per-call large allocation と不要 adapter を除去する。正当な低メモリ/非対応 fallback は保持し、WS/master/follow-up を結果で更新する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- acceptance.md の必須 MEM/IO/META/FLUSH/CACHE/WB/ASYNC/READ/EXEC/SG/REC/CRASH、q086 FS50、q087、Wi-Fi30 の該当回帰と x86 build が通る。
- 新しい RAM 利用と永続化保証、各性能差を独立の evidence にする。実機未確認セルを成功済みにしない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

既定化条件が足りない機能は opt-in のまま保持し、その理由と再開条件を記録する。WS 全体を完了と誤表示しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
