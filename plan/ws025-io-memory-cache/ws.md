# WS025: I/O・キャッシュ・物理メモリ管理の段階的再設計

Last updated: 2026-09-08

WSID: `ws025`

Status: mandatory p001–p026 completed; p031 in-progress; 必須 p001–p026 と依存 WS024 完了。q122 の統合受け入れ・既定設定・性能観測まで完了。条件付き p027–p030 は採用条件未成立のため今回は見送り、再開条件を記録した planned とする。実機 gate はユーザー判断によるクリア扱い。

Parent: [master plan](../master.md)

追加計画: [p031 ドライバ配置・命名・規約統一](phase031-driver-layout-style/phase.md) は構成合意済み・q123 で実行中。p027–p030 に先行する。q122 の完了記録は維持し、追加 Phase は未完了として扱う。

## 目的と承認済みの判断

通常ファイルの 64 KiB 要求を各層で保持し、新規作成・追記・小書込み・反復読込みまで改善する。固定サイズの小さな cache と毎回の物理連続確保から、所有者・永続化・回収の責任が明確な I/O/VM 設計へ段階的に移行する。

2026-09-07、ユーザーは [buffer-plan-codex-2.md](../buffer-plan-codex-2.md) の方針を承認し、次の追加方針を指定した。

- amd64 の 1 GiB 制限は過去の実機起動問題を調べるための暫定制限であり、解除してよい。
- firmware が報告する利用可能 RAM を実際に map・管理・利用する。loader 側の報告制限も調査・解消する。
- 独立 WS と Phase に分け、細部の設計と実装計画を作成する。

計画作成後、ユーザーは WS025 完了を goal とした Queue 作成と自走を承認した。q087 は finished として保存し、q088 から順次実行する。以前の報告書の予測値を実測結果へ書き換えない。

## 設計資料

- [メモリマップ・boot handoff・allocator/DMA の詳細設計](memory-design.md)
- [I/O/cache の共通契約・実装境界](io-design.md)
- [実装済みの観測 API と baseline 契約](observability.md)
- [受け入れ項目と実行行列](acceptance.md)
- [再利用 fixture と証拠の保存規則](tests/README.md)
- [承認済みの buffer 改善方針](../buffer-plan-codex-2.md)、[レビュー原文](../buffer-plan-claude-2.md)

## 完了像と範囲

1. amd64 UEFI/BIOS の維持対象 loader は、64-bit の型付き memory range を kernel に渡す。kernel は 1 GiB/4 GiB で RAM を切り捨てず、holes・予約領域を除いて使う。
2. kernel image、RAM direct map、MMIO、将来の vmap を別の仮想領域として扱う。高位 RAM の allocation、user mapping、SMP、DMA、USB-root が通る。
3. 64 KiB pool、cache/UFS run、USB reservation を揃える。新規 allocation と FAT/UFS metadata の重複更新も減らす。
4. file page cache の保持寿命とメモリ予算を確立し、read/write/mmap が同じ内容の所有者を共有する。
5. dirty/error 世代と下位 drain を用意し、既存 data の write-back、次に統合 UFS の ordered metadata/journal を有効化する。
6. async submission、先読み、exec snapshot 共有を個別に受け入れる。vmap/SG までを後続基盤として実装する。
7. 物理媒体の世代、DMA 隔離、fsync の永続化を維持し、通常・準正常・障害・低メモリの証拠を残す。

NVMe 多重発行、user page 直接 I/O、UAS は条件付き拡張とする。必要性を示す測定・実機がなければ planned のまま残し、主要到達点の完了を妨げない。BOT command 並列化、実媒体交換後の旧 mount 継続、NUMA 最適配置、memory hotplug、LA57 移行は本 WS の到達条件に含めない。

## 他 WS との責任分担

| WS | 関係 |
| --- | --- |
| WS018 | q086/q087 の完了証拠を引き継ぐ。粒度・cache/write-back の追加作業は WS025 が担当する。無関係の namespace/tmpfs 等の残件を引き取ったとは扱わない。 |
| WS003 / WS023 | 高位 ACPI・SMP・HAL 整形の既存証拠を維持する。1 GiB 解除と allocator の機能変更は新しい WS025 の結果として記録する。 |
| WS004 | DMA/USB/NVMe の機能境界を共同利用する。既存 WLAN/HID/ownership 回帰を実行するが、未選択の機器 bring-up を再開しない。 |
| WS024 | public UFS、64-bit format、driver 統合、mkfs/image 移行を所有する。WS025 は batching/dirty/永続化を担当し、p021 は統合先 UFS を消費する。 |
| WS011 / WS005 | net/networkd の通常操作を workload として使う。ネットワーク設定方針そのものは変更しない。 |

