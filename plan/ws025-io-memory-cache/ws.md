# WS025: I/O・キャッシュ・物理メモリ管理の段階的再設計

q237: experimental scalar write/pwrite view connection and copy/view counters
implemented; host syscall and build gates pass. Native acceptance and output
publication remain; default is still off.

q236: p028 readonly uaccess view connects pin/lease/borrowed mapping owners;
focused ordinary/sanitized/absent-HAL tests and three builds pass. Syscall
integration, output publication and native measurement remain.

q235: p028 actual VM protect/unmap/fault/fork lease waits, COW/refault and
last-space release pass ordinary/sanitized host tests. Integration and native
measurements remain.

q234: p028 bounded alias lease implemented, controlled rollback/ownership
fixture and three builds pass. Actual VM mutator races, syscall integration
and native measurements remain.

q233: p028 private pin-to-BUSY upgrade and concurrent fixture pass, with all
three builds. Full alias reservation/content lease remains next.

q232: p028 borrowed RAM vmap implemented and focused host/sanitizer plus
amd64/PCAT/PC98 builds pass. Content lease, syscall integration and measurements
remain; p028 is uncleared. See [results](phase028-direct-user-io/results.md).

q143 update: p030 private 0/160/4000 builds, actual MMIO interval readback and
USB/HID campaigns pass; write/fsync and load/latency measurement remain.
[Results](phase030-imod-measurement/results.md). q144 captured p029 QEMU
descriptors at both speeds; q145 implements and verifies the parser; q191 adds
the high-speed command IU/state engine; q192 adds reserved synchronous endpoint
transport and ownership tests. q193 binds the high-speed disk class and passes
QEMU raw write/sync/readback; q194 passes idle replug/readback and four-CPU halt.
q195 implements task abort and sticky write uncertainty with host tests; q196
proves native read timeout/task abort/new read. Media/live generation, streams,
uncertain-write injection and filesystem acceptance remain. q197 adds gated URB
stream identity; q198 implements xHCI primary ring/configuration ownership and
passes default-ring regression. q199 enables capable xHCI streams and passes
native SuperSpeed I/O, idle replug and halt. q202 passes fixed-medium SS reset,
reprobe and new read after timeout. q203 passes UFS/file fsync/remount at both
speeds; removable-media generation and in-flight lifetime acceptance remain. See [UAS results](phase029-uas/results.md).

q213 adds removable-medium readiness/retirement/republication ownership and checked
worker join. Targeted host checks, SS removable I/O/removal and three builds pass;
q214 proves in-place eject/change-medium at both speeds with protocol evidence.
q215 proves initially empty LUN boot/insertion at both speeds.
q216 implements partition rediscovery and passes HS/SS replacement MBR child
readback. q217 implements and proves HS/SS removable timeout/retirement/reset/read
recovery. q218 evidence audit reproduces BUG-021: old UFS mount cannot be retired after
medium loss. q219 saves explicit teardown design and fixes the BOT administrative
open omission. q220 implements reversible revoked-media writeback pause; cache,
filesystem and namespace commit remain. q221 adds revoked-buffer preflight to
avoid partial dirty discard on busy refusal. q222 adds VM ownership preflight;
q223 adds VM discard commit and dirty-credit retirement with focused checks/builds.
q224 adds UFS no-I/O revoked preparation, including snapshot/journal-reader refusal.
q225 adds retained-file external-owner refusal and deduplicated VM path counts.
q226 adds inode ownership preflight with VM counts outside the inode-cache lock.
q227 adds no-I/O UFS commit and local dirty-inode disposal.
q228 integrates public umount -f and reserved-mount VM reclaim exclusion.
SuperSpeed clean-medium exchange and cwd refusal/retry pass. q229 fixes pending
USB wait scheduling and passes High-Speed twice (BUG-022 observed path fixed).
q230 passes dirty-retained HS/SS acceptance and completes p029.
See phase029-uas/lost-media-teardown.md.

