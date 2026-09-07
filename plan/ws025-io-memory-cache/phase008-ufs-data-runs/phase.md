# ws025-p008: UFS data run

日付: 2026-09-07

Phase ID: `ws025-p008`

Status: completed; Queue q095; results.md に受け入れ証拠を記録

Parent: [WS025](../ws.md)

依存: ws025-p007

## 目的と境界

FS block 単位の再分割をなくし、mapping が連続する範囲をまとめる。

## 変更対象

- `src/drivers/fs/ufs1/ufs1-vfs.c`
- `src/drivers/fs/ufs2`
- `src/drivers/loop.c`
- `src/drivers/fs/fat.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 実装時点の production UFS driver を選び、既存ファイルの mapping を最大 64 KiB の連続 run にする。WS024 が先行したら統合 UFS に適用する。
2. hole、indirect 境界、部分 block、device max、short/error を扱う。新 allocation の zero/publication はこの段階で変更しない。
3. loop retained extent と FAT backing が要求を再分割する場所を測り、物理的不連続に応じた分割だけを維持する。
4. 旧 block loop は fallback とし、data/metadata の counter を別に記録する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- IO01–IO04、IO09–IO12。連続 64 KiB の FS data run 一回、断片/partial/hole は正しい範囲で分割。
- q086 FS50 の read/write、backing claim、MAP_SHARED、native/overlay 関連セルが通る。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

metadata の回数が残ることは p010/p011 へ記録する。overwrite だけで新規作成も改善済みとしない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
