<!-- awesome-plan project=zedbsd record=ws025-p036 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase036/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p036: approved HAL space API names

Date: 2026-09-10
Phase ID: ws025-p036
Status: cleared (q245)
Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

## Authorization and scope

User explicitly approved the following renames in hal.h and all consumers:

| Previous | Approved |
| --- | --- |
| hal_space_create | hal_space_create |
| hal_space_destroy | hal_space_destroy |
| hal_space_switch | hal_space_switch |
| hal_space_map | hal_space_map |
| hal_space_prot | hal_space_prot |
| hal_space_prot_query | hal_space_prot_query |
| HAL_SPACE_PAGE_PRESENT | HAL_SPACE_PAGE_PRESENT |
| HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_ACCESSED |
| HAL_SPACE_PAGE_DIRTY | HAL_SPACE_PAGE_DIRTY |
| hal_space_unmap | hal_space_unmap |
| hal_space_query | hal_space_query |
| hal_space_clear_flags | hal_space_clear_flags |
| hal_space_flush_tlb | hal_space_flush_tlb |
| hal_space_flush_tlb_range | hal_space_flush_tlb_range |
| hal_space_get_page_size | hal_space_get_page_size |
| hal_space_get_user_range | hal_space_get_user_range |
| hal_pmem_get_stats | hal_pmem_get_stats |
| hal_pmem_stats | hal_pmem_stats |

No compatibility aliases. Update actual source, declarations, all architecture
implementations, build/tool references and reusable test sources/extractors.
Preserve historical result logs, disposable copies and archived plans as evidence.
No other hal.h API modifications are authorized by this rename request. Changes
to that interface require explicit user approval; do not infer it from autonomy.

## Verification

Search active production/tests for exact old identifiers; confirm all 18 mappings
have matching declarations and definitions. Run focused VM and syscall fixtures,
then amd64/PCAT/PC98 disk-image builds sequentially. No aggregate check or full
legacy test run. Preserve all unrelated edits. Other architectures receive the
same source rename; do not claim their runtime acceptance from x86 builds.

## Relationship to vmap review

This naming correction precedes further HAL_SPACE_SYS consolidation. The existing
[review](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase028-direct-user-io/hal-vmap-review.md) remains valid under the new
hal_space_* names. This phase does not introduce new public APIs or implement
that separate architectural refactor.

## Result

All 18 specified names migrated across 44 source/test files. No old identifiers
remain in active source/test scans. Focused actual VM fixture passes ordinary
191650 checks and sanitizer 191654 checks; syscall stories pass flags off/on.
amd64, PCAT and PC98 disk-image builds exit 0. No other architecture runtime claim.
[Results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase036-hal-space-api/results.md). Additional allocation signature and SYS integration remain
in the [review proposal](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/hal-interface-proposal.md); they are not covered by
this naming phase's completion.
