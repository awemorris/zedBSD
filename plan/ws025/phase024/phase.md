# ws025-p024: SG BIO と xHCI DMA

## 2026-09-10 現行仕様の訂正

旧受け入れは履歴として保持。現行DMA vectorはcoherent物理割当を使い、旧vmap backingはない。現行UAS利用側との整合確認はp029に保持し、旧非連続backingを復活させない。
[修正後照合](../post-rollback-review.md)を優先し、以下は旧計画・履歴として読む。

日付: 2026-09-07

Phase ID: `ws025-p024`

Status: completed in q120; see [results](results.md) and [SG owner design](sg-design.md).

Parent: [WS025](../ws.md)

依存: ws025-p009、ws025-p019、ws025-p023

## 目的と境界

非連続 RAM の連続 LBA run を SG で渡し、条件を満たす device で copy を減らす。

## 変更対象

- `include/kern/disk.h`
- `src/kern/disk.c`
- `include/drivers/dma.h`
- `src/drivers/dma.c`
- `include/drivers/usb.h`
- `src/drivers/pci-xhci.c`
- `src/drivers/usb-storage.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. segment array の total length、block alignment、count、device boundary を検証する SG adapter を追加し、非対応 driver は単一 buffer/分割へ戻す。
2. drv_dma_map の page-vector と device address を扱い、mask 超過には bounded bounce。CPU PA と DMA address を混同しない。
3. xHCI は TRB 境界/数、TD completion、実効 DMA 幅を確認する。BOT の一 command は連続 LBA のままとする。
4. write の DMA backing を immutable snapshot/排他で固定し、再 dirty 世代と cancellation retirement を別に管理する。read は完了まで valid を公開しない。
5. 初期適用は normal I/O。reclaim-safe reserve と timeout 隔離を維持し、copy 数/bytes と TRB cost を実測する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- SG01–SG05、ASYNC01–ASYNC08、MEM12–MEM16。非連続 RAM、high page、boundary、mask、部分/遅い completion を検証する。
- 条件が揃う 64 KiB run の bounce copy 削減を示し、fragmented LBA を一 command と誤計数しない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

lifetime を保てない device/path は bounce を維持する。無条件 zero-copy capability を公開しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
