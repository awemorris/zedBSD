# ws025-p001: baseline・観測値・契約

日付: 2026-09-07

Phase ID: `ws025-p001`

Status: completed; Queue q088, 2026-09-07 — [results](results.md)

Parent: [WS025](../ws.md)

依存: q086/q087 の完了証拠

## 目的と境界

各層の実分割と RAM 制限を再現可能な基準にし、後段の adapter を測る。

## 変更対象

- `src/kern/syscall.c`
- `src/kern/buf.c`
- `src/kern/disk.c`
- `src/kern/vm-object.c`
- `src/hal/amd64/page.c`
- `src/drivers/usb-storage.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 現行 source/config/artifact と q086 FS50・q087・Wi-Fi30 の fixture を対応づける。既存結果を新 HEAD の結果へ流用しない。
2. file transfer、UFS data/metadata、loop、buf、leaf BIO、USB CDB、flush、DMA 確保/隔離の counter を追加する。計数位置と単位を固定する。
3. RAM reported range、usable 合計、highest end、map 済み/allocator 公開量を分離して観測する。counter snapshot と config の読取り API を用意する。
4. 受理/完了/永続化、content/claim/media 世代、lock/worker 所有権の一覧を fixture と照合する。ここでは batch 上限や write-through policy を変更しない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- MEM01–MEM03、IO01–IO04 の現行 baseline を記録し、期待される未達を PASS と混ぜない。
- 同じ workload を反復して p50/p95/p99 と層別 counter を保存する。計数追加で既存返値・失敗通知を変えない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

後段で必要な counter が欠けていたらこの Phase で補う。未分類の分割・flush を減らす変更へ拡張しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