WS024 と循環依存を作らない。WS024 の driver/format 統合は write-through のまま完了可能であり、WS025 の write-back 完了を待たない。WS024 が先行した場合、p008/p010/p011 は統合先の同じ機能へ適用し、UFS1 専用の第二実装を増やさない。

## Phase registry

p001 は q088 で completed（[results](phase001-baseline-contracts/results.md)）、p002 は q089 で completed（[results](phase002-boot-memory-handoff/results.md)）、p003 は q090 で completed（[results](phase003-amd64-direct-map/results.md)）、p004 は q091 で completed、p005 は q092 で completed、p006 は q093 で completed、p007 は q094 で completed、p008 は q095 で completed、p009 は q096 で completed、p010 は q097 で completed、p012 は q098 で completed、p014 は q099 で completed、p011 は q103、p013 は q104、p015 は q105 で completed、残りは planned。条件付き Phase は追加の採用条件も記載した。依存は Phase 番号順と同義ではなく、下表と各 P-book を正とする。

| Phase | 内容 | 直接の前提 |
| --- | --- | --- |
| [p001](phase001-baseline-contracts/phase.md) | baseline・観測値・契約と既存 fixture の対応 | q086/q087 |
| [p002](phase002-boot-memory-handoff/phase.md) | BIOS E820、UEFI range/lifetime、ZBL6 v6 | p001 |
| [p003](phase003-amd64-direct-map/phase.md) | RAM 専用 direct map と bootstrap 切替え | p002 |
| [p004](phase004-physical-allocator-dma/phase.md) | range 別 allocator、制約付き DMA、幅・探索・統計 | p003 |
| [p005](phase005-high-memory-acceptance/phase.md) | 高位 RAM の通常利用を有効化・受け入れ | p004 |
| [p006](phase006-io-pool-exec/phase.md) | 大小 I/O pool と exec chunk | p001 |
| [p007](phase007-buffer-runs/phase.md) | buffer cache の連続 run | p001 |
| [p008](phase008-ufs-data-runs/phase.md) | UFS data run | p007 |
| [p009](phase009-usb-reservations/phase.md) | core/HCD reservation、通常 64 KiB | p001。高位有効化との統合は p005 後 |
| [p010](phase010-ufs-metadata-adapter/phase.md) | UFS allocation adapter、CG/indirect cache | p008 |
| [p011](phase011-ufs-allocation-batching/phase.md) | UFS allocation batch と初期化・公開順序 | p010、p014 |
| [p012](phase012-fat-cache-cursor/phase.md) | FAT clean sector cache と chain generation | p001 |
| [p013](phase013-fat-metadata-batching/phase.md) | FAT 操作内の同期 metadata batch | p012、p014 |
| [p014](phase014-flush-generations/phase.md) | accepted/completed/stable 世代と flush 合流 | p001 |
| [p015](phase015-file-page-cache/phase.md) | file object の cache reference と read cache | p006、p008 |
| [p016](phase016-memory-budget-reclaim/phase.md) | 全体予算、clean reclaim、低位 RAM reserve | p005、p015 |
| [p017](phase017-dirty-error-drain/phase.md) | dirty/error 世代、観測 cursor、内部 drain | p011、p013、p014、p016 |
| [p018](phase018-data-writeback/phase.md) | opt-in data write-back、syncer、credit/throttle | p017 |
| [p019](phase019-async-bio/phase.md) | bounded async queue と完了・取消しの所有権 | p009、p014、p015 |
| [p020](phase020-readahead/phase.md) | 需要優先の bounded 先読み | p016、p019 |
| [p021](phase021-ufs-metadata-writeback/phase.md) | 統合 UFS の ordered metadata/journal/replay | p018、WS024 p001–p004 |
| [p022](phase022-exec-cache-snapshot/phase.md) | page cache による exec snapshot 共有 | p015、p016 |
| [p023](phase023-kernel-vmap/phase.md) | 非連続 page の kernel vmap | p005、p016 |
| [p024](phase024-sg-dma/phase.md) | SG BIO/DMA と xHCI の対応 | p009、p019、p023 |
| [p025](phase025-storage-media-recovery/phase.md) | sense/媒体世代/control recovery | p001、p014 |
| [p026](phase026-integration-defaults/phase.md) | 統合受け入れ、段階別既定化、旧経路整理 | p005–p025 の必須成果 |
| [p027](phase027-nvme-queue-depth/phase.md) | 条件付き: NVMe 多重発行 | p019、NVMe の測定結果 |
| [p028](phase028-direct-user-io/phase.md) | 条件付き: user page 直接 I/O | p022–p024、コピー律速の証拠 |
| [p029](phase029-uas/phase.md) | 条件付き: UAS driver | p019/p024、対応実機と descriptor |
| [p030](phase030-imod-measurement/phase.md) | 条件付き: IMOD 実機比較・設定判断 | p009/p025、対応実機 |
| [p031](phase031-driver-layout-style/phase.md) | ドライバ配置・統合・drv_命名・coding-style適用、mkfs独立化とコマンド整理 | p026。p027–p030 の実装に先行 |

