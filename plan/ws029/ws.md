<!-- awesome-plan project=zedbsd record=ws029 -->

# WS029: i915ネイティブGPU実装

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: q314 で p001〜p007 cleared。cold VFIO attach の間欠的な停止などを後続として登録済み
<!-- awesome-plan-current:end -->

## 単一目標

WS014で検証・整理したGPUフレームワークを使い、選定したIntel実機でzedBSDのi915 driverによる描画・表示を成立させる。ホストLinuxのi915を使うだけのQEMU/Venus実行とは別の成果。終了したWSを再利用せず、新しいWSとして持つ。

## 順序・依存

ユーザー指示の順序はGPUフレームワークのみ→QEMU＋Venusでのデバッグ/API改善→i915実装。前段は[WS014](https://github.com/awemorris/zedBSD/issues/15)が所有する。WS014最終contract・規約/検証結果を受け取り、i915で判明した不足も記録して整理する。

## 範囲・受け入れの具体化

struct drv_gpu_interfaceのstatic callback実装とPCI経由のGPU登録を用いる。対象GPUは従来のLatitude 5320を候補とし、PCI ID/世代、firmware、memory/submit/display/resetの要件、ユーザー空間driverとの分担、ライセンス境界を実装前に確定する。Linux i915コードの全面移植を今回決定したとは扱わない。

実機で合意した描画・表示テスト、console fallback、必要な同期/資源回収を確認することを完了方向とする。正確なAPI profileと受け入れは前段成果・実機情報で具体化する。PPCや他GPUを同居させない。

## Phase registry

| Combined ID | Phase | Status | Queue / 見積 |
| --- | --- | --- | --- |
| ws029-p001 | [対象確定・ライセンス境界・移植方針の固定](https://github.com/awemorris/zedBSD/issues/398) | cleared | q314-i01、120 分 |
| ws029-p002 | [driver骨格: PCI attach、MMIO/forcewake、GGTT、割込み](https://github.com/awemorris/zedBSD/issues/399) | cleared | q314-i02、240 分 |
| ws029-p003 | [メモリ: GEM object、48-bit PPGTT、CPU view](https://github.com/awemorris/zedBSD/issues/400) | cleared | q314-i03、180 分 |
| ws029-p004 | [実行: engine/LRC/execlists、request/seqno、engine reset](https://github.com/awemorris/zedBSD/issues/401) | cleared | q314-i04、300 分 |
| ws029-p005 | [drv_gpu統合、native stream、生UAPI試験クライアント](https://github.com/awemorris/zedBSD/issues/402) | cleared | q314-i05、240 分 |
| ws029-p006 | [VFIO passthroughテストループ（host手順・harness・初回起動）](https://github.com/awemorris/zedBSD/issues/403) | cleared | q314-i06、180 分 |
| ws029-p007 | [実機での描画確認、静的レビュー、規約全文確認](https://github.com/awemorris/zedBSD/issues/404) | cleared | q314-i07、180 分 |

対象実機はDell Latitude 5330（Alder Lake-P、8086:46a8）に確定し、Latitude 5320候補は置き換えた。設計の正本は[i915-design.md](i915-design.md)。表示/scanout、GuC、非LLC機、userland Vulkanのnative経路はp007の記録で後続Phaseとして列挙する。


## 後続（planning、q314 の p007 記録より）

| Combined ID | 内容 | Status |
| --- | --- | --- |
| ws029-f001 | cold VFIO attach の bring-up 間欠ハング安定化（forcewake/RC6 settle） | planning |
| ws029-f002 | hang 注入の実機 peer 継続（engine reset 後の queued request 再投入） | planning |
| ws029-f003 | display/scanout（IGD UPT では対象外、後続で mode set/DMC） | planning |
| ws029-f004 | GuC/HuC 不使用の再確認、非 LLC 機対応、userland Vulkan の native 経路 | planning |

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの全文を実装前に読む。HAL責務/hal.hの変更は別途適用承認が必要。既存PCI/VFS/VMの責務を確認し、大規模refactor前の配置を仮定しない。aggregate make checkは禁止。必要な対象buildはmake -j16と意味のある限定確認を用いる。無関係な変更を保護する。

ユーザーは計画・GitHub公開を指示した。まだ有限Queue、実行範囲と調査上限は選択していない。コード実装/build/QEMUは未実行。資料のgit add/commitはユーザーが行うためエージェントはadd/commit/pushしない。

## q310開始: GPUレビュー改善p007（2026-09-13）

ユーザーの新Phase作成・実行指示により、[WS014 p007](https://github.com/awemorris/zedBSD/issues/394)全体を単一項目q310-i01として実行する。直接VK_KHR_displayにもGPU copy/blit→共有linear画像→BLOB scanoutを使い、通常vkdemoのCPU readbackを除く。完了通知・command batch/reply mmap・controller排他の短縮・非同期present・同一open並行性・安全なtransport回復・buffer/optimal allocation共有を改善し、実QEMUで描画/寿命と転送数を検証する。

ユーザーはzwlの1パス1surface同期presentとlibwaylandの限定protocolをテストドライバとして承認した。一般Wayland環境、複数window合成・入力・既存Toolkit対応・常駐化は本Phaseへ入れない。reviewの推奨は仕様と照合し、送信受理、Venus decoder応答、VkFence完了、scanoutを区別する。以前のp006でGPU内表示を実証したのはWayland経路であり、直接表示にCPU経路が残った対応不足を訂正する。

p006/q309は実際の受入範囲のcleared/finishedと証拠を保持する。順序はp006 cleared → p007 in-progress → p004 planning/未queue → WS029 native i915。WS030 completed、p001の未決定と既存clearanceは維持。q310は720 active minutes見積/120分レビューの有限項目。HAL追加変更、VFIO/ホスト表示停止、git add/commit/pushは含めず、private image/source転送とGitHub計画同期の既存承認を使用する。開始時点では新実装・試験の成功を主張しない。

## q310最終引継ぎ

q310 finished、[WS014 p007](https://github.com/awemorris/zedBSD/issues/394) cleared。BLOB直接表示・標準OPAQUE memory/fence・同期/batch・topology/placementを受入済み。最終APIは170 commands / drv_gpu_ops v6。active Queueなし、p004とWS029は未queue。optimal共有には隔離したpaired renderer差分を使用。source/docのgit公開はユーザー担当。

## q314準備: WS029 i915ネイティブGPU driver（2026-09-14）

ユーザー指示により[WS029](https://github.com/awemorris/zedBSD/issues/386)へp001–p007を作成し、Queue q314を準備状態で作成した。実行は次の指示で開始し、開始時にp001の移植方針承認とp006のhost操作許可をjournalへ記録する。計画の正本は plan/ws029/i915-design.md（事実、決定、ファイル構成、関数一覧、初期化/実行/割込みの手順、native stream、試験設計、受入）。

決定: 対象はDell Latitude 5330のAlder Lake-P（8086:46a8）、execlists/ELSQ（GuC不使用）、表示は対象外（IGD UPTは画面出力なし）、LLC coherent前提、Linux i915のMIT定義/テーブルを出典付き`.inc`へ分離転記し論理はzedBSD規約で新規実装、HAL/UAPI不変、GEM backingは物理連続16 MiBまで。p001–p005はhost fixtureとamd64 buildで固定し、p006でVFIO passthroughのtest loop（GDM停止→i915 unbind→vfio-pci→QEMU→復旧）、p007で実機のcopy/fill/store/jobとhostのpmemsave照合、静的レビュー、規約全文確認。

Linux参照10ファイルのMIT表記を確認済み（p001で固定tagの全ファイルを機械監査）。host事実: IOMMU group 0単独、vfio-pci module、CONFIG_VFIO_PCI_IGD=y、QEMU 10.0.11、sudo -n可。見積1440 active minutes、120分ごとにレビュー、同条件retry 3回、VM 300秒/build・転送1200秒/host attach・restore各120秒。host reboot・package導入・cmdline変更は許可外。git add/commit/pushはユーザー担当。

## q314開始: WS029 i915ネイティブGPU driver（2026-09-14）

ユーザーの「では、実装してください。」により q314 を開始し、[WS029 p001](https://github.com/awemorris/zedBSD/issues/398) を q314-i01 として実行する。提示済みの計画（plan/ws029/i915-design.md、p001–p007）に対する開始指示を、p001 の移植方針（MIT 定義/テーブルの分離転記＋論理の新規実装）と p006 の host 操作（GDM 停止、i915 unbind、VFIO passthrough）の承認として journal に記録する。firmware が必要になった場合は userland/firmware/<機種>/ に置く（現計画では GuC/HuC 不使用で firmware なし）。順序は p001 → p002 → … → p007。見積 1440 active minutes、120 分ごとに点検、同条件 retry 3 回。HAL/UAPI 不変、host reboot・package 導入・cmdline 変更は許可外、git add/commit/push はユーザー担当。開始時点で新実装・試験の成功は主張しない。

## q314 p001 完了: 対象確定・ライセンス境界・移植方針の固定（2026-09-14）

[WS029 p001](https://github.com/awemorris/zedBSD/issues/398)（q314-i01）を cleared にし、[p002](https://github.com/awemorris/zedBSD/issues/399)（q314-i02、driver 骨格）を in-progress にする。source 変更なし、host 状態変更なし。

成果物（すべて local）:
- `plan/ws029/phase001/approval.json`: 開始指示「では、実装してください。」を、MIT 定義/テーブルの分離転記＋論理新規実装、GDM 停止・i915 unbind・VFIO passthrough の承認として記録（設計資料 SHA256 付き）。firmware は必要時に `userland/firmware/<機種>/`。
- `plan/ws029/i915-license-audit.md`（`plan/ws029/tests/fetch-linux-refs.sh v6.19`）: 参照 29 ファイル（`drivers/gpu/drm/i915/` 26、`include/drm/intel/pciids.h`、`include/drm/intel/i915_drm.h`、`include/uapi/drm/i915_drm.h`）を tag v6.19 で取得し SHA256 を記録。判定は全ファイル MIT（SPDX MIT または MIT/X11 permission notice）。GPL のファイルは参照していない。script は MIT 以外があれば非 0 で終了する。
- `plan/ws029/phase001/host-facts.json`（`plan/ws029/tests/host-i915-facts.sh`、読取専用）: Latitude 5330、kernel 6.19.13+deb13-amd64、`00:02.0` = `8086:46a8` rev 0c、i915 bound、iommu group 0 単独（全 17 group）、`CONFIG_VFIO_PCI_IGD=y`、vfio 系 module 解決可、`/dev/vfio` は `vfio` のみ、GDM active、QEMU 10.0.11 に `vfio-pci` あり、RMRR `0x6c000000–0x707fffff`。設計資料 §1 と差異なし。
- `plan/ws029/i915-symbols.md`（`plan/ws029/tests/gen-symbols.py v6.19` が生成）: p002–p005 が転記・参照する 257 symbol を 10 群（補助 macro、device ID/PCI、forcewake、reset、engine register、割込み、command、LRC、GTT、MOCS）に分け、出典 file:line と gen12/ADL-P での有効条件を列挙。値は書いていない。未検出 0。論理の参照元関数（ggtt/ppgtt/uncore/irq/engine/lrc/execlists/reset/emission/mocs）を「転記せず挙動を新規実装」として別表に列挙。
- `plan/ws029/i915-vfio-plan.md`: IGD UPT（画面出力なし）、iommu group 0 単独、RMRR relaxable、`vfio-pci` module、QEMU `-device vfio-pci,host=0000:00:02.0`、`sudo -n` 起動の理由（memlock）、`host-igd.sh attach/restore/status` の手順、許可済み操作（GDM 停止、i915 unbind、VFIO passthrough、restore）と許可外操作（reboot、package 導入、cmdline/modprobe.d/udev/limits 変更、BIOS、他プロセス kill）、既知 risk の扱いを記載。
- `plan/ws029/phase001/host-facts-after.json` と `host-state-diff.txt`: p001 の前後で driver/GDM/`/dev/vfio`/drm node/cmdline が同一であることを示す。

判明した事項:
- `SNB_GMCH_CTRL`/`BDW_GMCH_GGMS_*` は v6.19 では `include/drm/intel/i915_drm.h`（MIT）にあり、`intel_pci_config.h` にはない。`I915_MOCS_PTE` は `include/uapi/drm/i915_drm.h`（MIT）の enum。両ファイルを参照一覧と監査に追加した。
- gen12 の CSB 判定は `GEN12_CSB_SW_CTX_ID_MASK`/`GEN12_IDLE_CTX_ID`/`GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE` を使い、`GEN8_CTX_STATUS_*` は使わない。CSB entry が `-1` のままの場合は `GEN8_EXECLISTS_STATUS_BUF`/`GEN11_EXECLISTS_STATUS_BUF2` の mmio mirror から読む（tgl HSDES 22011327657 相当）。
- LRC image の per-context batch pointer は gen12 では offsets 表の index 0x12（`lrc_ring_wa_bb_per_ctx`）で、`CTX_BB_PER_CTX_PTR` という define は存在しない。p004 では 0 を書く。

検証: 監査 script exit 0、generator 未検出 0、host 前後 diff すべて same。次: p002（`src/drivers/gpu/i915/` 骨格、PCI attach、MMIO/forcewake、GGTT、割込み）。

## q314 p002 完了: driver 骨格（PCI attach、MMIO/forcewake、GGTT、割込み）（2026-09-14）

[WS029 p002](https://github.com/awemorris/zedBSD/issues/399)（q314-i02）を cleared にし、[p003](https://github.com/awemorris/zedBSD/issues/400)（q314-i03、メモリ）を in-progress にする。実機は未使用。HAL/UAPI 変更なし。git add/commit/push はユーザー担当。

新規 source（すべて local、Zlib）:
- `include/drivers/i915.h`: `drv_i915_pci_driver_register()` のみ公開。
- `src/drivers/gpu/i915/internal.h`: `struct i915_device`/`i915_ggtt`/`i915_session`、定数（engine slot、forcewake domain、timeout、予約 GGTT page、IRQ bank）、cross-file prototype。
- `src/drivers/gpu/i915/i915.c`: ID 表（ADL-P/ADL-N/RPL-U/RPL-P、`8086:46a8` を含む 54 ID）、attach → start（stage: dma-provider、save-pci-state、enable-pci-memory、bar0、map-registers、uncore、gt-reset、ggtt-probe/scratch/bitmap/fill、enable-bus-master、irq、gpu-publication）→ publish（`drv_gpu_ops` v9、capabilities=0、open/close/get_info のみ）、stop/detach の逆順解放。失敗時 `i915: attach stopped at <stage>: <errno>`。
- `src/drivers/gpu/i915/uncore.c`: `drv_i915_read32/write32`（BAR0 窓の範囲検査）、`drv_i915_wait32`（`sched_ticks` 上限）、`drv_i915_forcewake_get/put`（GT/RENDER の 2 domain、masked write、ACK poll 50 ms、参照計数）、`drv_i915_uncore_init`（全 domain 解放、GDRST 進行中なら待つ）、`drv_i915_gt_reset`（GRDOM_FULL、1 s）。
- `src/drivers/gpu/i915/ggtt.c`: `drv_i915_ggtt_start`（GMCH 0x50 の GGMS から entry 数、BAR0 上半分を map、scratch page、bitmap、全 PTE を scratch で埋めて flush）、`alloc/free`（first-fit、先頭 1 MiB 予約）、`insert/clear`（PTE=phys|PRESENT、`GFX_FLSH_CNTL`）、`stop`。
- `src/drivers/gpu/i915/irq.c`: `drv_i915_irq_start/stop/reset`（MSI 1 本、RENDER_COPY enable に user/CS error/context switch/semaphore、RCS0/BCS0 mask、他 class は disable/mask、master enable）、`drv_i915_irq_handler`（master disable→bank→selector→identity valid 待ち→class/instance/intr→counter→ack→master enable）。今は counter（user/context switch/error/unknown/identity timeout）だけを更新し、p004 で request 処理へ接続する。
- `src/drivers/gpu/i915/linux/i915-regs.inc`（163 定義）、`linux/i915-ids.inc`（4 表）: `plan/ws029/tests/gen-inc.py v6.19` が Linux v6.19 の MIT ファイルから `#define` だけを機械転記（`_MMIO` 除去、`REG_BIT`→`I915_INC_BIT` 等、U suffix）。header に各出典の copyright 行、MIT permission notice、出典 path と SHA-256、変換規則を記載。generator は出典 SHA-256 が `i915-license-audit.md` の値と一致し判定が MIT であることを検査し、転記本文が未転記 symbol を参照していれば失敗する。
- build: `Makefile`（`CONFIG_DRIVER_PCI_I915`/`_SELFTEST` 既定 n、-D、`KERN_GPU_BACKENDS` に追加）、`platform/amd64/vmunix.mk`（`AMD64_I915_SOURCES`）、`src/kern/platform/pcat.c`（登録）、`config/drivers/pci.drivers`、`config/kernel-options.list`（selftest bool）、`plan/ws029/tests/config-i915-amd64.mk`。

検証（agent-1）:
- `make BUILD=build/i915-amd64 ZEDBSD_CONFIG=plan/ws029/tests/config-i915-amd64.mk vmunix`: PASS（warning 0、`amd64 vmunix check: PASS`、`drv_i915_*` 14 symbol link）。
- 同 config で I915 := n: PASS、`drv_i915_`/`drv_gpu_register` symbol 0。
- `sh plan/ws029/tests/run-i915-host-tests.sh`（uncore/gtt/irq、通常＋ASan/UBSan）: PASS。forcewake 参照計数と timeout 時の巻き戻し、GDRST self-clear/stuck、範囲外 MMIO 拒否、GGTT 1M entry の scratch fill・first-fit・insert/clear/free・不正引数、IRQ enable/mask 値、bank/identity decode（RCS0/BCS0/未知 class）、CS error の EIR 記録、identity timeout。
- `python3 plan/ws029/tests/run-i915-build-selection-test.py`: 6 platform × Venus/i915 各 y/n で PASS（GPU core はどちらかの backend が y のときだけ、i915 object は amd64 かつ y のときだけ）。
- `git diff --check`: PASS。規約 checklist（forward declaration、purpose comment、`Succeeded:` return、条件分割、for 初期化子なし）を自己確認。

制限: 実機未接続のため hardware 動作は未証明。engine/LRC/submission は p004。`GPU_CAP_*` は 0 のため `/dev/gpu0` は open/get_info しかできない。

## q314 p003 完了: メモリ（GEM object、48-bit PPGTT、CPU view）（2026-09-14）

[WS029 p003](https://github.com/awemorris/zedBSD/issues/400)（q314-i03）を cleared にし、[p004](https://github.com/awemorris/zedBSD/issues/401)（q314-i04、実行）を in-progress にする。実機は未使用。HAL/UAPI 変更なし。git add/commit/push はユーザー担当。

新規・変更 source（すべて local、Zlib）:
- `src/drivers/gpu/i915/ppgtt.c`: `drv_i915_ppgtt_create`（scratch page → scratch PT/PD/PDP の連鎖、PML4 は scratch PDP で充填）、`destroy`、`va_alloc`（bump、開始 0x1_0000_0000、2 MiB 揃え、再利用なし）、`insert`（4-level walk、欠けた table を 4 KiB page で確保し下位 scratch entry で充填）、`clear`（leaf を scratch に戻す）、`lookup`（試験用）。encode: page/table = `GEN8_PAGE_PRESENT|GEN8_PAGE_RW`（PAT index 0）、scratch table は Linux の `gen8_pde_encode(I915_CACHE_NONE)` と同じ `PAT0|PAT1`。
- `src/drivers/gpu/i915/gem.c`: `drv_i915_gem_create`（`kern_pmem_alloc_limited` 4 KiB 揃え・39-bit 上限・zero fill、device list に登録）、`destroy`（bound/quarantined なら保持）、`bind/unbind_ggtt`、`bind/unbind_vm`、`read/write`（範囲再検査、barrier）。
- `src/drivers/gpu/i915/internal.h`: `struct i915_ppgtt`/`i915_ppgtt_page`/`i915_gem_object`、session は `vm` を別 allocation で保持（quarantine 時に close で device の `quarantined_vms` に移し、reset/stop で解放）、device の object list と counter。
- `src/drivers/gpu/i915/i915.c`: capabilities = `GPU_CAP_RESOURCE|GPU_CAP_TRANSFER`、`open`（session 番号、PPGTT 作成）、`close`（PPGTT 破棄または quarantine 保持）、`resource_create`（`GPU_RESOURCE_USAGE_STORAGE` のみ、1..16 MiB、VM へ bind、log `i915: resource session=%u slot=%u bytes=%llu phys=0x%llx va=0x%llx`）、`resource_destroy`（quarantine 時は保持）、`resource_read/write`（mutex 下で copy）、`i915_stop` が残存 object と quarantined VM を解放。
- build: `platform/amd64/vmunix.mk` に `ppgtt.c`/`gem.c`。監査に `gt/intel_gtt.c`（MIT）を追加（29→30 ファイル）。
- fixture: `plan/ws029/tests/i915-gtt-test.c` に PPGTT 3 試験（scratch 連鎖と encode 値、index bit ごとの walk と table 確保数、VA allocator）、`i915-backend-test.c`（新規: 登録→attach→publish→open/get_info→create/write/read/destroy→close→unpublish→detach、6000 B が 2 page に丸められ VA 0x1_0000_0000、16 MiB 上限、EINVAL/ENOMEM 巻戻し、quarantine 保持と detach での回収、lease 全解放）、`i915-fixture.inc` に mutex/PCI attach/drv_gpu 登録の stub と 32 MiB の偽 page pool。

検証（agent-1）:
- `make BUILD=build/i915-amd64 ZEDBSD_CONFIG=plan/ws029/tests/config-i915-amd64.mk vmunix`: PASS（warning 0、`amd64 vmunix check: PASS`、`drv_i915_*` 28 symbol）。I915 := n: PASS、symbol 0。
- `sh plan/ws029/tests/run-i915-host-tests.sh`（uncore/gtt/irq/backend、通常＋ASan/UBSan）: PASS。
- `python3 plan/ws029/tests/run-i915-build-selection-test.py`: 6 platform × Venus/i915 各 y/n PASS（i915 object 6 個は amd64 かつ y のときだけ）。
- `git diff --check`: PASS。

判明した事項: quarantined session の close で page table が漏れる設計穴を fixture が検出し、VM を別 allocation にして device 側 list へ移す形に直した（close は失敗できない契約のため close 時に allocation しない）。

制限: 実機未接続。`GPU_CAP_COMMAND`/JOB は p004–p005。resource 上限は 1 object 16 MiB、物理連続。

## q314 p004 完了: 実行（engine/LRC/execlists、request/seqno、engine reset、selftest）（2026-09-14）

[WS029 p004](https://github.com/awemorris/zedBSD/issues/401)（q314-i04）を cleared にし、[p005](https://github.com/awemorris/zedBSD/issues/402)（q314-i05、drv_gpu 統合）を in-progress にする。実機は未使用（selftest の実行は p006）。HAL/UAPI 変更なし。git add/commit/push はユーザー担当。

新規・変更 source（すべて local、Zlib。転記 `.inc` は MIT 表示付き）:
- `linux/i915-commands.inc`（63 定義: MI_*/XY_*/PIPE_CONTROL）、`linux/i915-lrc-offsets.inc`（`gen12_xcs_offsets`/`gen12_rcs_offsets` と NOP/LRI/REG/REG16/END の encode macro を `I915_LRC_*` に改名して verbatim 転記）、`linux/i915-mocs.inc`（LE_/L3_/L4_ macro、`MOCS_ENTRY`、`GEN11_MOCS_ENTRIES`、`gen12_mocs_table`）。generator `plan/ws029/tests/gen-inc.py` に verbatim block 転記と `BUILD_BUG_ON_ZERO` 除去を追加。`i915-regs.inc` に `GEN11_GRDOM_RENDER`、`GEN9_LNCFCMOCS`、`BLIT_CCTL_*_MOCS_MASK`、`GEN12_GFX_PREFETCH_DISABLE` を追加（168 定義）。
- `engine.c`: `drv_i915_engines_start`（forcewake GT+RENDER を device 寿命で保持、global MOCS 64 entry と LNCFCMOCS 32 pair を書込み、RCS0/BCS0 の HWSP object と kernel context を作成し `i915_engine_program`: HWSTAM、`GEN11_GFX_DISABLE_LEGACY_MODE`、STOP_RING 解除、HWS_PGA、EMR/EIR/ESR、BCS の `BLIT_CCTL` を uncached MOCS index 3、CSB pointer reset）、`drv_i915_engine_reset`（STOP_RING+PREFETCH_DISABLE→MODE_IDLE 待ち→`RESET_CTL` request/ready→`GDRST` engine domain（2 回書き）→cancel→再 program）、`drv_i915_engine_interrupt`（irq_lock 内で CSB 消費→seqno retire→次 request 投入、lock 外で完了 callback）、`drv_i915_engine_idle`。
- `lrc.c`: `drv_i915_lrc_create`（RCS 14 page / BCS 2 page の image を GGTT に bind、context ごとに 64 KiB ring、offset 列を `MI_LOAD_REGISTER_IMM|LRM_CS_MMIO(|FORCE_POSTED)` に展開、CONTEXT_CONTROL の inhibit、PDP0=PML4、MI_MODE pair の STOP_RING 解除、RING_START/HEAD/TAIL/CTL、末尾 `MI_BATCH_BUFFER_END|1`、descriptor low=64B addressing|VALID|PRIVILEGE|GGTT、high=sw_id<<5|class<<29|instance<<16）、`submit`（image tail 更新→ELSQ port1=0/port0=desc|FORCE_RESTORE→`EL_CTRL_LOAD`）、`reset_csb`、`csb_consume`（HWSP write pointer、entry -1 なら mmio mirror、gen12 parse: away 無効または new queue で promotion、それ以外 completion）、`ring_space`/`ring_emit`（末尾 NOOP 詰めで wrap）。
- `request.c`: slot 32、FIFO queue、`kick`（engine idle 時のみ emit+submit: preparser disable→TLB invalidate flush(BCS: MI_FLUSH_DW、RCS: PIPE_CONTROL)→extra dwords→`MI_BATCH_BUFFER_START_GEN8|NON_SECURE`(48-bit PPGTT)→breadcrumb（BCS: flush + `MI_FLUSH_DW` post-sync store to HWSP seqno via GGTT、RCS: PIPE_CONTROL flush + QW_WRITE）→`MI_USER_INTERRUPT`→ARB enable→ARB_CHECK/NOOP）、`retire`（HWSP seqno 一致）、`fail`（queue/active を error で回収）、`complete_list`（`drv_gpu_complete` を lock 外で呼び slot 解放、`drv_gpu_capacity_changed`）。
- `selftest.c`（`CONFIG_DRIVER_PCI_I915_SELFTEST=y` のみ link）: kernel context で BCS0 に `MI_STORE_DWORD_IMM|USE_GGTT`（HWSP scratch dword に 0xdeadbeef）を含む request を投入し 100 ms 以内に値・seqno・user interrupt 増加を確認、`i915: selftest bcs0 store=%s irq=%u seqno=%u/%u`。失敗は attach 失敗。
- `irq.c` が engine へ転送、`i915.c` は attach で engines start（+selftest）、open で engine ごとの context 作成、close で破棄（quarantine 時は保持）、stop で GT reset→engines stop→object 回収。`uncore.c` に `drv_i915_domain_reset`。`platform/amd64/vmunix.mk` に engine/lrc/request と条件付き selftest、`plan/ws029/tests/config-i915-selftest-amd64.mk`。
- fixture: `i915-lrc-test.c`（新規: image layout が Linux の CTX_* index と一致、descriptor、ELSQ 書込み、CSB 判定と mirror fallback、ring wrap/space）、`i915-fixture.inc` に execlists emulator（ELSQ load を記録し `fixture_run_engines()` が image→ring を parse: MI_STORE_DWORD_IMM(GGTT/PPGTT)、MI_FLUSH_DW store、PIPE_CONTROL QW write、MI_BATCH_BUFFER_START を PPGTT 経由で追跡、MI_SEMAPHORE_WAIT で hang、CSB event 2 件と割込み identity を作り handler を呼ぶ; MI_MODE/RESET_CTL/GDRST の応答）、`i915-backend-test.c` に engine 初期化 register 値、selftest 成功、batch による resource 書込みと完了 callback、hang→`request_fail(EIO)`→engine reset→再実行を追加、`i915-irq-test.c` に engine 転送の確認。

検証（agent-1）:
- 3 構成 build PASS（i915、i915+selftest、GPU なし: `drv_i915_` symbol 0）、warning 0、`amd64 vmunix check: PASS`。
- `sh plan/ws029/tests/run-i915-host-tests.sh`（uncore/gtt/irq/lrc/backend、通常＋ASan/UBSan）: PASS。
- `python3 plan/ws029/tests/run-i915-build-selection-test.py`: PASS。`git diff --check`: PASS。

設計との差分: ring は engine 共有ではなく context ごと（execlists は context save で RING_HEAD を image に書き戻すため、共有 ring では head/tail が食い違う。Linux と同じ構成）。forcewake は attach 後に恒久保持（IRQ 文脈からの ELSQ 書込みで ACK 待ちを避けるため）。`CTX_R_PWR_CLK_STATE` は 0（RCS の 3D 利用は WS029 後続）。

制限: 実機での CSB/割込み挙動は未証明（fixture の model は Linux の parse 規則に基づく）。1 engine 1 request 直列。`GPU_CAP_COMMAND`/JOB/recovery ops は p005。

## q314 p005 完了: drv_gpu 統合、native stream、生 UAPI 試験クライアント（2026-09-14）

[WS029 p005](https://github.com/awemorris/zedBSD/issues/402)（q314-i05）を cleared にし、[p006](https://github.com/awemorris/zedBSD/issues/403)（q314-i06、VFIO passthrough テストループ）を in-progress にする。実機は未使用。HAL/UAPI 変更なし。git add/commit/push はユーザー担当。

新規・変更（すべて local）:
- `plan/ws029/i915-native-stream.md`: native stream の確定版（header 32 byte、relocation 16 byte、engine/timeline の対応、job の意味、試験 batch、harness 向け handle 対応付け）。
- `src/drivers/gpu/i915/i915.c`: `drv_i915_stream_parse`（magic/version/engine/count/dwords/flags/reserved/bytes 一致/末尾 `MI_BATCH_BUFFER_END`/relocation 範囲を検査）、`i915_submit_stream`（session の batch pool（最大 32、空き object を再利用）へ copy、relocation を handle→session object の VA で patch、request を投入）、`i915_submit_marker`、`i915_command`（同期受理）、`i915_command_submit`（bytes 0 は marker、timeline 0/1=BCS0、2=RCS0）、`i915_command_drain`（`retire_waitq` で pending 0 まで待つ）、jobs（`reserve`: slot 確保＋callback 保持、`commit`: marker 投入、`cancel(0)`: slot 解放、`cancel(fault)`: RETAINED で保持、`capacity`: 空き slot 数）、recovery（`stop_begin`/`stop_poll`(pending で EAGAIN)/`isolate`(session の request を EIO で回収し、active なら engine reset して他 session を継続)/`fault`(全 request 回収、`failed=1`)/`reset_device`(GT reset、engine 再 program、quarantined object/VM 解放)）。capabilities = RESOURCE|TRANSFER|COMMAND|NOTIFICATION|JOB|JOB_CAPACITY。resource log に `handle=` を追加。
- `request.c`: RESERVED/RETAINED slot も `request_fail` で回収、retire 時に batch を pool へ戻し `retire_waitq` を起こす。`engine.c`: `drv_i915_engine_recover`（irq_lock 外で engine reset、`resetting` 中は handler と kick が待つ）。`internal.h`: stream 定数、session の object/batch list、`retire_waitq`。
- `userland/base/tests/gpu-i915/{main.c,Makefile}`（`/bin/gpu-i915-test`、libvulkan 非依存）: `GPUI915 START` → GET_INFO（driver_name/capabilities）→ 64 KiB resource ×2 → pattern write → copy（`XY_SRC_COPY_BLT`）→ 照合 → fill（`XY_COLOR_BLT`）→ 照合 → store（`MI_STORE_DWORD_IMM`）→ 照合 → job（RESERVE/COMMIT/WAIT）→ `GPUI915 PASS copy=1 fill=1 store=1 job=1 src_handle=<h> dst_handle=<h>`、失敗は `GPUI915 FAIL stage=<s> errno=<e>`。両 config の `ZEDBSD_USER_PROGRAMS` に追加。
- fixture: `i915-stream-test.c`（新規、正常 3 形と不正 13 形）、`i915-fixture.inc` に waitq stub と `XY_SRC_COPY_BLT`/`XY_COLOR_BLT` の emulation、`i915-backend-test.c` に ops 経由の stream（relocation patch、copy/fill の結果照合、不正 handle/長さ拒否）、marker、drain、jobs（capacity 32→31、rollback、commit 完了、fault cancel の保持）、stop_begin/poll、isolate（hang した session を engine reset で切り離し peer の request が継続）、fault→open ENODEV→isolate→close→reset_device→再 open 成功。

検証（agent-1）:
- `sh plan/ws029/tests/run-i915-host-tests.sh`（uncore/gtt/irq/lrc/stream/backend、通常＋ASan/UBSan）: PASS。
- kernel build 3 構成 PASS（warning 0）、`make ... disk-image`（i915 config）PASS、`build/i915-amd64/rootfs/bin/gpu-i915-test` を確認。
- `run-i915-build-selection-test.py` PASS、`git diff --check` PASS。

制限: 実機での blit/interrupt は未証明（p006/p007）。RCS0 の 3D state は対象外（`MI_STORE_DWORD_IMM`/`PIPE_CONTROL` 経路のみ）。

## p006 実機結果（VFIO passthrough テストループ、2026-09-14）

Latitude 5330（`awe@10.0.10.25`）の IGD（`0000:00:02.0`、`8086:46a8` rev 0c）を `host-igd.sh` で vfio-pci に切替え、i915 selftest 付き image を QEMU で起動して attach → selftest → `/dev/gpu0` 公開まで到達した。各 attempt の後に i915 と GDM を復旧した。

### 復旧 rehearsal（QEMU なし）

`plan/ws029/phase006/host-rehearsal.json`。`attach` で driver=vfio-pci、`/dev/vfio/0` 出現、GDM inactive。`restore` 後 driver=i915、GDM active、`driver`/`gdm`/`dev_vfio`/`drm_nodes` が開始前と一致（`restored=true`）。復旧手順が成立することを確認。

### attempt 履歴（同条件の修正と再実行）

| attempt | mode | 結果 | 原因・修正 |
| --- | --- | --- | --- |
| boot-001 | boot-only | fail | harness の import 依存 `venus_rfb.py` を転送していなかった → 転送一覧に追加 |
| boot-002 | boot-only | fail | UEFI loader が GOP framebuffer 無しで `Locate GOP` 停止 → QEMU 引数を `-vga none` から `-vga std`（表示 backend なし）に変更 |
| boot-003 | boot-only | fail(attach) | i915 が `map-registers` で EINVAL → BAR0 を `drv_pci_device_claim_bar` してから map するよう修正（PCI は claim した BAR しか map させない） |
| boot-005 | boot-only | 進捗 | BAR0 は正しく map（regs va=0xffffffffe0000000, bus=0x380010000000）、GGTT/MSI まで到達、engine bring-up で停止 → 段階 log 追加 |
| boot-006 | boot-only | 進捗 | 両 engine init と `engines started` まで到達、selftest で停止 → selftest に log 追加 |
| **boot-007** | boot-only | **pass** | `i915: selftest bcs0 store=ok irq=1 seqno=1/1`、`registered native GPU node`。実機の BCS0 が store を実行し user interrupt を上げた |

### 受入

- attach 全段階（PCI enable、BAR0 claim/map、forcewake、GT reset、GGTT、MSI、engine×2、selftest、publish）を通過。
- `boot-007` boot-pass: guest.log に `attach stopped` なし、`selftest bcs0 store=ok`、`registered`。
- host 復旧: attempt 後 driver=i915、GDM active（`host_restored=true`）。attach 中は driver=vfio-pci、`/dev/vfio/0`、GDM inactive。
- 証拠: `plan/ws029/temp/remote/q314-i915-boot-007/`（result.json、guest.log、qemu.log、qmp.jsonl、console）、`plan/ws029/phase006/host-rehearsal.json`。

実機で GPU が実際に命令を実行した最初の到達点。copy/fill/store/job と host 側 RAM 照合は p007。

## q314完了: WS029 i915ネイティブGPU driver（2026-09-14）

q314 finished、ws029-p001..p007 全 cleared。実機（Latitude 5330、IGD 8086:46a8、VFIO passthrough）で attach → selftest（BCS0 が store を実行し user interrupt）→ /dev/gpu0 公開まで到達し、userland /bin/gpu-i915-test が copy/fill/store/job を実行、host が QMP pmemsave で guest RAM を独立照合して PASS（test-002）。Linux i915（MIT）の定義/テーブルは出典付き .inc へ転記、driver 論理は zedBSD 規約で新規実装。HAL/UAPI 不変。静的解析 gcc -fanalyzer / clang --analyze 0 件、規約 §14 確認、host fixture・GPU core 回帰・build 3 構成 PASS。制限: cold VFIO attach の bring-up 間欠ハング（最優先の後続）、hang 注入の実機 peer 継続未達、display/scanout は対象外。後続は WS029 registry の planning 行に列挙。source/doc の git add/commit/push はユーザー担当。