Last updated: 2026-09-10

WSID: `ws025`

Status: mandatory p001–p026 completed; p027 completed (q139); p029 completed/cleared (q230); p028/p030 uncleared; p031 completed/cleared q188; p032 reopened for current physical PC98 failure; p033–p035 completed. Direct-user I/O, IMOD measurement and physical PC98 recovery keep WS025 unfinished.

Parent: [master plan](../master.md)

追加計画: [p031 ドライバ配置・命名・規約統一](phase031-driver-layout-style/phase.md) はq188でユーザー受け入れによりcleared。q123のuncleared履歴を維持し、後続Phaseの先行条件を解除する。

最新ユーザー指示 (2026-09-09): p032は現行バイナリの実機不動作により再開。
q124のPC98 QEMU overlay/native・永続化・停止・loader 16/16の合格は維持するが、
実機成功とは扱わない。p029はQEMU UASの動作受け入れでcleared可能とする。
q124のp033〜p035の無線受け入れ完了は維持する。

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
| [p029](phase029-uas/phase.md) | UAS driver。q230でHS/SS QEMU受け入れ完了 | p019/p024、QEMU UASと取得descriptor |
| [p030](phase030-imod-measurement/phase.md) | 条件付き: IMOD 実機比較・設定判断 | p009/p025、対応実機 |
| [p031](phase031-driver-layout-style/phase.md) | ドライバ配置・統合・drv_命名・coding-style適用、mkfs独立化とコマンド整理 | p026。p027–p030 の実装に先行 |
| [p032](phase032-pc98-boot-regression/phase.md) | 再開：現行PC98バイナリの実機起動回復 | QEMU合格は保持。現行成果物と実機停止位置を照合しR10を確認 |

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

## 2026-09-09 回帰修正 queue

q123 は p031 uncleared で終了。q124 で p032 を完了し、[p033 AX211 自動接続・DHCP](phase033-ax211-wifi-dhcp-regression/phase.md) を第二項目として実行中。p031 の残件判定と p027–p030 より回帰修正を優先する。

2026-09-09 追加: q124 に [p034](phase034-ax211-operation-recovery/phase.md) を追加（pending、最新依頼で実行承認済み）。AX211 の直接 up/down/connect 反復後の無限エラーを再現・修正する。p033 実機試験後に実行。

q124 進捗: p033 DHCP 復旧中に AX211 の反復 rollback エラーを実機再現したため、p034 を in-progress として原因修正を先行する。p033 は受け入れ未完了のまま保持する。

追加依頼: q124 に [p035](phase035-multi-radio-selection/phase.md) を追加（pending、実行承認済み）。AX211＋RTL8822BU の同時自動接続と遅延を確認・修正する。p034 反復試験後に開始。

q124 最新: p032–p034 completed、p035 in-progress。2 seed のAX211実機反復とDHCP/取消し復旧の全gate完了。追加の複数無線 baseline を実行する。

q124 最終: p032–p035 completed / finished。[p035結果](phase035-multi-radio-selection/results.md)：二台の両操作順序・反復・取消し復旧、AX211単独反復、36 storiesと3 architecture build完了。OFFER/bound観測約40秒→約6.5秒（同一enable→鍵登録系列）。firmware assert自体は残存事項として分離した。

## Priority continuation clarification (2026-09-09)

The user explicitly permits implementing p027–p030 without physical hardware.
q125 uncleared evidence is historical; physical absence is no longer a reason
to stop implementation. Separate QEMU functional evidence from unmeasured
physical performance. The user owns no UAS hardware. Local QEMU exposes
usb-uas; determine whether it can validate the transport. Move UAS to Future
List only if QEMU validation proves unavailable. EHCI/UHCI analysis is deferred
after ready implementation work but remains inside the current goal.

## q139: p027 complete