## 実装 wave と中間完了点

| Wave | Phase | その時点で使える改善 |
| --- | --- | --- |
| 0 | p001 | source-linked baseline と後段を測る counter |
| 1a | p002 → p003 → p004 → p005 | BIOS/UEFI で 1 GiB 超・4 GiB 超の RAM を実際に利用 |
| 1b | p006、p007 → p008、p009 | write-through のまま上位から USB まで大きい要求を保持 |
| 2 | p014、p010 → p011、p012 → p013 | 新規作成・追記、metadata の重複削減 |
| 3 | p015 → p016 → p017 → p018 | cache 保持・回収、エラー通知、data write-back |
| 4 | p019 → p020、p022、p023 → p024、p025 | 先読み・exec 共有・非連続ページ・復旧 |
| 5 | WS024 → p021、p026 | 統合 UFS の metadata 永続化と既定化 |
| 整理 | p031 | 合意したドライバ構成・命名・規約へ統一 |
| 拡張 | p027–p030 | 計測した機器・用途に限定した改善 |

1a と 1b は開発上独立した部分を持つ。早い I/O 改善を全 RAM 対応まで待たせる必要はない。ただし高位 RAM の通常公開は DMA/VM まで準備した p005 で行う。共有 checkout の編集競合と build/runtime の並列実行は避ける。

最初の Queue は p001 から選ぶ。p001 の結果を受けて p002–p005 または p006–p009 の有限の範囲を選択する。全 30 Phase を一つの Queue に入れない。実行時間枠は次の Queue 選択時に具体化し、P-book の途中状態・診断・再開条件を残す。

## 完了判定

主要部分の完了は p001–p026 の必須条件の証拠で判定する。対象 FS/device ごとの実効 policy と未対応項目を明記し、failed writeback/DMA を残したまま成功扱いしない。条件付き Phase は測定判断を別に記録し、未実施を実装済みと表現しない。

build は supported x86 設定を `make -j16` で直列実行する。aggregate `make check` と commit は行わない。amd64 は `qemu-system-x86_64`、破壊可能な storage 試験は disposable image を使う。今回の計画作成に伴うソース変更・実機操作はない。

実機判定 (2026-09-07): ユーザーが手作業の実機動作をクリア扱いとして継続を指示。
p005/p026 の本件 physical gate は user-accepted。agent による実機 runtime 実施とは区別する。

q102 completed WS024 p001–p004; q103/p011 and q104/p013 completed allocation batching, and q105/p015 completed file-cache lifetime. Physical acceptance remains user-accepted, agent runtime not executed.

q105 completed p015 with bounded ordinary file-cache ownership, coherent 64 KiB fills, final-close retention, lifecycle/claim drain, concurrent failure tests and native warm-read proof. Next p016 selects managed-memory budgets and clean reclaim.

q106 completed p016: shared RAM accounting, page-backed AVL file cache, clean reclaim, mandatory pool/DMA and worker reserve; host, 3 x86 builds, FS50/Wi-Fi30/native and the 12-cell RAM matrix pass. Physical gate remains user-accepted.

q109 completed p018: opt-in allocated-data writeback, per-device workers/credits,
checked synchronous completion and failure-preserving lifecycle boundaries.
Focused host/sanitizer, native USB/NVMe, supported builds and FS50/Wi-Fi30 pass;
see p018 results for coverage and the retained USB shutdown lifecycle finding.


q110 completed p020 and the resolved-open synchronous flag regression in p018.
Bounded optional prefetch now shares coherent cache/lifecycle contracts; pinned
cached demand bypasses speculative backend serialization. Focused gates, measured
cold sequential/random workloads, supported builds and FS50/Wi-Fi30/native pass.
Continue with dependency-ready p021 metadata journal planning/implementation;
p025–p026 and explicit conditional adoption decisions remain.

## WS025 completion (q122)

必須 p001–p026 完了。最終結果は [p026 results](phase026-integration-defaults/results.md)、
[97項目の受け入れ対応](phase026-integration-defaults/acceptance-results.md)、
[実効policyと条件付きPhaseの採否](phase026-integration-defaults/effective-policy.md)、
[性能観測](phase026-integration-defaults/performance.md) に記録した。
ホスト／sanitizer、14 RAM構成、USB/NVMe、媒体交換、終了、FS50/Wi-Fi30が合格。
通常artifactへ復帰済み。旧journal v1互換は維持せず、ZUJ2 v2に統一。
前掲のq105等の「次」「残り」は履歴であり、この節と先頭Statusを最終状態とする。
