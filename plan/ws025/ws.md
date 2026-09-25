<!-- awesome-plan project=zedbsd record=ws025 -->

# WS025: I/O・キャッシュ・物理メモリ管理の段階的再設計

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-11（p029・p030・p032・p038 をユーザー判断で cleared、WS を閉鎖）
Primary Milestone: MG004
Related Milestones: MG003, MG008
Objectives: O1, O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

物理メモリの handoff と direct map、DMA 制約つきの allocator、buffer cache と write-back、非同期の BIO、先読み、metadata journal、SG DMA、媒体世代を段階的に作り直し、既定の構成にする。

## 結果

E820/UEFI のメモリ handoff から、range 別の allocator、I/O pool、UFS と FAT の cache と一括 metadata、flush 世代、全体のメモリ予算と reclaim、非同期 BIO、先読み、exec の cache 共有、page-vector vmap、SG BIO と xHCI DMA、NVMe の多重発行、UAS、driver の配置と命名の統一、AX211 の回復、承認済みの HAL 統合までを完了した。設計は [io-design.md](io-design.md)、[memory-design.md](memory-design.md)、HAL の提案と議論は [hal-interface-proposal.md](hal-interface-proposal.md)・[hal-trap-interface-discussion.md](hal-trap-interface-discussion.md)。

## 制限・移管

p028（user page の直接 I/O）は取り消した（[user-page-io-removal-audit.md](user-page-io-removal-audit.md)）。実機の性能は未測定の項目がある。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws025-p001 | baseline・観測値・契約 | cleared（q088） |
| ws025-p002 | BIOS E820 / UEFI memory handoff | cleared（q089） |
| ws025-p003 | RAM 専用 direct map と bootstrap | cleared（q090） |
| ws025-p004 | range 別 allocator と DMA 制約 | cleared（q091） |
| ws025-p005 | 高位 RAM の通常公開と受け入れ | cleared（q092） |
| ws025-p006 | I/O pool と exec chunk | cleared（q093） |
| ws025-p007 | buffer cache の連続 run | cleared（q094） |
| ws025-p008 | UFS data run | cleared（q095） |
| ws025-p009 | USB 64 KiB reservation | cleared（q096） |
| ws025-p010 | UFS allocation adapter と metadata cache | cleared（q097） |
| ws025-p011 | UFS allocation batch と公開順序 | cleared（q103） |
| ws025-p012 | FAT clean cache と chain generation | cleared（q098） |
| ws025-p013 | FAT 操作内の同期 metadata batch | cleared（q104） |
| ws025-p014 | flush 世代と永続化証明 | cleared（q099） |
| ws025-p015 | file object の保持寿命と read cache | cleared（q105） |
| ws025-p016 | 全体メモリ予算と clean reclaim | cleared（q106） |
| ws025-p017 | dirty/error 世代と下位 drain | cleared（q107） |
| ws025-p018 | 既存 data の write-back と throttle | cleared（q109） |
| ws025-p019 | 実際に非同期な BIO submission | cleared（q108） |
| ws025-p020 | bounded 先読み | cleared（q110） |
| ws025-p021 | 統合 UFS の metadata journal/write-back | cleared（q116） |
| ws025-p022 | exec の cache snapshot 共有 | cleared（q118） |
| ws025-p023 | kernel の page-vector vmap | cleared（q119） |
| ws025-p024 | SG BIO と xHCI DMA | cleared（q120） |
| ws025-p025 | storage sense と媒体世代 | cleared（q121） |
| ws025-p026 | 統合受け入れと既定化 | cleared（q122） |
| ws025-p027 | 条件付き NVMe 多重発行 | cleared（q139） |
| ws025-p028 | 条件付き user page 直接 I/O | canceled（user page 直接 I/O は取消し） |
| ws025-p029 | 条件付き UAS driver | cleared |
| ws025-p030 | 条件付き IMOD 実機比較 | cleared |
| ws025-p031 | ドライバ配置・命名・コーディング規約の統一 | cleared（q188） |
| ws025-p032 | リファクタリング後の PC-98 QEMU 起動回帰修正 | cleared |
| ws025-p033 | AX211 の自動接続・DHCP・復旧回帰修正 | cleared（q124） |
| ws025-p034 | AX211 直接操作反復後のエラー出力と復旧 | cleared（q124） |
| ws025-p035 | 複数無線インタフェースの自動接続と選択遅延 | cleared（q124） |
| ws025-p036 | approved HAL space API names | cleared（q245） |
| ws025-p037 | approved HAL consolidation | cleared（q264） |
| ws025-p038 | fixed HAL syscall and fault callbacks | cleared |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws025/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
