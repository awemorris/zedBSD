# ws025-p023: kernel の page-vector vmap

## 2026-09-10 現行仕様の訂正

旧実装は完了履歴。67b28ce0ではhal_vmap/vm_kernel_mapとDMA/scratchのvmap fallbackが撤去された。旧機能の存在を現行仕様として主張しない。復活を目標にしない。
[修正後照合](../post-rollback-review.md)を優先し、以下は旧計画・履歴として読む。

日付: 2026-09-07

Phase ID: `ws025-p023`

Status: completed in q119; see [results](results.md) and [owner design](vmap-design.md).

Parent: [WS025](../ws.md)

依存: ws025-p005、ws025-p016

## 目的と境界

非連続な物理 page を連続 kernel VA へ map し、scratch/cache の物理連続依存を緩める。

## 変更対象

- `src/hal/amd64/space.c`
- `src/hal/amd64/space.h`
- `include/hal/hal.h`
- `src/kern/entry.c`
- `src/kern/vm-reclaim.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. memory-design.md の vmap 窓に bounded VA reservation と page-vector map/unmap を追加する。virtual reserve と resident allocation を分離する。
2. page table と全 process の共有 upper-half 公開、SMP shootdown、部分 map の rollback、参照中 unmap 拒否を実装する。
3. 汎用 VA→page 取得を整え、vmap buffer を単一物理領域と誤認する DMA/allocator caller を拒否または vector へ接続する。
4. まず pool の optional backing と worker scratch に接続する。8 KiB reclaim/DMA coherent reserve は物理条件を維持する。
5. amd64 以外は既存 bounded buffer を使い、未対応 vmap を capability で明示する。kernel heap 全体の allocator 置換には拡張しない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- MEM08–MEM13、CACHE06–CACHE09、SG01–SG03。断片物理 page の連続 VA、high PA、partial failure、全 space からのアクセスと TLB を検証する。
- reserve だけでは resident が増えず、unmap/free 後に table/page accounting が戻る。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

仮想 map ができただけで DMA 直接転送可と報告しない。未対応 HAL の通常 I/O を維持する。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