NVMe now admits 64 KiB BIOs and pipelines up to four existing command slots per
BIO. Depth 1/2/4/8 host fault tests and QEMU native/restart/counter gates, three
platform builds and NVMe UFS writeback regression passed. See
[p027 results](phase027-nvme-queue-depth/results.md). p028-p030 remain open; p031 was subsequently cleared in q188. Physical throughput is not claimed.

### q242: p028 strict file output boundary

VM prefix contract is connected to READ/PREAD transactions with pinned cache and
content lease. Actual transaction short/error/position/completion checks and three
builds pass. Writable uaccess/read syscalls and native output acceptance remain;
p028 stays uncleared. See [results](phase028-direct-user-io/results.md).

### q243: p028 writable user view

Output uaccess view now acquires the full alias lease and writable borrowed VA,
marks captured owners dirty and shares input retirement. Focused orchestration
checks and three builds pass. Syscall connection and native output remain;
see [results](phase028-direct-user-io/results.md). p028 stays uncleared.

### q244: user HAL boundary review

Output syscall/native flag-on functionality passes; no baseline timing comparison.
User review identified hal_vmap_* duplication of kernel mapping responsibility.
[Findings and proposed correction](phase028-direct-user-io/hal-vmap-review.md)
prioritize shared HAL_SPACE_SYS page operations and common VM lifetime ownership.
p028 remains uncleared; flags default off. No automatic API replacement yet.

### q245: approved HAL API names

[p036](phase036-hal-space-api/phase.md) completed: 18 names, all architecture
sources/common consumers and active tests migrated; focused checks and three
builds pass. Further HAL restructuring awaits review of
[the overall proposal](hal-interface-proposal.md), with pmem request removal and
SYS/common-VM integration still unimplemented. p028/p030/p032 remain open.

### q246/q247: IRQ instrumentation and approved HAL migration

p030 IRQ counter/collector implementation and amd64 build are ready; native rate
collection waits until the HAL migration completes. The user approved the full
HAL proposal. [p037](phase037-hal-interface-consolidation/phase.md) removes physical
allocation request structures in q247, with focused checks and three builds passing.
SYS/query/common VM migration remains; p037 and p028 remain uncleared.

### q248: shared system page operations

p037 now includes optional-PA query, kernel allocation range and amd64 SYS page
operations under shared table ownership. Focused checks and three builds pass.
Common VM lifetime migration, old API removal and native acceptance still remain;
see [results](phase037-hal-interface-consolidation/results.md).

### q249: HAL vmap retired

Common VM now owns kernel VA/frame/lifetime policy; scratch/DMA/uaccess migrated.
hal_vmap_* and hal_kernel_page_lookup removed. Focused tests, three builds and
native output cell pass; owned-frame/DMA/input native acceptance remains. Output
is slower in the preliminary comparison; default-off preserved, range coalescing
will use existing HAL API. See [p037 results](phase037-hal-interface-consolidation/results.md).

### q250: native common-owner acceptance

p037 owned-frame/scratch/SMP probes and root login pass on 8 GiB / 4 CPU QEMU.
Ordinary build restored without wrappers; native DMA/input acceptance remains.
See [p037 results](phase037-hal-interface-consolidation/results.md).

### q251: input view migrated, performance unresolved

Current off/on native input cells pass with identical guest. Direct-input CPU
5.98 s versus copy 2.89 s; default off maintained. Ordinary image restored.
p037 retains native DMA acceptance; p028 retains optimization/acceptance.
See [results](phase037-hal-interface-consolidation/results.md).

### q252: map physical runs

Common VM now groups contiguous physical runs through existing HAL map; contiguous
64 KiB needs one call. Focused rollback/ownership tests and three builds pass.
Native performance remeasurement and DMA remain in p037/p028.

### q253: coalesced input measured

Native input pair passes: copy CPU 2.87 s, view 4.68 s (q251 view 5.98 s).
Grouping helps but view still costs more; default off. Native DMA remains in p037.
Ordinary build restored; [results](phase037-hal-interface-consolidation/results.md).

