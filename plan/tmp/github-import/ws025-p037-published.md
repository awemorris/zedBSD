<!-- awesome-plan project=zedbsd record=ws025-p037 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase037/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p037: approved HAL consolidation

## 2026-09-10 現行仕様の訂正

旧q264完了は当時の実装の履歴。現行67b28ce0はHAL_SPACE_SYSを保持し、VMへのvmap管理移設とpmem引数展開を撤回している。hal_pmem_request構造体引数が現行仕様。旧案へ戻す修正を行わない。
修正後照合（ローカル資料: `../post-rollback-review.md`）を優先し、以下は旧計画・履歴として読む。

Date: 2026-09-10
Status: completed (q264); approved HAL migration and focused/native acceptance complete. See results.md for scope limits.
Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)
Authority: user explicitly approved [the full proposal](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/hal-interface-proposal.md).

## Implementation sequence

1. Remove hal_pmem_request from declarations, architecture allocators, helpers,
   consumers and reusable fixtures. Expand paddr,size,alignment,type,attr in that
   order, then existing min/max/boundary and descriptor. Preserve allocator
   validation/ownership/rollback behavior. Focused allocation and VM tests, builds.
2. Add approved kernel VA range query and physical-address output to space query.
   Implement HAL_SPACE_SYS through common page operations with supervisor flags,
   shared-root lifetime, consistent table locks and full shootdown/rollback.
3. Move VA/vector/lifetime ownership into common kernel VM. Migrate scratch, DMA,
   uaccess together; remove hal_vmap_* and hal_kernel_page_lookup after migration.
4. Focused common/architecture checks, supported builds and disposable QEMU
   scratch/DMA/direct-view regression. No new public HAL APIs outside proposal.

A finite queue selects an implementation increment; intermediate passing checks
do not clear the whole phase. All steps and consumer migrations must complete.

## q248 system operation boundary

Unify amd64 SYS mutations and existing temporary vmap table ownership under one
serializer/list. Dynamic mutations are confined to the planned kernel VA window;
query can resolve existing upper-half page/large-page mappings without mutating
fixed mappings. Expose PA through query on all HALs; retain aligned user queries.
Kernel range is empty on architectures whose dynamic SYS mapping is not yet
implemented. Existing vmap lifetime remains temporarily until common VM migration;
its table updates share the SYS owner to prevent concurrent incompatible walkers.
Test syscall-independent page operations and existing VM/scratch regressions,
then supported builds. Do not call the full common VM migration complete.

## q249 common owner

Implement vm_kernel_map_* in common vm.c with kernel VA range from HAL, owned or
borrowed frame vectors, and explicit reserve/populating/live/retiring state. The
bounded slot/size policy initially matches existing scratch acceptance. A leaf
metadata lock protects state only; no allocation/HAL calls while held. Release
retires SYS mappings before freeing owned frames; borrowed frames remain caller
owned. Failure rolls back only installed mappings and retains a retryable reserve.
Migrate all production scratch/DMA/uaccess consumers and reusable fixtures, remove
hal_vmap and hal_kernel_page_lookup from hal.h/amd64. DMA PA lookup uses SYS query.
Preserve default-off user view flags; unsupported dynamic kernel ranges fall back.

## q250 native owned-frame acceptance

Run the migrated link-only vmap-kernel probe on a disposable amd64 USB image with
8 GiB and four CPUs. Verify table allocation rollback, high fragmented frames,
existing/new user-space visibility, AP visibility, exact physical-free restoration
and forced noncontiguous scratch fallback. Retain guest logs and source identity.
Restore ordinary kernel with forced relink and verify wrapper symbols absent.
This bounded queue does not authorize the pending trap/syscall API proposal.

## q251 input-view migration acceptance

Use the current input-view guest against default and input-view-enabled amd64
images, sequentially on disposable QEMU USB images. Check data/readback, post-view
CPU writes, unaligned fallback, append, short RLIMIT write and counter attribution.
Compare identical guest hashes and record CPU/wall totals without treating a
single pair as physical performance evidence. Restore ordinary image after the
experimental build. Native DMA acceptance remains separate.

## q252 contiguous mapping runs

Allocate/capture the entire vector before publishing mappings. Group consecutive
physical pages without unsigned wrap, use one existing hal_space_map per run,
and track only successfully installed pages for rollback. HAL range mapping
retains its own atomic failure rollback. Free owned frames only after all
successful runs have been unmapped. Test contiguous, fragmented and repeated
PA runs, allocation failures and later-run map failure/retry. Supported builds.

## q253 native input after coalescing

Repeat the current ordinary/input-enabled pair after q252. Require identical guest
hashes, exact counter attribution, data/readback and existing boundary checks.
Record CPU/wall totals against q251 without overstating single-pair evidence.
Preserve default-off policy until performance acceptance; restore ordinary build.

## q254 native DMA acceptance

Build link-only sg-native probe with current writeback guest fixture. Run UEFI
8 GiB / four CPU USB-root plus disposable USB journal filesystem. Require SG
probe's high 16-segment DMA markers and writeback/fsync/remount data verification.
Restore ordinary kernel by forced relink and verify no wrapper symbols remain.
Review accumulated p037 evidence before declaring phase complete.

## q263 explicit QEMU high-DMA capability

For the known QEMU xHCI AC64 controller only, link-only registration wrapper
creates a 64-bit DMA device before HCD start/allocation. Retain this test-owned
device for the HCD's entire guest lifetime; never free it with live buffers.
Use existing constrained allocator injection to force high fragmented pages.
Require actual AC64 capability log, bits=64 vectors with all addresses >4 GiB,
and native USB writeback/fsync/remount data verification. Restore ordinary build
and check wrappers absent. This validates common/HAL/HCD high-DMA operation,
not a production policy enabling 64-bit masks for all PCI devices.

## q264 diagnose high fixture

Record failed forced range allocations and actual segment count before assertion.
If exact-PFN conflicts are demonstrated, select unoccupied test candidates via
bounded retries without relaxing high/nonadjacent requirements. Repeat native
acceptance only on new evidence; restore ordinary build in all cases.
