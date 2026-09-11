<!-- awesome-plan project=zedbsd record=ws025-p016 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase016/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p016: 全体メモリ予算と clean reclaim

日付: 2026-09-07

Phase ID: `ws025-p016`

Status: completed (q106); shared physical ownership, indexed/slab file cache, clean reclaim and worker reserve verified

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p005、ws025-p015

## 目的と境界

増えた RAM を需要に応じて cache に使い、VM/DMA が必要としたときに返せるようにする。

## 変更対象

- `src/kern/vm-reclaim.c`
- `src/kern/vm-object.c`
- `src/kern/buf.c`
- `src/kern/entry.c`
- `src/hal/amd64/page.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. managed RAM 合計から resident page/buf/metadata/pool/DMA と低位 reserve を計上する。仮想予約、hole、隔離資源を free cache と数えない。
2. VM pressure から clean buf/file page を回収する入口を作る。registry/global VM lock を離してから FS 操作へ進む。
3. mapping/op/pin/dirty と競合する object eviction を検証し、最後の cache reference と inode/mount を正しく解放する。
4. dirty worker の最低資源・device ごとの reserve を先に設けるが、write-back はまだ有効にしない。予算変更は drain/resize の成功後に確定する。
5. amd64 の高位 page 優先で低位 DMA を圧迫しないことと、pcat/pc98 の小容量で予算が縮むことを確認する。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- CACHE05–CACHE09、MEM10–MEM16。working set を増やすと cache が成長し、VM pressure で clean bytes が返る。
- last-close eviction、unmount、pinned/isolated、低位 DMA reserve の accounting と前進を確認する。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

dirty reclaim を global lock 下の同期 USB I/O で解決しない。clean-only 段階での不足は明示して p017/p018 へ引き継ぐ。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q106 selected implementation

The current VM registry has bounded ownership from p015. The block cache already
accounts page-rounded data/header slabs, but its automatic cap stops at 16 MiB.
VM page descriptors still use the fixed-heap/fallback allocator and pages are
linearly searched. I/O pool backing and coherent DMA remain separate allocations.
These are the concrete integration points; do not introduce another content cache.

1. Add one memory-cache accounting owner, initialized from managed physical RAM.
   Its initial total target is managed RAM / 4, with a separately visible free-RAM
   floor (managed RAM / 64, bounded to 64 KiB–8 MiB). Track pending allocation and
   resident ownership separately. Charge real page-rounded block/data/metadata,
   file-page metadata, pool and DMA backing once; mandatory VM/DMA/pool ownership
   can exceed the soft target but then excludes optional growth. Do not count
   holes, virtual reservations, dirty/pinned pages or quarantined DMA as free.
2. Make optional file-cache growth consume shared credits instead of the p015
   16-page retention ceiling. Keep the bounded 16-page I/O vector and bounded
   cache-object count. Add an ordered page index and page-backed metadata storage
   so increased RAM does not imply a linear lookup or a fixed-heap bottleneck.
   Preserve the existing page list for finite sync/resize scans and maintain both
   identities through fault, orphan/abort, invalidate and reclaim.
3. Connect buffer reservations to the same accounting and remove the automatic
   16 MiB ceiling; explicit CONFIG_BUF_CACHE_KIB remains a component cap. Existing
   component controls do not bypass the shared admission target. Pool and coherent
   DMA are mandatory resident categories, including buffers awaiting retirement.
4. Add a nonblocking clean-only reclaim path before private-page swap/writeback.
   It may free idle unmapped/unpinned file pages and clean block buffers, without
   filesystem I/O, waiting on an active read or holding a global VM lock across
   lower-layer operations. Mapped/pinned/dirty/busy pages are conservatively
   ineligible; existing mandatory mapping semantics remain intact. Empty object
   close/drain stays at p015's explicit safe lifecycle checkpoints.
5. Reserve bounded worker scratch before enabling any delayed writes; existing
   per-device USB control/bulk reserves remain device-owned. Publish effective
   reserve availability and do not count a missing reservation as implemented
   writeback capability. Policy remains write-through through this phase.
6. Expose a read-only accounting snapshot plus a bounded cache-target control.
   During shrink, gate new optional admission against a pending target, drain only
   eligible clean memory, and publish the new target only if ownership fits.
   Failure restores the old target and reports EBUSY; reclaimed clean copies need
   not be recreated. No dirty data or pending DMA is discarded to force success.

Use the existing amd64 high-first allocator rather than introducing a second
physical-memory owner. Account explicit reserve/mandatory bytes separately from
uncommitted virtual address space. The new target is a default policy, not a new
RAM reporting cap; validate adjustments against managed memory and the reserve.

Host fixtures cover credit admission/cancellation, concurrent reservations,
mandatory over-target ownership, successful/failed shrink, page-index mutation,
metadata exhaustion, dirty/pin/busy exclusions and no-I/O clean reclaim. Native
cells grow beyond 64 KiB, show warm read reuse, shrink under pressure, preserve
mapped/dirty data and verify low/high RAM behavior with normal production defaults.
Supported x86 builds and affected existing cache/VM/storage gates remain required.