### q254: native fragmented DMA passes within 32-bit bus mask

Four 16-segment DMA vectors and USB writeback/fsync/remount verification pass.
PCAT PCI root fixes 32-bit mask, so high-DMA gate remains unverified despite AC64
on QEMU xHCI. Ordinary build restored. [Evidence/resume](phase037-hal-interface-consolidation/results.md).

### q255: native IRQ counter evidence

p030 current IMOD 4000 USB/HID campaign passes with register readback and
64 confirmed write/fsync/readback samples. 1181 callbacks over 16.5 seconds;
0/160 current comparisons and remaining topology/recovery coverage still open.
[Results](phase030-imod-measurement/results.md).

### q256: three current IMOD intervals compared

p030 0/160 campaigns pass; source/workload identical to q255 4000. All rates
approximately 71.5 callbacks/s in paced QEMU; no default change. Remaining
USB2/WLAN/recovery coverage recorded in [results](phase030-imod-measurement/results.md).

### q257: USB2 default interval

p030 USB2-only xHCI option and speed oracle added to tests. Actual high-speed
root plus IMOD 4000, USB/HID/64 storage samples pass; 71.35 callbacks/s.
Other USB2 intervals and WLAN/recovery remain. [Results](phase030-imod-measurement/results.md).

### q258: USB2 interval matrix complete

p030 USB2 0/160 campaigns pass; current USB2 and USB3-root comparisons complete
for all three settings. Default 4000 maintained; WLAN/recovery/physical gates
remain. [Results](phase030-imod-measurement/results.md).

### q259: media recovery and WLAN readiness

p030 default media replacement checks pass (REC03/05/06 subsets). WLAN host
10.0.10.25 currently returns No route to host; remote tests resume after access
returns. Remaining reset/pending-I/O coverage retained. [Results](phase030-imod-measurement/results.md).

### q260: current BOT bounded recovery

Obsolete test runner repaired. Selected storage fixture passes ordinary/sanitizers
with actual bounded self-reset/repeated-UA and worker recovery. Native IRQ/recovery
correlation remains distinct. [Results](phase030-imod-measurement/results.md).

### HAL syscall/fault discussion

ユーザー提示の固定syscall / user fault / sys faultの3入口と、vectorの
診断用途への限定案を[議論資料](hal-trap-interface-discussion.md)へ整理した。
新規宣言は未確定で実装未着手。space/pmemの既承認と区別する。

### q261: USB core late completion

Selected actual USB reservation fixture passes ordinary/sanitizers; failed cancel
retains staging until late completion. This is a controlled-HCD lifetime test,
not full native recovery. [Results](phase030-imod-measurement/results.md).

### q262: current HCD reservation/SG

Selected consolidated xHCI fixture passes ordinary/sanitizers for ownership, SG
encoding and generation limits. Controlled DMA scope; native high-DMA remains
separate. Core/BOT/HCD limited regression refresh is complete; avoid unchanged
repeats. [Results](phase030-imod-measurement/results.md).

### q263: high-DMA experiment needs diagnosis

Test-only 64-bit HCD owner observes two high fragmented vectors, then fails
segment-count assertion before full acceptance. Production default restored.
Diagnose constrained allocation returns/fallback before modifying behavior;
[p037 results](phase037-hal-interface-consolidation/results.md).

### q264: p037 completed

High-DMA test PFN refusal diagnosed and fixture corrected without production
changes. Four >4 GiB fragmented DMA vectors and native data/fsync/remount pass.
Ordinary build restored; approved HAL consolidation p037 completed. Production
PCI mask remains conservative; p028 performance and trap discussion remain.
[Completion audit](phase037-hal-interface-consolidation/results.md).

### q265: stage timer prerequisite unavailable

p028 input path already avoids staging on successful view. Link-only stage
profiling cannot use unavailable HAL TSC counter in current QEMU. No stage
timing acquired; ordinary build restored. Next verify timer configuration or
collect counts. [Results](phase028-direct-user-io/results.md).

