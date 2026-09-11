<!-- awesome-plan project=zedbsd record=ws025-p015 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase015/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p015: file object の保持寿命と read cache

日付: 2026-09-07

Phase ID: `ws025-p015`

Status: completed (q105); bounded cache lifetime, fault/concurrency and native acceptance passed

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p006、ws025-p008

## 目的と境界

最後の read/close 後も予算内で内容を保持し、read/write/mmap の一貫性を保つ。

## 変更対象

- `src/kern/vm-object.c`
- `include/kern/vm-object.h`
- `src/kern/file.c`
- `src/kern/vmspace.c`
- `src/kern/overlayfs.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. mapping/op/cache の reference を分け、最後の mapping で clean object を即破棄する現行処理を改める。inode/file/mount の保持と eviction を対にする。
2. 通常 read から coherent object を使い、miss は最大 64 KiB の内部 populate に進む。populate が同じ page-cache path に再帰しない purpose/context を使う。
3. write-through は内容世代と resident page を更新する。MAP_SHARED、truncate、copy-up、media change の invalidation と lease 順序を維持する。
4. loop claimed backing は従来の拒否/迂回を保つ。p016 前も暫定の bounded bytes/object 数を持ち、無制限の cache reference を残さない。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- CACHE01–CACHE05。最後の close 後の再 open で cache hit、予算内 warm data の device read ゼロ。
- MAP_SHARED/通常 write/別 fd/truncate/copy-up、populate 中の失敗と unmount で参照漏れと stale data がない。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

所有 inode が不明な object や再帰 fill は有効化せず修正する。既存 block cache hit を新規改善の証拠に数えない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q105 implementation selection

The existing registry already distinguishes mapping_count and active_operations,
but its only zero-mapping retention state means failed writeback. Add a separate
clean cache-reference classification; never relabel a failed dirty writeback as
a successful clean retention. Ordinary reads acquire/publish cache admission
before taking inode/position locks, then use the existing operation reference and
content-read lease across transfer. Internal VM/loop/formatter traffic bypasses
optional admission. Allocation/admission failure leaves ordinary backend I/O
available, but cannot bypass existing dirty shared pages.

Initial retention is bounded by 32 cache objects with at most 16 optional resident
pages per object (2 MiB payload upper bound); mapped/pinned mandatory VM pages
are separately owned and may prevent optional cache retention. Do not impose
this optional retention ceiling on successful mmap faults. p016 replaces the
provisional policy with managed-RAM accounting.

Cache misses collect at most 16 adjacent pages and borrow the existing 64 KiB
I/O pool, stopping at resident/busy/EOF boundaries. Allocate before publishing
BUSY, validate generations, perform one FILE_IO_VM_OBJECT backend read, and
publish only initialized pages. Failed/short reads release or mark every acquired
page consistently; resident dirty pages always remain authoritative. A bounded
clean eviction or direct leased fallback keeps large sequential reads progressing
when the optional cache budget is occupied.

Unmount and backing-claim admission explicitly discard eligible clean cache
references before their existing busy checks. Detach selection runs under the
registry lock, while file close and filesystem work run after releasing it.
Mapped, active, pinned, dirty or failed-writeback objects remain busy. The cache
key remains file_vm_inode, so overlay open/copy-up retains the actual selected
layer and never merges lower and upper content. Disk lifecycle admission must
also cover cache hits; the broader media-identity recovery policy remains p025.

Focused acceptance extends the maintained formatter/shared-VM host fixture with
backend-read counters, reopened identity, multi-page fill, concurrent operations,
EOF/write/truncate/pin and cache-drain cases. Reuse the selected existing VM
regressions already copied into WS019 temp if needed; do not read .internal.
Then run supported x86 builds and disposable native file/cache acceptance.

Implementation refinement: optional objects use a dedicated read-only backend
handle and verify its final inode before publication. Existing mapping-created
objects retain their prior policy; cache-created objects share subsequent mappings.
A normal write to an unmapped clean cache does not retain the user's writer FD.
The last mapped writer handle is released after successful final synchronization.
Claim cache drain is outside descriptor/inode locks, followed by a post-publication
VM alias check in shared backing-claim admission to close racing admission.
