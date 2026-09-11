# HAL修正の全体案

> 2026-09-10更新：この文書は旧提案・監査の履歴。ユーザーによる67b28ce0の修正が現行基準。再構成をこちらで追加実行しない。HALのVM移設・pmem引数展開等の旧案を復活させない。 [現行照合](post-rollback-review.md)。

日付: 2026-09-10
状態: 2026-09-10ユーザーが全体案を明示承認。space/pmem/SYS/query/common VMの移管と旧API撤去は実装済み。p037はq264で高位DMAを含む受入れを完了。

新たなsyscall/fault整理は別途[議論中](hal-trap-interface-discussion.md)。この資料への過去の承認を新規trap宣言の承認とは扱わない。

## 1. 指定済みの名前変更

[ws025-p036](phase036/phase.md)の18項目を採用する。
空間操作をhal_space_*、状態フラグをHAL_SPACE_PAGE_*、物理統計を
hal_pmem_get_stats/struct hal_pmem_statsへ統一する。旧名の互換aliasは作らない。
実装・利用側・再利用テストへ反映し、q245で3構成のビルドを確認済み。

## 2. HAL_SPACE_SYS対応

hal_space_map/prot/prot_query/unmap/query/clear_flagsで共有システム空間を扱う。
ユーザー空間と共通のページ操作を使い、システム空間はsupervisor PTEを作る。
カーネルイメージ・RAM直接map・MMIO窓は動的VA割当領域から除く。
共有rootは不滅とし、動的に所有する下位tableだけを同期後に回収する。
PTE更新のロックを一本化し、SMP shootdown、部分失敗rollback、A/D観測を維持する。
NULL拒否の除去だけでは不十分で、ユーザー用leaf属性や範囲検証を流用しない。

## 3. hal_vmap_*の撤去

VA予約・再利用、owned/borrowed物理vector、利用中参照、解放順序は共通VMへ移す。
HALは物理割当とhal_space_*によるPTE/TLB/table管理を担う。
既存のuser pin、COW、内容leaseは共通VMの責務として維持する。
scratch、DMA、uaccessをまとめて移行後、hal_vmap_*とspace-vmap.incを撤去する。

## 4. hal_pmem_requestの廃止

ユーザー指定済み。引数案は以下。結果のstruct hal_pmemは維持する。

```c
int hal_pmem_alloc(
    hal_physaddr_t paddr, size_t size, size_t alignment,
    uint32_t type, uint32_t attr, struct hal_pmem *desc);

int hal_pmem_alloc_range(
    hal_physaddr_t paddr, size_t size, size_t alignment,
    uint32_t type, uint32_t attr,
    uint64_t minimum, uint64_t maximum, uint64_t boundary,
    struct hal_pmem *desc);
```

範囲の両端は包含する物理バイト境界。boundaryは跨いではいけない境界。
各実装と内部helperからもrequest構造体を除き、呼出し側・fixtureを更新する。
既存の割当制約・rollback時のdescriptor保持を変更しない。

## 5. 新たに承認を求めるインタフェース変更

```c
/* limitは動的カーネルVA範囲の外側の先頭。 */
void hal_space_get_kernel_range(uintptr_t *minimum, uintptr_t *limit);

/* 既存queryを拡張。paddr == NULLならflagsのみ取得する。 */
int hal_space_query(hal_space_t space, void *vaddr,
    hal_physaddr_t *paddr, uint32_t *flags);
```

動的VA領域をHALから共通VMへ公開し、PA照会を既存queryへ統合する。
hal_kernel_page_lookupを廃止する。SYS照会は直接mapなどの大ページにも対応する。
この2件も2026-09-10の全体案への明示承認に含まれる。

## 6. 順序と検証

1. 指定済み命名と物理割当引数展開。
2. 承認後、SYSページ操作と共通VM管理。
3. scratch/DMA/uaccess移行と専用HAL API撤去。
4. 関連限定fixture、amd64/PCAT/PC98ビルド、必要なQEMU機能確認。
5. 直接I/Oの比較計測へ復帰。

hal.hへの未提示変更を実装判断で追加しない。必要になれば差分・理由を先に提示する。
全体案は承認済み。有限Queueで順次実装する。