### q266: input mapping fragmentation observed

Native input oracle passes with count-only instrumentation. 2048 views invoke
SYS map 18432 times (9 per 64 KiB average), user protection and SYS unmap once
per view. Ordinary restored. p028 next audits publication/synchronization costs;
no new HAL interface approved. [Results](phase028-direct-user-io/results.md).

### q267: retained input permission audit

Actual lease failure/retry fixture passes ordinary and ASan/UBSan. Retained
INPUT_PROTECTED is a recovery hint, not a current readonly certificate; failure
and mprotect paths preclude flag-only skip optimization. Production unchanged.
[p028 evidence and next step](phase028-direct-user-io/results.md).

### q268: redundant amd64 protection synchronization

Identical PTE permission-only calls now avoid rewriting and shootdown under
the existing serializer. Actual changes and flag observation retain sync.
Focused actual HAL ordinary/sanitizer tests and three image builds PASS.
Native functional/CPU comparison remains required; p028 uncleared/default off.
[p028 results](phase028-direct-user-io/results.md).

### q269: native input functionality passes; CPU condition still unmet

Same guest off/on runs PASS. 128 MiB CPU off 2.78 s, on 5.20 s; defaults stay
off. Ordinary restored. Next investigate absent-PTE publication synchronization,
not flag-only protection skips. [Evidence](phase028-direct-user-io/results.md).

### q270: fresh data mapping synchronization removed

Intel-specified absent-PTE publication avoids success shootdown; executable and
retirement paths retain it. HAL host/sanitizer, amd64 builds, native input PASS.
Off CPU 2.82 s, on 3.21 s for 128 MiB; p028 adoption still unmet/default off.
Ordinary restored. [Results](phase028-direct-user-io/results.md).

### q271: SMP mapping acceptance after synchronization changes

Native 8 GiB/4 CPU high fragmented maps, explicit same-VA/different-PA reuse,
AP observations, table rollback, free-byte balance and root login PASS.
Ordinary restored/no wrappers. p028 performance still open; output-direction
comparison next. [Evidence](phase028-direct-user-io/results.md).

### q272: output functional acceptance, performance unmet

Current-HAL read/pread/EOF/COW native oracle PASS off/on. CPU 0.85 s versus
2.01 s for 128 MiB. Ordinary restored; defaults remain off. Next evaluate
output alias unmap batching. [Results](phase028-direct-user-io/results.md).

### q273: output alias range retirement

Adjacent output aliases now share a HAL unmap call, with per-run mapped-state
updates and unchanged lifetime reservations. Actual VM batch/refusal tests and
three builds PASS; native output comparison next, defaults remain off.
[Evidence](phase028-direct-user-io/results.md).

### q274: native output batching acceptance

Output data/EOF/COW off/on PASS. CPU 0.85 s versus 1.56 s for 128 MiB;
adoption still unmet/default off. Ordinary restored. Further ownership-cost
measurement required before another optimization.
[Evidence](phase028-direct-user-io/results.md).

### q275: IMOD recovery evidence boundaries

WLAN SSH still No route to host. Reconciled accepted p025/p029 recovery with
p030 experiment-specific residuals; next test-only integration is explicit
image/MMIO readback for native media recovery at nondefault intervals.
[Ledger](phase030-imod-measurement/remaining-evidence.md).

### q276: media recovery runner verifies IMOD

Explicit source/root image and expected MMIO interval integrated. Default4000
media recovery passes; wrong expected interval rejected with guest cleanup.
No production changes. Next 0/160 native media comparisons.
[p030 results](phase030-imod-measurement/results.md).

### q277: IMOD media comparison complete

0/160 native register/media tests PASS alongside q276 default4000, identical
workload root. Ordinary restored; remaining ledger updated, physical/WLAN
still uncleared. [Results](phase030-imod-measurement/results.md).

### q278: staging/resume cost audit

Output scratch is nonblocking preallocated reserve borrowing, not repeated
allocation; deferring it would change tested transaction ordering. No production
change. Next output-specific map/pin attribution targets remapping after alias
retirement, preserving writer lifetime. [Results](phase028-direct-user-io/results.md).

### q279: output user remapping observed

Native output oracle PASS with count-only probe. 2048 leases: user map32754
(one page each), user/SYS unmap2048 each, SYS map16384. Ordinary restored.
Evaluate post-writer alias restoration against COW/failure/lifetime constraints
before implementation; p028 uncleared. [Evidence](phase028-direct-user-io/results.md).

### q280: output aliases restored after writer retirement

Best-effort restoration only for this lease's revoked aliases, with COW and
original holes preserved; refusal stays faultable. Actual VM ordinary/sanitizer
checks and three builds PASS. Native output comparison pending/default off.
[Results](phase028-direct-user-io/results.md).

### q281: native restored-output acceptance

read/pread/EOF/COW off/on PASS; CPU 0.83 s/1.23 s for 128 MiB. Restoration
functional check accepted, CPU adoption remains unmet/default off. Ordinary
restored. [Results](phase028-direct-user-io/results.md).

### q282: page-vector output design

Stable pinned kernel addresses can avoid output SYS remapping if file/VM copy
destination supports spans. Designed shared destination without per-page backend
calls or ownership relaxation. [Implementation steps](phase028-direct-user-io/page-vector-output-design.md).

### q283: VM destination spans implemented

Shared strict coherent read supports bounded scattered destinations, retaining
backend batching/prefix guarantees. Actual focused VM and destination tests,
sanitizers and three builds PASS. File/uaccess/syscall integration still pending.
[Results](phase028-direct-user-io/results.md).

### q284: file transaction vector destination

Strict destination shares existing file/VM transaction, guards and accounting.
Actual READ/PREAD prefix checks and64 KiB/16span/one-backend case PASS; sanitizer
and three builds PASS. uaccess/syscall wiring next; p028 uncleared.
[Results](phase028-direct-user-io/results.md).

### q285: uaccess span owner

Mapping-free exclusive output owner implemented and tested without map symbols;
ordinary/sanitizer and three builds PASS. Syscall switch must support512-byte
fallback slices, then remove old mapped output path. p028 remains uncleared.
[Results](phase028-direct-user-io/results.md).

### q286: scalar output switch

READ/PREAD destination wiring and old output-map removal complete. Focused
host/sanitizer gates and three image builds PASS; native comparison remains.
p028 remains uncleared, defaults off. [Results](phase028-direct-user-io/results.md).

### q287: native comparison deferred by boot failure

Current ordinary image reaches login but authentication then exec ENOSPC fails;
runner timeout and source hash evidence retained. Diagnose before output timing.
No enabled image built; p028 remains uncleared. [Results](phase028-direct-user-io/results.md).

### q288: ordinary native output PASS, intermittent boot failure open

Diagnostic and restored ordinary runs pass. Restored kernel hash equals failed
q287 kernel; no fix claimed. CPU0.84s/copy128MiB for ordinary path. Diagnostic
code removed. Enabled-output comparison remains executable; boot ENOSPC remains
unexplained and p028 uncleared. [Results](phase028-direct-user-io/results.md).

### q289: output spans native PASS

128MiB output copyout counter0; CPU0.87s vs q288 ordinary0.84s, same guest.
No demonstrated CPU benefit; defaults off, ordinary image restored. Next inspect
repeated destination scans/validation under VM lock. p028 uncleared and q287
intermittent boot failure open. [Results](phase028-direct-user-io/results.md).

### q290: sequential output cursor

Removed repeated destination scans from VM page-copy sites through a validated
owned descriptor cursor. Actual VM/file and cursor host/sanitizer gates plus
three builds PASS. Native measurement next; defaults off, p028 uncleared.
[Results](phase028-direct-user-io/results.md).

### q291: cursor native comparison

Both modes PASS; CPU0.86s ordinary/0.96s enabled, no demonstrated benefit.
Ordinary restored, defaults off, p028 uncleared. Measure remaining ownership
stages before selecting another change; q287 remains open.
[Results](phase028-direct-user-io/results.md).

### q292: profiling clock prerequisite checked

No KVM device; TCG explicitly rejects invariant TSC. Precise stage timing cannot
be claimed. Next refresh current operation counts with existing link-only output
probe, preserving HAL clock policy. No production change; p028 uncleared.
[Results](phase028-direct-user-io/results.md).

### q293: current output operation profile

Native PASS;2048 leases have0 SYS maps/unmaps,18434 user maps and2048 user unmaps.
Counts include concurrent external callers; no CPU attribution. Ordinary forced
relink/no wrappers verified. Next audit input source spans because input retains
temporary SYS mappings. p028 remains uncleared.
[Results](phase028-direct-user-io/results.md).

### q294: input span design

Audited delayed/immediate file writes and unordered VM commit.
[Input design](phase028-direct-user-io/page-vector-input-design.md) defines bounded
source gather, pre-lock contiguous fallback and shared transaction ordering.
Implementation next; no production changes, p028 uncleared.

### User steering: HAL ahead of I/O performance

I/O p028 is held for expert review; q294 input design is not executing.
[ws025-p038](phase038-hal-fixed-entry/phase.md) implements the requested fixed
syscall/user-fault/sys-fault entries; q295 starts syscall-only migration.

### q295: fixed syscall entry

All five HALs call kernel_syscall_handler; registration API/pointers removed.
amd64/PCAT/PC98 builds PASS. Fault integration and runtime gates remain in
[p038](phase038-hal-fixed-entry/phase.md); I/O performance stays on review hold.

### q296: fixed supervisor fault entry

Trap registration removed; all five HALs call fixed sys fault callback. Existing
unhandled diagnostic/stop policy retained. ARM64 handled-return loop fixed using
unchanged frame layout. Three image builds and ARM64 assembly PASS; user fault
normalization and runtime gates remain in [p038](phase038-hal-fixed-entry/phase.md).

### q297: normalized user fault policy

All HAL user calls use cause/access and diagnostic raw data; old user-int entry
removed in favor of fixed syscall observation. Actual generic-entry host/sanitizer
and three builds PASS; non-x86 C syntax PASS. Detailed decode/frame audit and
QEMU runtime remain in [p038](phase038-hal-fixed-entry/phase.md).

### q298: amd64 fixed-entry runtime PASS

Real exception testing found user INT3 gate incorrectly DPL0; fixed vector3 on
both x86 HALs. amd64 fault/signal/sigreturn/EINTR/restart/ENOSYS guest now PASS,
three builds PASS. Other-architecture runtime/detail audit remains in p038.

### q299: ARM64 exception slot identity

Actual slot passed after unchanged frame save; FIQ/SError no longer decoded as
synchronous ESR events. Actual C dispatch host/sanitizer and target assembly/
syntax PASS. ARM64 runtime and SPARC frame audit remain in p038.

### q300: SPARC startup decision pending

Initial priming invokes user fault without saved user frame; concrete
[review](phase038-hal-fixed-entry/sparc-startup-review.md) and question recorded.
SPARC dependent edits await answer; other HAL verification can continue.

### q301: m68k access mode

Non-memory sys faults corrected to NONE. Actual source host/sanitizer and target
syntax PASS; [p038 results](phase038-hal-fixed-entry/results.md). SPARC startup
decision and remaining architecture runtime gates still open.

### q302: PC/AT fixed-entry runtime PASS

32-bit BIOS QEMU passed real exception/signal return/EINTR/restart/ENOSYS guest
on disposable image; source unchanged. p038 retains SPARC startup decision and
non-x86 runtime limitations; see phase results.
