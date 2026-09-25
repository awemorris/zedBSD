<!-- awesome-plan project=zedbsd record=ws014 -->

# WS014: virtio-gpu bring-up

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: p004（最終 API と規約の確認）が planning・未 queue
<!-- awesome-plan-current:end -->

## 単一の目標と完了条件

QEMUのvirtio-gpu上でzedBSDのGPU/表示経路を成立させ、宣言した描画・表示APIから描画内容を画面にpresentできること。初期環境はamd64 QEMUを候補とし、p001でdevice/backend/versionと受け入れ条件を固定する。Vulkan公開API案の可否・対象profileは未確定であり、2D出力だけをVulkan対応完了と呼ばない。

## 範囲と段階

1. p001で層分け、Vulkan表示API案、最小OS ABI、QEMU環境と有限実装Phaseの必要判断を供給する。
2. virtio PCI/virtqueueと2D resource/scanout/transfer/flushによる画面更新、boot framebufferからの安全な切替を先に検証する。
3. 選定したVulkan実装とtransport/WSIを接続し、宣言したAPIで描画・presentする。画面列挙・mode選択・同期と資源回収を確認する。
4. p006で別processの最小Waylandクライアント/コンポジタへGPU allocationを共有し、GPU内で画像を扱えるWSIと全画面表示を成立させる。
5. console/graphics fallbackと権限分離を含む結合確認、変更ソースに対する適用規約全文確認をp004へ含める。

i915実機対応、GLES2実装、デスクトップ全体の移植はこの単一目標に混ぜず、後続で選択された場合に別WSで扱う。旧案の内容と判断履歴は下に保持する。既存WS004/WS007等の実装責任はこの計画更新だけで移管しない。

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| ws014-p001 | [設計判断](https://github.com/awemorris/zedBSD/issues/213) | planning | interface/PCI/所有権の必要判断を供給。未決定を自動clearしない |
| ws014-p002 | [ws014-p002](https://github.com/awemorris/zedBSD/issues/383) | cleared | frameworkのみ |
| ws014-p003 | [ws014-p003](https://github.com/awemorris/zedBSD/issues/384) | cleared | QEMU＋VenusループとAPI改善 |
| ws014-p005 | [標準APIの3D shader/API検証](https://github.com/awemorris/zedBSD/issues/387) | cleared | WS030標準library＋q308実測で訂正完了。p004へ引き渡し |
| ws014-p006 | [kernel handle・GPU共有・最小Wayland](https://github.com/awemorris/zedBSD/issues/393) | cleared | q309でK handle/GPU共有/最小Wayland実装、最終direct002・Wayland004受入。p004へ引渡し |
| ws014-p007 | [GPUレビュー・BLOB直接表示・同期と性能](https://github.com/awemorris/zedBSD/issues/394) | cleared | q310 finished。BLOB直接表示・標準fd・同期/転送・topology/配置と実QEMU受入済み |
| ws014-p008 | [GPU完了責任・fence所属と描画資源](https://github.com/awemorris/zedBSD/issues/395) | cleared | q311-i01 finished、承認回答A1–A8＋fence所属の改善を受入 |
| ws014-p009 | [GPUレビュー対応とフレームワーク共通化](https://github.com/awemorris/zedBSD/issues/396) | cleared | q312-i01、R1–R6とGPU共通の待機/期限/所有権、backend停止契約・WSI改善と受入 |
| ws014-p010 | [GPU監督の共通化仕上げと局所隔離](https://github.com/awemorris/zedBSD/issues/397) | cleared | q313-i01、self-review D1–D4/B3–B6とVenus→framework移管、session隔離、実QEMU受入 |
| ws014-p004 | [ws014-p004](https://github.com/awemorris/zedBSD/issues/385) | planning | p010後の最終API整理・規約全文確認 |

## 制約・再開点

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの適用規約に従う。HAL責務変更は既存の承認条件を守る。最初に現行PCI/DMA/interrupt/console/graphics基盤を確認し、リファクタリング前のコード配置を仮定しない。実装前に有限Queueの承認が必要。

q308でp005を標準Vulkan APIへ訂正し、3D shader/texture/depth、回転、正常・異常終了後の再open、console/所有権を実測確認した。次はp006のkernel handle・GPU共有・最小Wayland。p004の最終framework/API整理はp006後の別の未queue作業。p001の未決定を保持する。

## 2026-09-12 GPU計画更新

ユーザー指示により、[WS014](https://github.com/awemorris/zedBSD/issues/15)の初期bring-up対象をi915からQEMU virtio-gpuへ変更する。未完了・未着手の既存目標の具体化であり、終了WSの再利用ではない。[p001](https://github.com/awemorris/zedBSD/issues/213)の設計検討の手動保留を解除しplanningとする。実装Queueは作成・再開しない。WS009など他WSの保留は自動解除しない。

Vulkanのディスプレイ拡張をOSの公式なユーザー向け表示APIとする案を検討する。正式採用やABI凍結は未決定。Linux DRM互換を必須としない従来方針は維持するが、メモリ管理、同期、画面出力、所有権・権限を担うOS/ドライバ機構は必要。Vulkan APIをそのままカーネルABIへコピーしない。

## API案と検証上の区別

- `VK_KHR_display`はdisplay/mode/planeの列挙とdisplay surface作成を提供し、`VK_KHR_swapchain`と合わせてウィンドウシステムを介さず表示する構成を取れる。DE/compositorが使う公式ユーザー空間APIの候補とする。通常アプリに画面の排他的制御を無条件で与えない。
- VulkanはOSのカーネルABIを規定しない。Linux DRM互換を採用しなくても、zedBSD側にはdevice/contextの権限、buffer寿命・mapping・DMA、submit/fence、scanout・表示所有権・復旧を実装する必要がある。正確なカーネル/ユーザー空間分担と最小ABIはp001で設計する。
- `VK_EXT_acquire_drm_display`は既存DRM経由の所有権取得用であり、DRMが不要になる根拠ではない。zedBSDでDRMを使わない場合は独自の所有権管理へWSIを接続する。
- `VK_KHR_display`だけを完全なOS表示管理仕様とはみなさない。hotplug、カーソル、電源、複数表示、console/panic切替などは必要な拡張とOS側の不足分を整理する。
- QEMUの標準virtio-gpu 2Dはscanout用で、Vulkanの描画実装そのものではない。加速VulkanはVenus/virglrendererまたはgfxstreamなど別経路が必要。Venusは候補であり採用確定ではない。ソフトウェアVulkanも選択肢だが移植済みとは扱わない。
- 既存Mesa/Venusを利用する場合はLinux DRM依存のtransport、buffer、同期、WSIを監査し、zedBSD ABIへ接続する移植量を評価する。Venus描画の成功だけでゲストdisplay surfaceへのpresent成功を推定しない。ホストがDRMを使うこととゲストzedBSDがDRM ABIを公開することは別。

公式資料（2026-09-12参照、設計判断は上記仕様からの推論）:
- [VK_KHR_display](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_display.html)
- [VK_EXT_acquire_drm_display](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_acquire_drm_display.html)
- [QEMU virtio-gpu](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
- [Mesa Venus](https://docs.mesa3d.org/drivers/venus.html)

## 2026-09-12 Vulkan API関数別の責務表

ユーザー依頼により、Vulkan 1.0〜1.4の全コア234関数と選択した表示関連拡張41関数、計275関数について、libvulkan.so側（loader/ICD/WSIを含むユーザー空間実装）とzedBSD GPUドライバ側の責務を一関数一行の表にした。

[WS014 p001の関数別責務表](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)に全文を掲載する。固定したKhronosレジストリとの集合照合で欠落・重複・空欄なし。libvulkan.so単体の構成とloader/ICD分離の違い、Venus転送、記録/submit/表示の違い、対象外拡張、対応宣言ではないことを明記した。

設計資料でありAPI/ABI採用確定やPhaseクリアランスではない。WS014/p001はplanning、Queueは未開始。Markdownはローカル作業ツリーにも保存し、git commit/pushはしていない。

ローカル資料: [Vulkan API関数別責務表](vulkan-api-responsibilities.md)。

## 2026-09-12 責務分類をU/Kに統一

ユーザー指示により、[Vulkan API責務表](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)の275関数を、U（ユーザー空間実装）とK（GPUドライバ）の二つの責務欄だけで整理した。旧Q/C/R分類と境界列を削除。キャッシュ・記録・転送は責務欄の説明として保持する。ドライバへの照会はK、結果の整形等はUであり、キャッシュ可能性を別分類にしない。関数集合とplanning状態、Queue未開始は維持。

## 2026-09-12 GPUドライバ関数インタフェース案

ユーザー依頼により、[同じ責務資料](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)へK側インタフェース44件の表を追記。仮の関数シグネチャ、入力・出力、Kの責務、対応するVulkan APIを記載した。接続/context、resource/mapping、transport/submit/sync、display/event、および任意機能の群に整理し、初期2Dと後続Venusの範囲を区別した。

275関数のU/K表は保持。今回の関数名・型・構造体は設計案で、実装済み/ABI確定ではない。WS014/p001はplanning、実装Queueなし。git commit/pushなし。

## 2026-09-12 GPU interfaceをcallback構造体へ変更

ユーザー判断により、[GPU責務資料](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)の44操作をstruct drv_gpu_interfaceの関数ポインタメンバーへ変更。個別drv_gpu_*関数の公開案を置換した。PCI側がattach成功後にinterface/private data等をGPUコアへ登録し、GPUコアが/dev/gpuNを公開・dispatchする。detach/rollbackと参照寿命も記録した。

現行PCI attachはint戻り値のみでGPU登録の引渡し機構は未実装。構造体とPCI側class/service連携の詳細は設計事項。275関数のU/K分類を維持し、コード・Queue・Phase状態は変更しない。

## 2026-09-12 GPU実装の段階化

ユーザー指定の順序をPhase化: [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)（GPUフレームワークのみ）→[ws014-p003](https://github.com/awemorris/zedBSD/issues/384)（QEMU＋Venusの画面取得・自動デバッグ、API不足の修正）→[ws014-p004](https://github.com/awemorris/zedBSD/issues/385)（最終API整理・規約全文確認）。既存p001は設計判断を供給し、未決定を完了扱いしない。次段階のi915ネイティブ実装は単一目標の[ws029](https://github.com/awemorris/zedBSD/issues/386)へ分離する。

Linux i915＋ANVホスト、egl-headless＋QMP screendump、frame更新によるキャプチャ検証、serial/画像/renderer証拠の保存をp003へ記録。実ホストでの動作は未確認。275関数のU/K表と44callback案は出発点で、p002/p003の実装結果により不足を補い整理する。実装Queueは未開始。資料のgit add/commitはユーザーが行い、エージェントはadd/commit/pushしない。

## q304実行開始（履歴）

ユーザー承認の唯一の実装対象は[WS014 p002](https://github.com/awemorris/zedBSD/issues/383)。GPUフレームワークのみを実装し、コーディングスタイル全文に従う。p003/Venus/i915は未開始。p001の必要contractをp002に具体化し、p001全体をclearしたとは扱わない。

Queue: q304 / attempt: q304-i01 / active・in-progress。時間枠は120 active minutes、内容はこの単一Phaseに限定する。重大な未解決仕様・外部blockが生じた場合は根拠と再開条件を記録する。git add/commit/pushは行わない。

## q304実行結果（2026-09-12）

q304-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)をclearedとし、単一PhaseのQueue q304をfinishedにした。GPUフレームワーク、5 callbackのstruct drv_gpu_interface、PCI所有のservice公開・解除、/dev/gpuN、session/世代handleを実装した。

実gpu.c/cdev.cと最小backendの通常・ASan/UBSanテスト、ILP32/LP64の固定ABI照合、実pci.cのlifecycleテスト、amd64対象kernel buildがPASS。全文規約レビューで目的コメント・参照寿命を確認し、git diff --checkもPASS。clang-format 19.1.7のdry-runは全文規約と衝突する関数定義/forward declaration整形等を指摘したため非zeroで、機械的整形は適用していない。詳細はPhase本文の検証記録。

WS014はincomplete。p001の残る設計判断とp003/p004はplanning、Venus/実GPU描画/i915は未開始。次はp003でmmap、submit/sync、display等の不足を実利用から補う。現在の実行Queueはなし。コードと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。

## q305実行開始時の範囲（履歴）

ユーザーがPCI公開serviceと起動時publishをGPUヘッダへ出す設計を見直し、普通の動的ops登録APIへ変更する案に「では、実装を変更してください」と実行を指示した。q305 / q305-i01はWS014 p002の同一目標内の修正のみ、120 active minutes。p002をin-progressへ戻し、旧q304の結果と試験証拠は履歴として保持する。WS014はincomplete、p003/Venus/i915は開始しない。

公開APIはdrv_gpu_register(const struct drv_gpu_ops *, void *, struct drv_gpu_device **)とdrv_gpu_unregister(struct drv_gpu_device *)。ユーザーが変更したops名を保持。GPUのPCI依存、registrationラッパー、public service/publish API、固定8台配列を除く。PCI側の通常service経路から共通APIを呼べることをfixtureで検証する。

共通cdevの固定16個制限とdevfsの固定snapshotを動的化し、VFSの初期化は既存登録を破棄しない。GPU専用の再公開を不要にする。複数deviceのops共有とprivate data分離、16台超の登録/列挙、早期登録のmount後存続、通常/失敗/解除の参照寿命を確認する。既存resource ioctl契約は維持する。

コード変更前に計画・Issue・Projectを同期して読み戻す。全文coding-styleに従い、限定GPU/PCI/cdev/devfs test、ASan/UBSan、32/64bit ABI、amd64対象make -j16 buildを実行。HAL責務/hal.h変更、aggregate make check、git add/commit/pushは行わない。

<details>
<summary>2026-09-12より前の計画（i915 first・手動保留は上記判断で変更）</summary>

# WS014: native GPU stack



Last updated: 2026-08-27

WSID: `ws014`

Status: Blocked; future architecture discussion on manual hold

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: none until the user explicitly resumes GPU architecture
discussion. Do not transfer implementation ownership from WS004/WS007 or
publish `/dev/gpuN` UAPI while the manual hold is active.

Shared reviews: [WS014 review index](tests/README.md)

## Goals

- Define a versioned, driver-independent zedBSD GPU UAPI at `/dev/gpuN`.
- Implement i915 first without exposing i915-specific userspace ioctls.
- Provide a declared Vulkan profile and OpenGL ES 2.0 on that implementation.
- Allow a GPU driver to take over scanout from the boot framebuffer with safe
  console/graphics fallback.

## Objective

Separate the native GPU stack from the broader hardware and desktop WSs, then
resolve its object, memory, submission, synchronization, display, security, and
capability contracts before implementation begins.

## Scope

- `/dev/gpuN` versioning, discovery, handles, contexts, queues, memory/images,
  synchronization, validation, errors, teardown, and permissions;
- mandatory graphics and reduced GLES2-class profiles, with compute optional;
- userspace Vulkan and GLES layering over the kernel UAPI;
- `/bin/gpu` diagnostics;
- boot framebuffer, `/dev/console`, and `/dev/graphics` provider takeover;
- i915 ownership and its WS004 PCIe/DMA/MSI/firmware dependencies;
- later WS007 X11/Wayland consumers and WS008 BeUI consumers.

## Non-goals

- Linux DRM or driver-specific ioctl compatibility;
- claiming full Vulkan before a profile and conformance boundary are published;
- requiring compute shaders on every supported GPU;
- implementing GPU code during the discussion Phase.

## Dependencies

- [WS004](../ws004/ws.md) owns reusable PCIe, DMA, interrupt, power,
  and firmware foundations.
- [WS007](../ws007/ws.md) owns X11 and Wayland desktop integration.
- WS006 owns evdev input; WS008 owns Noct/BeUI; WS009 owns public references.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws014-p001` | [GPU architecture discussion](phase001/phase.md) | Blocked by manual hold | Resume later to freeze UAPI/profile/display/i915 boundaries and the Phase map |

No implementation Phase is defined until `ws014-p001` is complete.

## Confirmed product direction

- `/dev/gpu0` is the first instance of `/dev/gpuN`.
- The public interface is driver-independent and only thinly exposes the
  primitives needed by the declared Vulkan-like graphics model.
- Compute may be unsupported; GLES2-class hardware may use a reduced profile.
- `/bin/gpu` communicates with the device through the public UAPI.
- GPU initialization may replace the VBE/GOP linear-framebuffer provider, but
  fallback and panic output must remain defined.
- i915 for the Latitude 5320 is the first hardware target.

## WS completion direction

The current planning-stage WS may pause after p001 produces fixed architecture
and implementation Phase decomposition. UAPI or driver completion conditions
are deferred until the capability profile and acceptance environments exist.

## Reconsideration boundaries

Reconsider if the common UAPI requires driver-private command formats, cannot
validate untrusted submissions, cannot recover resources on process exit or
GPU reset, or cannot preserve console output during display takeover failure.

</details>

## q305実行結果（2026-09-12）

q305-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)の修正を完了し、Phaseをcleared、Queue q305をfinishedとする。ユーザー指定どおり、GPUコアは通常の `drv_gpu_register(ops, private_data, **device)` / `drv_gpu_unregister(device)` で個々のdeviceを登録・解除する。GPU公開ヘッダのPCI依存、registration wrapper、専用service table、一括publish APIを除いた。同じ `struct drv_gpu_ops` を共有する複数deviceがそれぞれのprivate dataを持つ。

GPUと共通cdev/devfsの固定台数制限を動的registry・snapshotへ変更し、VFS mount時の登録消去を除いた。早期・追加登録を通常のdevfs経路で扱う。使用中のunregisterはEBUSYでhandle/backendを保持し、解除成功後はhandleを消費する。古いinodeは世代の異なるdeviceや解放済みbackendへ接続しない。既存5 callbackとresource ioctlの責務は維持する。

実GPU/cdev/PCI coreを使う40 GPUの通常・ASan/UBSan試験、ILP32/LP64のUAPI照合、共通cdev/devfsの80 device登録・全件列挙・mount・割当失敗・世代と参照寿命の試験がPASS。共通層の限定runnerは既存GCC -fanalyzer gatesを含めPASS。amd64対象kernel buildとvmunix checkerもPASS。変更箇所の適用コーディング規約全文とlifetime/rollbackをレビューした。最終source hashと手順は `plan/history/queue-q305.md` に保持する。

WS014はincomplete、p001/p003/p004はplanning。QEMU＋Venus・i915は未実行で、新しいactive Queueはない。GPUフレームワーク資料とp001の責務表を現行ops/登録契約へ更新した。ソースと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。GitHubは計画Issue・Projectを同期し、本文・native lifecycle・Projectフィールドを読み戻す。

## q306: Venus実装・リモートQEMUループ（ユーザー実行指示）

2026-09-12、ユーザーがp002をレビューし「これはOK」と受け入れ、p003完了までの環境確認・実装・QEMU実行を指示した。対象はユーザー提供の `awe@10.0.10.25`。実装は `src/drivers/gpu/venus/` に置き、Venus用のdriverとして構成する。汎用virtio-gpu driverへの抽象化を要求しない。q306 / q306-i01はこの単一Phaseだけを選択する。p002はclearedを維持し、ユーザーのcommit `3236b560` に取り込まれている。

環境確認済み: Debian 13.6 / Linux 6.19.13、Intel Iris Xe 8086:46a8 / i915、Mesa ANV 25.2.6 / Vulkan 1.4.318、QEMU 10.0.11、virglrenderer 1.1.0。KVM API 12、renderD128、udmabufの利用権、Venus/blob/hostmem/egl-headlessオプション、外部メモリ等の必要候補featureを確認。実際のVenus描画はこれから検証する。Intel ICDを指定し、ソフトウェアrendererの誤認を防ぐ。ホスト上の専用作業ディレクトリと使い捨てimageを使用する。

受け入れはzedBSDゲスト内の最小2DとVulkanテスト描画、frame更新のQMP取得・期待画像照合、変更→再build→再起動→新frame照合の再現可能なループ。serial/QMP/QEMU・rendererログ、source/image hash、起動引数、選択したGPUとversionを試行単位で保存する。ホスト単独のvkcubeやcommand提出ログだけではclearしない。全Vulkan適合・物理表示timingは受け入れ外。

既存PCI/DMAと通常のGPU ops登録を使用し、Venus内にPCI virtqueue、capset/context/blob、command/reply転送、完了確認、scanoutを実装する。必要なGPU callback/UAPIを実利用から追加する。初期経路はkernel所有のメモリと検証付きcopy ioctlを候補とし、ユーザー空間がVulkan command/応答を扱う。base systemは独立実装とする既存方針を維持し、上流実装を無断で取り込まない。対象subset・不足API・ownership/versionへの影響を資料へ記録する。

見積枠は240 active minutes、120分ごとに成果・境界を点検する。ユーザーは今回p003完了までの継続を指示済み。同じ失敗状態に対する無変更再試行は3回までとし、各起動・pollにtimeoutを設け、証拠に基づいて修正する。HAL責務/hal.hは変更せず、必要な判断が実際に発生した場合にのみ確認する。C規約全文、意味のある限定test、make -j16対象build、QEMU実測、差分レビューを適用する。p004・ネイティブi915は実行対象に追加しない。git add/commit/pushはユーザーが行う。

## q306 HAL変更の許可待ち（2026-09-12）

ユーザーが「HALの改変には許可が必要です」と明示した。既存宣言の実体補完も含め、HALの全変更に適用する。エージェントが責務変更を伴わないMMIO補完を許可不要と解釈したのは誤り。追加したsrc/hal/amd64/asm.cの8 accessorを取り消し、元のソースへ戻した。具体差分を `plan/ws014/phase003/amd64-mmio-proposal.patch` に保存し、適用・検証の許可を質問中。未許可の候補を用いた追加build/QEMU試験は停止し、独立したdriver/client/loopの確認を続ける。p003はin-progressのまま、clearedではない。

候補はhal.h宣言済みのMMIO read/write8/16/32/64のamd64実装のみ。hal.hや責務の変更はないが、許可は必要である。候補適用時のamd64 kernel/image linkは成功したが、実QEMUでGPU登録にまだ失敗しており、描画成功は確認していない。候補のbuild結果を受け入れ済み実装と混同しない。

前準備はKVM/ANV/QEMU環境、既存kernelの起動・QMP画面取得、TTY履歴のread-only取得まで成立した。GPU core拡張・ユーザー空間クライアント・Venus backendの限定compile/host testsが進んでいる。現行U/K契約は下記資料に記録する。イメージ転送は、ユーザーが10.0.10.25を私有サーバーとして機密データも含め明示許可済み。

## q306 HAL変更の承認・再開（2026-09-12）

ユーザーが提示済み差分に「許可します。」と回答した。`plan/ws014/phase003/amd64-mmio-proposal.patch` の8個のamd64 MMIO read/write accessorの適用・検証を許可されたため、同一差分を適用し、build/QEMU検証を再開する。hal.hやHALの責務は変更しない。直前の「HAL変更の許可待ち」は解消済み。今後の別のHAL変更には、その具体差分に対する事前許可を引き続き必要とする。

PCI BARのcapability部分だけをmapして失敗する問題をdriver側で修正し、register BARを一度だけ全体mapして各capabilityに範囲を渡す。driver単体・ASan/UBSan試験は通過済み。実際のVulkan描画は引き続き未検証で、p003/q306はin-progress。

## q306 実行診断と画面取得方式の更新（2026-09-12）

HALの提示差分はユーザー承認済み。修正後のamd64 image buildと実QEMUのGPU登録・capset4照会が成功した。q306-2d-003/004ではzedBSDの2Dクライアントが全画素/FNV検証とpresent成功マーカーまで到達したが、QMP screendumpは継続してno surfaceを返す。

QEMU v10.0.11公式実装を確認した結果、GL scanoutはSCANOUT_TEXTUREとなり、QMPが呼ぶqemu_console_surface()はNULLを返す。egl-headlessは別途pixman surfaceへ実際のGL画像をreadbackしており、VNCはそのsurfaceを参照する。このためQMPは起動制御・console/log取得を維持し、描画画像はQEMU標準のVNC Unix socket経由で取得する方式へ更新する。外部TCPポートは使わない。実画像の全ピクセル・独立期待値・試行/frame/hash照合という受け入れは維持し、表示成功マーカーだけではclearしない。QEMUやゲスト画像を改造して成功画面を作る方式ではない。

ホストにはvirgl-serverが欠落していたため、Debian公式virgl-server_1.1.0-2_amd64.debを専用rootのdependencies配下へ展開した。システムのdpkg状態は不変。RENDER_SERVER_EXEC_PATHで指定しbinary/packageのhashと起動確認を記録する。

q306-venus-001はcapset4/wire1照会後、返信blob確保付近でENOMEMとなる。Vulkanコマンドの実行成功はまだ未確認。p003/q306はin-progressのまま、driverのHOSTVISIBLE mappingと有限VNC captureを修正・検証する。

根拠: https://github.com/qemu/qemu/blob/v10.0.11/ui/console.c 、https://github.com/qemu/qemu/blob/v10.0.11/ui/ui-qmp-cmds.c 、https://github.com/qemu/qemu/blob/v10.0.11/ui/egl-headless.c 、https://github.com/qemu/qemu/blob/v10.0.11/ui/vnc.c 。

## q306完了: p003 cleared（2026-09-13 JST）

Venus専用driver、GPU任意callback/UAPI、独立したVulkan clear/copy/fence/readbackクライアント、build→転送→新規QEMU→実画面照合の有限ループを実装・検証した。2D/Vulkanともframe1とframe2の全49,152RGB pixelが赤緑／青黄の独立期待値に一致。Vulkan fence完了とGPU readback全画素も確認した。最終sourceからのbuildは実測済みkernel/clientとバイト一致する。

QEMU10のGL scanoutはQMP screendumpで取得できないため、QMPは制御とconsole取得、画面はegl-headlessのreadbackをVNC Unix RAWで取得。hostmemは現行amd64 MMIO窓に合わせ8MiB。HALはユーザーが具体差分を許可した8accessorのみ変更した。

q306はfinished、q306-i01/p003はcleared、active Queueはなし。p002 cleared、p001/p004 planning、WS014 incompleteを維持。次はユーザーが追加したp005（テクスチャ付き回転直方体デモ）、その後p004。native i915は別WS029であり今回未実行。一般のlibvulkan.so、全Vulkan適合、汎用WSI/mmap/zero-copyは未実装。

詳細と実測hashは[p003](https://github.com/awemorris/zedBSD/issues/384)。local evidenceはplan/ws014/phase003/results.md、evidence/、queue履歴はplan/history/queue-q306.md。GitHubは計画・証拠本文を同期し、source/資料のgit add/commit/pushはユーザーが行う。未コミットのファイルを公開済みリンクとして扱わない。

## q307開始: p005をp004の前へ追加（2026-09-13）

ユーザーがテクスチャ付きの回転直方体デモをuserland/base/vkdemoとして作り、vertex/fragment shaderとAPI不足を確認するよう依頼。[p005](https://github.com/awemorris/zedBSD/issues/387)を追加し、p003 cleared → p005 → p004の順とする。q307/q307-i01はp005だけを実行。p003/q306のclear/終了は維持し、p004とnative i915は未実行。

独自GLSL→SPIR-V、実texture/depth/graphics pipeline、時間の進む同一process、GPU readbackとVNC実画面、独立した幾何/texture照合で確認する。既存GPU APIを再利用し、必要なU共通化と実測された不足だけを補う。HALの追加変更は未許可。見積240 active minutes、120分ごとの点検、有限build/VM/pollを適用する。GitHub同期はユーザー明示承認済み、git add/commit/pushはユーザーが行う。

## q307完了: p005 cleared（2026-09-13 JST）

userland/base/vkdemoにテクスチャ付き回転直方体を実装。独自vertex/fragment shader、実texture、depth、Vulkan pipelineを使用する。q307-vkdemo-002で固定3時刻と実時間3枚のGPU readback/VNC hashが一致し、独立したray/texture期待値との照合も不一致0。正常終了後、同じVMで通常2秒・12frameの回転を再openしてDONE/shell復帰、QEMU exit0まで確認した。

新規ioctlは不要。Uの共通Venus clientとgraphics操作を追加し、実測で発見したKのblob unmap待機の早期timeoutを修正した。clock進行中は10秒deadlineを維持し、clock停止中だけ連続poll上限を使う。HAL追加変更なし。有限host tests、shader/CLI/画像検証、専用amd64 build、p003回帰がPASS。

q307 finished、q307-i01/p005 cleared、active Queueなし。p003/q306の完了を維持し、次はp004（planning、未実行）。p001 planning、WS014 incomplete。native i915は別WS029。汎用libvulkan/ICD・全Vulkan適合・汎用WSI/zero-copyは未実装。

実測結果とAPI表は[p005](https://github.com/awemorris/zedBSD/issues/387)。ローカルのplan/ws014/phase005/results.md、api-coverage.md、evidence/とplan/history/queue-q307.mdへ保存。GitHubは計画/結果本文を同期し、source・資料・画像のgit add/commit/pushはユーザーが行う。

## q308: 標準Vulkan 1.0・直接表示libraryとp005訂正

2026-09-13のユーザー確定指示に従い、標準Vulkan 1.0全137core＋VK_KHR_surface/display/swapchain/display_swapchainを提供する単一目標の [WS030](https://github.com/awemorris/zedBSD/issues/388) を新設した。公開headerはinclude/libc/vulkan/、独立実装はuserland/base/libvulkan/、配置は/lib/libvulkan.so。EGLは今回cancelし将来GLES-on-Vulkan時へ、Waylandは将来backendとする。上流実装は移入せず、固定した公式XMLから宣言・定数を独立生成する。

[WS014 p005](https://github.com/awemorris/zedBSD/issues/387) のq307旧clearは、直接Venus wire/GPU ioctlを使う有限clientであり「純粋な標準Vulkan APIアプリ」を満たさないため失効（uncleared）。q307の6画像・正常回収・同VM再openという実測と当時の試行履歴は保存し、新しいq308-i04で標準API化を訂正する。p005はin-progressとして再開し、Queue itemは必要library出力までpending。p002/p003のclear、WS014 incomplete、p004未実行、別WS029 i915後段を維持する。

[q308](https://github.com/awemorris/zedBSD/issues/362) の順序はWS030 p001→p002→p003→WS014 p005→WS030 p004。ユーザーは全実装・作業継続・GitHub同期を明示承認済み。見積720 active minutes、120分ごと点検、各command/VM/poll有限、無変更retry3回まで。HALの追加変更・aggregate make check・git add/commit/pushは許可しない。既存private hostへの転送許可を維持する。

全API、必須能力/limits、memory可視性、同期、FIFO/image再利用の意味論を未検証のままcompleteとしない。詳細はWS030の実装契約。依存するsource変更は計画・native lifecycle/親子依存・Projectの同期とreadback後に開始する。

## q308完了: 標準Vulkan・直接表示libraryと標準APIデモ（2026-09-13）

WS030 p001/p002/p003/p004とWS014 p005の標準API訂正をclearedとし、WS030 completed、q308 finished、active Queueなしとする。WS014はincomplete、p001/p004 planning、p004未queue、native i915は別WS029のまま。q307の旧scopeの実測と履歴は保持する。

`include/libc/vulkan/` にVulkan1.0の公開header、`userland/base/libvulkan/` に独立した全137 core＋選択direct-display WSI18の実装を提供し、`/lib/libvulkan.so` に配置した。vkdemoは標準Vulkan/WSIだけを使い、GPU ioctl/Venus codecをアプリへ持ち込まない。ABI、Noct再生成、155実exportとproc-address、全familyの限定意味論試験、U/Kの所有権・権限・失敗回収、適用C規約の独立レビューを実施した。正式CTS認証は主張しない。

最終 `q308-lifecycle-003` は実QEMU10.0.11/virglrenderer1.1.0/Intel ANVで6枚の回転直方体を描画し、実VNC/GPU readback/独立ray-texture oracleが一致（評価対象不一致0）。通常終了後6frame再起動、SIGINT後6frame再起動、640×480文字画面への復帰とechoによる画面更新、別processの表示競合拒否とowner35frame/DONEを確認した。42.671秒、QEMU exit0。最終書式変更後のkernel/appは実行済みbinaryと一致する。

承認済みHAL patch SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d` のみを適用し、既存hal_space_map_device/device usermapを補完した。追加HAL APIはない。PCI cache属性、queue総数63、allocator破棄、console/query/通知の修正と、先行失敗・再実行理由を保存した。公開coherent HOST_VISIBLE、256MiB aperture、native watchdog等の制約は能力監査へ記録した。

結果は `plan/ws030/results-q308.md`、155行の台帳は `plan/ws030/phase004/api-verification.md`、最終証拠は `plan/ws030/phase004/final-evidence/verification.json`、p005訂正は `plan/ws014/phase005/results-q308.md`、履歴は `plan/history/queue-q308.md`（いずれもlocal/uncommitted）。GitHubは計画Issue/Project/結果コメントの同期であり、source/doc/imageのgit add/commit/pushはユーザーが行う。EGLは今回cancel、Waylandは将来VK_KHR_wayland_surface backendとして追加する。

## WS014 p006追加: kernel handle・GPU共有・最小Wayland（2026-09-13）

ユーザー指定により[WS014 p006](https://github.com/awemorris/zedBSD/issues/393)を一つのplanned Phaseとして追加した。kernel_handle/handle_fd_*とSCM_RIGHTS、GPU/Venusの別context共有、GPU画像を扱えるWSI、VK_KHR_wayland_surface、最小client library、全画面zwl、標準APIのwltestを本Phaseで実装・検証する計画。コード配置はinclude/libc/wayland/、userland/base/libwayland/・zwl/・wltest/、公開libraryは/lib/libwayland-client.so。

中核のK/driver実装を先に進め、Wayland通信/WSI/試験アプリを接続して実測から設計を改善する。新経路はCPU readbackを必須にせず、GPU allocationの実共有と同期・寿命を確認する。linux-dmabuf-v1、ゲストdma-buf/DRM、EGL、一般DEは採用しない。内部の段取りは別Phaseへ分割しない。

順序はp005 cleared → p006 planned → p004 planning。p004はp006の最終ソース/API/検証を受けて規約確認する。WS030 completedとq308 finished、既存Phaseのclearを維持。今回作成したのは計画であり、active Queue・新しい実装/試験結果はない。HALの追加差分は従来どおり個別承認、git add/commit/pushはユーザー担当。

## q309開始: WS014 p006を単一項目で実行（2026-09-13）

ユーザーの「では、実行してください。」により、[p006](https://github.com/awemorris/zedBSD/issues/393)全体をq309-i01として実行する。kernel handle/fd/SCM_RIGHTS、GPU/Venusの別context allocation共有・GPU内表示、VK_KHR_wayland_surface、最小libwayland-client.so・zwl・wltest、限定検証と実QEMU受入を一つのPhase/項目に含める。中核K/driverを先に実装し、通信/WSI/アプリを接続して実測から改善する。p004は含めない。

時間枠は720 active minutes見積、120分ごとに進捗・残件を確認。各command/VMを有限化し、同条件無変更retryは3回まで。達成の保証や無限継続ではなく、未達は証拠と再開条件を残す。全規約を適用し、既存成果/履歴を保持する。HAL追加変更とgit add/commit/pushは許可されたとは解釈しない。private host・image/source転送の承認を維持する。

q309はactive、q309-i01とp006はin-progress、WS014はincomplete。q308 finished・WS030 completed・既存clearanceは維持。新経路でCPU readbackを必須にせず、実GPU allocation共有と同期/寿命/Wayland protocolを確認する。実装成功・Phase受入はまだ記録していない。

## q309 checkpoint001: GPU画像共有とWayland実表示（2026-09-13）

ユーザーの指摘に沿って表示経路を確認した。従来のCPU readback経路もGOPへは書かず、virtio-gpuの2D resource/SET_SCANOUTへ送っていた。p006では共有GPU allocationをSET_SCANOUT_BLOBへ渡し、通常のWayland表示にCPU readback/再uploadを必須としない。表示数・接続・推奨サイズはGET_DISPLAY_INFO、追加モード・nominal refreshは今回追加したGET_EDIDのbase/CTA progressive DTDから取得する。無効/未対応EDID時は既存50Hz、100Hz超・standard/established timing・DisplayID・物理vblank保証は対象外。

K handle/fd/SCM_RIGHTS、GPU共有、最小libwayland-client.so、VK_KHR_wayland_surface、zwl、標準Wayland/Vulkanアプリwltestを実装した。K参照・copyout rollback、GPU export/import/scanout、Wayland byte/fd FIFO・queue・frame/release・swapchainの限定試験は通常とASan/UBSanで通過。公開ABIはi386/amd64 C/C++で固定公式Wayland/Vulkan headerに一致、Vulkan137 core＋20 WSIの157 dispatch/exportを照合した。正式CTSや全Wayland SDK互換は主張しない。

実QEMU10.0.11/virglrenderer1.1.0/Intel ANVのq309-wayland-001は21.257秒でPASS。生成元プロセス終了後、独立receiver contextでimportしたGPU画像をGPU copyし、検証専用readbackの1024画素が一致。wltest→実SCM_RIGHTS→別processのzwl→scanoutではFIFO/MAILBOX各6枚、計12枚の320×240実VNC画像が独立oracleと全画素一致した。各modeのswapchain再作成と通常終了、zwl12frame/cleanup_failed=0も確認した。これは初回実測で、最終ソースの受入ではない。

検証後、WSI破棄によるアプリ所有surfaceの暗黙unmapを除き、zwlの明示unmap/remapを整理。共有extentの照会範囲とexport上限を一致させた。クライアント/コンポジタ異常終了・再起動、可視console復帰、最終限定回帰/build/規約確認を継続中。p006とq309-i01はin-progress、q309 active、WS014 incomplete。p004未queue、WS030 completedと既存clearanceを維持。HAL追加変更なし、git add/commit/pushはユーザー担当。

local/uncommitted証拠: plan/ws014/phase006/checkpoint001.json、plan/ws014/temp/remote/q309-wayland-001/result.json とevidence/。source/doc/imageはGitHub repository未公開であり、本同期はIssue/Projectの計画と結果記録。

## q309完了: GPU handle共有・Wayland WSI・virtio scanout（2026-09-13）

WS014 p006 / q309-i01をcleared、q309をfinishedとする。active Queueなし。WS014はincomplete、p001/p004 planning、p004未queue。WS030 completedと既存Phaseのclearanceを維持し、native i915は別WS029のまま。

kernel_handle/handle_fd_*と共通fd参照・SCM_RIGHTS、GPU/Venusの独立process/context共有、VK_KHR_wayland_surface、最小libwayland-client.so、全画面zwl、標準Wayland/Vulkanアプリwltestを実装した。共有GPU imageはGPU copyと所有権同期を経て別processへ渡り、SET_SCANOUT_BLOB/RESOURCE_FLUSHで表示する。通常の新WSI経路にCPU readbackや再uploadを必須としない。旧copy経路もGOPではなくvirtio 2D scanoutであり、直接表示の互換経路として保持する。

GET_DISPLAY_INFOとGET_EDIDのbase/CTA progressive DTDで表示・モードを列挙。実QEMUでは1280×800、74,994mHz、320×200mmを取得した。custom framebuffer寸法はEDID寸法と独立に扱い、1〜100Hzのguest nominal pacingを検証する。virtioは物理pixel clockを設定しないため、物理vblank同期の保証とは区別する。

最終q309-wayland-004は43.869秒、QEMU exit0でPASS。FIFO/MAILBOX各6枚の320×240実VNC画像、計921,600画素が独立期待値と全画素一致し、12回の表示が実import資源とBLOB scanoutに対応した。生成元終了後の独立renderer import/GPU copyも検証専用readbackの1024画素が一致。swapchain再作成、client中断・再open、compositor通常終了・SIGKILL・再起動、実SURFACE_LOST/cleanup=0、強制終了直後の640×480 console復帰とechoによる画面更新を確認した。

同じ最終kernelのq309-direct-002も42.705秒、QEMU exit0でPASS。標準vkdemoの回転直方体6枚をGPU readback/VNC/独立oracleで照合し、通常終了とSIGINT後の再open、console復帰、表示競合拒否とowner完走を確認した。K/fd/SCM・GPU/EDID・Wayland/WSIの実コード限定fixtureとsanitizer、157 Vulkan dispatch/export、両ABIの公式header照合、Noct再生成、rootfs配置、対象buildと全適用規約を確認した。正式CTSや全Wayland SDK互換は主張しない。

途中のharness起動待ち不足とEDID/custom mode回帰を修正し、失敗証拠と再実行理由を保存した。最終reviewのMSG_PEEK二重put疑義は、rights付きpeekを既存guardが拒否するため到達不能と確認し、本体変更を戻して拒否後の参照寿命を追加検証した。formatterは規約と設定の不一致によりexit1でありPASSとは扱わず、全文確認とdiffcheckを記録した。HAL追加変更なし。ローカル結果はplan/ws014/phase006/results.md、技術資料3件、conformance.mdとfinal-evidence/verification.json、Queue履歴はplan/history/queue-q309.md。これらsource/doc/imageのgit add/commit/pushはユーザーが行う。本同期はGitHub Issues/Projectの計画・受入結果である。

## q310開始: GPUレビュー改善p007（2026-09-13）

ユーザーの新Phase作成・実行指示により、[WS014 p007](https://github.com/awemorris/zedBSD/issues/394)全体を単一項目q310-i01として実行する。直接VK_KHR_displayにもGPU copy/blit→共有linear画像→BLOB scanoutを使い、通常vkdemoのCPU readbackを除く。完了通知・command batch/reply mmap・controller排他の短縮・非同期present・同一open並行性・安全なtransport回復・buffer/optimal allocation共有を改善し、実QEMUで描画/寿命と転送数を検証する。

ユーザーはzwlの1パス1surface同期presentとlibwaylandの限定protocolをテストドライバとして承認した。一般Wayland環境、複数window合成・入力・既存Toolkit対応・常駐化は本Phaseへ入れない。reviewの推奨は仕様と照合し、送信受理、Venus decoder応答、VkFence完了、scanoutを区別する。以前のp006でGPU内表示を実証したのはWayland経路であり、直接表示にCPU経路が残った対応不足を訂正する。

p006/q309は実際の受入範囲のcleared/finishedと証拠を保持する。順序はp006 cleared → p007 in-progress → p004 planning/未queue → WS029 native i915。WS030 completed、p001の未決定と既存clearanceは維持。q310は720 active minutes見積/120分レビューの有限項目。HAL追加変更、VFIO/ホスト表示停止、git add/commit/pushは含めず、private image/source転送とGitHub計画同期の既存承認を使用する。開始時点では新実装・試験の成功を主張しない。

## q310追補: 標準external memory/fence fdと描画・表示の組（2026-09-13）

ユーザーの追加review2 B/C/E/F/Gを[WS014 p007](https://github.com/awemorris/zedBSD/issues/394)へ追加し、q310-i01の同じ目標で実行する。KERNEL_HANDLE_FENCE/POLLIN・command/present wait/signal、VK_KHR_external_fence_fdとVK_KHR_external_memory_fdのOPAQUE_FD、必要な標準問い合わせ/拡張依存、libvulkanでの描画nodeと表示nodeの組、driverによるscanout import可否/制約照会、swapchain作成時の一度だけの共有またはCPU fallback判断を含める。

OPAQUE_FDはLinux dma-buf/SYNC_FDを要求しないが、同じdeviceUUID/driverUUIDの互換条件を守る。別描画GPUからの無条件importは前提にしない。表示専用foreign importはdriverが実backingを検証した場合のみで、未対応は拒否する。通常VenusのBLOB直接表示を受入条件として維持する。別node/制約/fallbackは実コードfixture、標準共有・描画・寿命は実Venusで検証する。

zwlの同期1surface/passとlibwayland限定protocolはユーザー承認のテストドライバ範囲として維持。一般Wayland/Toolkit、external_semaphore_fdやdisplay_controlの全API、native i915、新HAL/物理foreign-DMA受入、git add/commit/pushは自動追加しない。720 active minutes見積/120分レビューを維持。新規機能の実装・受入成功はまだ記録していない。p004はこの追加を含む最終APIを後続で確認し、未queueのまま。

## q310 checkpoint 01: 実QEMUで並行操作・標準fd共有を確認（2026-09-13）

[WS014 p007](https://github.com/awemorris/zedBSD/issues/394) / q310-i01 は実装・検証を継続中。p007は未完了であり、p004/native i915には進んでいない。

- 実QEMU `q310-wayland-005` で同一GPU fdの4 pthread×32回（128 allocation lifecycle）の書込/読出全4096 byte照合がPASS。
- 別processの標準Vulkan OPAQUE_FD fenceのexport/import/reset・実GPU signal・参照寿命がPASS。linear共有と標準buffer OPAQUE_FDのproducer終了後import/GPU copy/pixel検証もPASS。
- 同じ実行はoptimal image capability queryでVK_ERROR_FORMAT_NOT_SUPPORTEDとなりFAIL。対応が必要なnative dedicated allocation条件を確認し、必要なVK_KHR_get_memory_requirements2 / VK_KHR_dedicated_allocationを標準APIで補う。対応型の能力を偽って成功扱いにしない。Vulkan coreは1.0を維持する。
- 限定fixtureの通常版/ASan・UBSanではtyped fence/poll/SCM_RIGHTS、signal fd close/reuse競合、K allocation rollback・元所有者終了後の保持、display-only foreign backingのDMA/cache拒否と最後の解放、非同期presentの所有権、作成時に一度だけのfallback選択、command batching、mapped reply、IRQ out-of-order/制御用slot確保を確認した。fixtureと実GPUの証拠は区別する。
- 実試験で見つかったQUERY出力欄をRESET/WAIT入力へ再利用する不具合を修正。次に露呈したAMD64 pthread初期SPのC ABI不一致はlibcで修正し、005で例外が消えた。HAL変更は行っていない。
- 失敗履歴001（旧診断不足）、002（harnessで未対応の`;`を入力）、003（RESET EINVAL）、004（workerのmovapsでstack alignment例外）、005（上記optimal profile拒否）を保存。失敗を過去の成功へ書き換えない。

残件はoptimal allocationの標準API受入、direct BLOB/Waylandの画面・寿命・性能検証、故障/回復の有限試験、最終API/規約/宣言再生成確認。zwlとlibwaylandの承認済みテストドライバ制約は維持する。資料・コードは作業treeにありgit add/commit/pushはユーザー担当。GitHub Issues/Projectの同期とrepository公開を混同しない。

ローカル証拠: `plan/ws014/temp/remote/q310-wayland-001`〜`005`（実行ごとのsource/image hash、build/transfer/guest/renderer log）。UAPI対応表は `plan/ws014/phase007/gpu-uapi-contract.md`。同表は下記Phase本文にも掲載する。720 active minutes見積・120分レビューを維持し、今回のcheckpoint後もq310を継続する。

q310 checkpoint 02: paired OPAQUE rendererでstandard buffer/optimal/fence、Wayland12画面、direct6画面と通常readback0、producer終了error、10000ms timeout後のchecked recoveryを実QEMUでPASS。最後のtopology POLLPRI/ACK通知と統合確認を継続中。詳細は[WS014 p007](https://github.com/awemorris/zedBSD/issues/394)。

## q310完了: BLOB直接表示・標準fd共有・GPU同期改善（2026-09-13）

WS014 p007 / q310-i01をcleared、q310をfinishedとする。active Queueなし。WS014はincompleteでp001/p004はplanning、p004とWS029 native i915は未queue。p006/q309・WS030の既存clearanceを維持する。

直接VK_KHR_displayもGPU copy/blit→共有linear画像→SET_SCANOUT_BLOBへ移行し、通常vkdemoのCPU画像readback/uploadを除いた。IRQ完了通知、vkCmd batchingとmapped reply、表示待ち中のcontroller排他短縮、所有jobによる非同期present、同一openの待機admission、安全なtransport回復を実装した。

review2 B/C/E/F/Gも反映: KERNEL_HANDLE_FENCE/POLLINとcommand/present wait/signal、標準VK_KHR_external_memory_fdとexternal_fence_fdのOPAQUE_FD、描画node＋表示nodeの組、driverによるscanout import判定とswapchain作成時に一度だけの経路選択。GPU_DISPLAY_EVENTS/POLLPRIのQUERY→列挙→exact ACKと、GPU_BLOB_CREATE_PLACEDによる物理配置要求を追加した。GPU ABI v1の旧要求layoutを保ち、内部drv_gpu_opsはv6。実backingで満たせない配置はENOTSUP、OOMやdevice lossはfallbackで隠さない。

OPAQUE_FDはguestにdma-buf/SYNC_FDを要求しない。標準memory共有は同じdeviceUUID/driverUUIDの互換範囲、別GPUの任意importは未対応。表示専用foreign import・DMA/cache/placement・別node組合せは実コードfixtureによる検証であり、異種実機DMAや新HAL allocatorの受入ではない。Venusは非零physical placementを拒否する。

| 最終実QEMU | 結果 |
| --- | --- |
| q310-wayland-008 | PASS、46.531秒、QEMU exit0。128回同一fd並行操作、標準fence・linear/buffer/optimal共有、初期topology QUERY/ACK、FIFO/MAILBOX各6画面・異常終了/reopen/console |
| q310-direct-004 | PASS、41.83秒、QEMU exit0。直接表示6画面の独立oracle、通常readback0の動く2実画像、QEMU BLOB trace/対象寸法のlegacy経路なし、SIGINT/reopen/lease競合/console |
| q310-fence-exit-003 | PASS、4.424秒、QEMU exit0。pending producer終了後DEVICE_LOST、独立30秒fault期限と実測、最終waitpid |
| q310-recovery-004 | PASS、13.807秒、QEMU exit0。所有rendererだけを停止、10000ms timeoutとpeer error/旧参照gate、再開後checked resetと新contextの4096byte/decoder |

最終buildと限定K/U/driver/WSI fixtureの通常・ASan/UBSan、170公開APIのdispatch/両ABI、Noct生成8file一致を確認した。通常デモは約2秒で13frame程度という実測を残し、速度倍率や物理vblank保証・CTS適合を主張しない。zwlの同期1surface/passとlibwayland限定protocolはユーザー承認のテストドライバ制約として維持する。

optimal共有はstock virglrenderer1.1.0 proxyのOPAQUE attach不足を補ったisolated paired library/serverで受入した。exact168B capsetとINIT handshakeで合意した場合だけ選択し、private WSIのnative DMA経路を保持。system library/packageは変更していない。patch/library/server hashと手順はlocal/uncommitted plan/ws014/phase007/renderer-opaque/に保存した。stockだけでoptimal共有が通るとは扱わない。

失敗履歴wayland001–005、direct001、recovery001、fence-exit002を保存。fence-exit002はconsumer/driver双方10秒の期限競合でVK_TIMEOUTが先行した。fault専用期限を30秒に分け、final closeはpending fenceをerrorへしてからcallback drainを待つ順序へ改善した。修正前FAIL・修正後normal/sanitizer PASSの因果fixtureも保存し、実行中ioctl/file参照によるfinal close入口までの遅延とは区別する。

資料はlocal/uncommitted plan/ws014/phase007/results.md、gpu-uapi-contract.md、display-wsi.md、transport-sync.md、各verification JSONとQueue履歴plan/history/queue-q310.md。新HAL変更・GDM/VFIO操作・git add/commit/pushなし。GitHub Issues/Projectへの計画/受入同期と、ユーザー担当のrepository公開を区別する。

## q311開始: GPU完了責任・fence所属と描画資源の改善（2026-09-13）

ユーザーがレビュー回答を承認し、独立Phaseの作成・実行を指示した。[WS014 p008](https://github.com/awemorris/zedBSD/issues/395)を単一項目q311-i01で実行する。p007/q310のcleared/finishedを保持し、p008 → p004 planning/未queue → 別WS029 native i915の順とする。

A1の局所表示エラー分離、A3のGPUドライバによるfence終端、A4のqueue容量、A5のacquire待機、A6の同時進行slot別pool/cb再利用、A7のexternal worker撤去・console通知、A2/A8の検証補強を含む。fenceはdrv_gpuフレームワークへ移し、kernには不透明handle/fd/refcount/poll/SCM_RIGHTSを残す。共通DRIVER分類＋ops識別とGPU組込時だけのbuildを用いる。

isolated paired rendererのSTRICT_QUEUE能力を合意し、実GPU成功だけを正常retireへ流す。失敗はsticky化して後続まとめretireを抑止し、K watchdogでERROR終端する。native投入前の予約も期限管理し、U停止による未監督仕事を残さない。通常BLOB表示と標準OPAQUE_FDを維持する。

720 active minutes見積・120分レビュー、有限fixture/build/VMで完了まで進める。HAL追加変更・一般Wayland・native i915・git add/commit/pushは含めない。既存private host/転送とGitHub同期の承認を使用する。開始時点で新実装や検証成功は主張しない。詳細はp008本文と承認回答コメント、local plan/ws014/phase008/に残す。


## q311完了: GPU完了責任・driver fence・描画資源改善（2026-09-13）

WS014 p008 / q311-i01をcleared、q311をfinishedとする。active Queueなし。p007/q310の受入を保持し、WS014はincomplete、p001/p004はplanning、p004と別WS029 native i915は未queueのまま。

承認回答A1–A8とfence所属を実装した。fenceはdrv_gpuフレームワークへ移し、kernは不透明handle/fd/refcount/poll/SCM_RIGHTSを保持する。6platformでGPU共通層＋fenceをbackend選択時だけbuildし、GPUなしamd64実ELFでGPU symbol/object不在とgeneric handle/fd残存を確認した。

native投稿前のGPU_JOB予約から独立watchdogが監督し、strict paired rendererの実submission VkFence成功でKがexact generationを終端する。U-only/未commitのpendingをK内部で無期限に待たない。slotは最大64descriptor/32chain（28job＋4control）、外部fenceごとのworkerを撤去。acquireはmonotonic condition、present pool/cbは同時slotごとに再利用し、consoleは文字・所有権変更で起床する。局所表示エラーと全device故障も分離した。

最終5VMは同じkernel/base imageでPASS/QEMU exit0: direct-002 41.345秒、wayland-002 46.011秒、producer-stop-004 14.117秒、producer-exit-002 4.214秒、recovery-002 13.823秒。直接/Waylandの通常BLOB表示・独立画像oracle・複数process・再open/consoleを確認。SIGSTOP中fdを開いたproducerは9970msでDEVICE_LOST、renderer停止は10000msで故障通知後にchecked reset・新context往復を確認した。

K/U/transport/host/consoleの限定normal・sanitizer、170API/両ABI/Noct8file、対象build・規約・独立レビューを完了。U回収競合2件は修正前FAIL→修正後PASS。static analyzerの4警告は実callee/有効入力の前提と照合して記録し、全警告0とは扱わない。初期のbuild/harness失敗も保持する。

新libvulkanのVkDevice作成にはSTRICT_QUEUE対応のisolated paired rendererが必要。stock/旧pairは初期化で拒否する。host system packageとHALの追加変更なし。通常2秒sceneはp007再測定13frameからp00815frameだが、QEMU CPU時間は0.36秒から0.48秒の単発観測で、CPU削減や速度倍率は主張しない。一般Wayland/Toolkit、任意GPU間DMA、native i915、CTSは未受入。source/doc/patchのgit add/commit/pushはユーザー担当。


受入記録: [p008結果コメント](https://github.com/awemorris/zedBSD/issues/395#issuecomment-5652374702)。local/uncommittedの資料は plan/ws014/phase008/、Queue履歴は plan/history/queue-q311.md。

## review4への対応案（2026-09-13）

[レビュー回答と提案の全文](https://github.com/awemorris/zedBSD/issues/395#issuecomment-5652742665)を記録した。照合対象はユーザーcommit `cca12445`。R1は一時的容量不足とOOMを分け、Uの完了回収を止めない二段階admission＋device全体の容量通知を提案する。R3は予約/control/GPU実行の期限分離と、停止確認・DMA/descriptor退役を含むcontext単位の障害処理を提案する。R4はdirect acquireの通知接続、R5はprivate fence一括resetを専用負荷で測定、R6は内部寿命待ちとvalid usageを区別する。

R2のstock互換は能力別に実証してから有効化し、当面strictを維持する案。Vulkanはdevice loss時のfence SUCCESSを許すが、stockの非TIMEOUT retire/切断をそのまま資源再利用の証明にはしない。独自rendererの配布負担を記録した。

これは対応方針の提案であり、新実装・新試験の受入ではない。p008 cleared / q311 finished、WS014 incomplete、p004 planning/未queueを維持する。新Phase・Queueの作成と実行は行わず、既存BLOB表示・GPU framework所属fence・テストドライバ範囲を維持する。回答書はローカル `plan/ws014/gpu-stack-review4-response.md`（今回未commit）。git add/commit/pushはユーザー担当。

## q312開始: GPUレビュー対応とフレームワーク共通化（2026-09-13）

ユーザー指示により[WS014 p009](https://github.com/awemorris/zedBSD/issues/396)をq312-i01の単一Phaseとして実行する。承認範囲は[review4回答](https://github.com/awemorris/zedBSD/issues/395#issuecomment-5652742665)とGPU共通化の協議。job/fenceの状態・容量待機・期限・session故障と参照保持をdrv_gpuへ寄せ、backendは実資源の予約・投稿・完了と停止/DMA退役確認を担う。R1のU排他と容量通知、R3の期限/障害範囲、R4のdirectAcquire通知、R5のprivate fence reset再利用・測定、R6の寿命を改善する。

R2はstrictを当面維持し、stock互換の能力と退役条件を限定検証する。安全性が成立しなければstrictと具体的な不足・制約を記録する。context停止も能力と実確認が前提で、停止不能時はquarantine/全体resetを維持する。通常BLOB表示・GPU内共有・標準APIとzwl/libwaylandのテストドライバ範囲を保持する。

p008/q311のcleared/finishedを保持し、順序はp009 → p004 planning/未queue → 別WS029。720 active minutes見積・120分レビュー、有限fixture/build/VMで実装・受入する。追加HALや一般DE/native i915、git add/commit/push、system package/GDM/VFIO変更は含めない。private host/転送・隔離依存build・GitHub同期は既存承認を使用する。開始時点では新実装・試験の成功は主張しない。


## q312完了: GPUレビュー対応とフレームワーク共通化（2026-09-13）

WS014 p009 / q312-i01をcleared、q312をfinishedとする。active Queueなし。p008/q311の受入を保持し、WS014はincomplete、p001/p004はplanning、p004と別WS029 native i915は未queueのまま。

R1: 容量不足をOOMにせず、`GPU_JOB_CAPACITY` QUERY/WAIT（ioctl 37）と`GPU_JOB_POLICY`（38）を追加。libvulkanはnative準備→QUERY→回収→非待機RESERVEとし、EAGAINではqueue/device/context mutexを外して待つ。R3: 予約10秒・実行60秒・停止10秒をmake/menuconfigの設定と実効値照会にし、session単位のsticky errorと`drv_gpu_recovery_ops`（stop_begin/stop_poll/fault/reset）でcontext単位の停止確認を導入。Venusはflags7のquiescence契約（全native VkDeviceWaitIdleを確認したCPU0 ACK）を持つisolated pairで実停止を証明し、確認不能なら従来のquarantine/全体resetへ進む。R4: direct acquireは画像返却・故障・topologyをwaiter固有pipeと`ppoll`で待ち、10 ms周期起床を除いた。R5: terminal private fenceを最大64本ずつ一括resetしREADYを再利用。R2はstrict（flags7）維持、stock 1.1.0の情報欠落をstock-compat/で記録。fenceとjob監督はdrv_gpu内に保持し、汎用kernへの追加なし。

最終8VMは同一最終artifactでPASS/QEMU exit0: direct-003 41.935秒、wayland-002 47.342秒、submit-load-005 11.833秒（2process 576 submit、OOM 0）、completion-delay-003 25.316秒（15秒遅延完了、peer継続）、context-timeout-003 25.138秒（短縮期限でDEVICE_LOST、peer継続）、producer-stop-002 19.438秒（SIGSTOP中に7770 msで終端）、producer-exit-002 4.165秒、recovery-002 14.051秒（10000 ms watchdog後checked reset）。限定fixture（K 12 suite、U 10+5 job、transport/host/console）、170 API/両ABI/Noct、6platform×GPU有無のbuild入力、GPUなしamd64実ELF、規約確認を完了。失敗履歴（submit-load-001の能力bit漏れ、producer-stop-001の旧期待値、recovery-001のerrno期待値）を保持し、初回成功とは扱わない。

新libvulkanのVkDevice作成にはflags7（OPAQUE+STRICT+QUIESCE）のisolated paired rendererが必要で、stock/旧pairは初期化で拒否する。実行期限60秒は正当な長時間computeにも適用される。任意GPU間DMA、native i915、一般Wayland/toolkit、CTSは未受入。HAL・host system package・git add/commit/pushは行っていない。

受入記録: local `plan/ws014/phase009/results.md`、`runtime-verification/summary.json`。受入記録: [p009結果コメント](https://github.com/awemorris/zedBSD/issues/396#issuecomment-5655170913)。

## q313開始: GPU監督の共通化仕上げと局所隔離（2026-09-14）

ユーザー指示により[WS014 p010](https://github.com/awemorris/zedBSD/issues/397)をq313-i01の単一Phaseとして実行する。前提はp009の自己レビュー（plan/ws014/gpu-stack-review5.md、SHA256 `05435c48bdfd9168184fc026a1a628ba5d392c91522fa0556b9ed662fa2a200c`）と、その後のframework側実装可否・Venusから移せる処理の回答。S1 停止期限の起点をstop_begin実呼出しへ（D1/B3）、S2 close時のcommit済みjob監督継続（D2）、S3 停止shortcut・fault cancel後のsession失敗・RESERVED回収・停止flag・control期限定数のframework移管、S4 monitor起床の限定とrecovery_ready除去（D4/B6）、S5 停止未確認contextのsession隔離とidle時reset回収（D3/B4/B5）の順に、各段階を限定fixtureで固定してから進める。

既存UAPIのlayout/ioctl番号/sizeは変えず、内部opsは版9へ進める。実QEMUは既存7件の回帰に加え、producer-exit-delayed（既定policyで15秒jobを持つproducer終了後にconsumer fenceが成功）とproducer-exit-hang（event待ちjobで実行期限DEVICE_LOST、他sessionの継続、idle時のreset回収）を新設し、producer-exitは実行期限ERRORへ期待値を更新する。

p009/q312のcleared/finishedを保持し、順序はp010 → p004 planning/未queue → 別WS029。720 active minutes見積・120分レビュー、fixture120秒/build・転送1200秒/VM180秒（hang系300秒）で有限化。追加HAL、stock互換、一般DE/native i915、git add/commit/push、system package/GDM/VFIO変更は含めない。private host/転送・隔離依存build・GitHub同期は既存承認を使用する。開始時点では新実装・試験の成功は主張しない。GitHub Issues/Projectへのq312完了とq313開始の公開は、このsessionでは自動承認レビューにより保留され、outbox/draftsに記録した。

## q313完了: GPU監督の共通化仕上げと局所隔離（2026-09-14）

WS014 p010 / q313-i01をcleared、q313をfinishedとする。active Queueなし。p009/q312の受入を保持し、WS014はincomplete、p001/p004はplanning、p004と別WS029 native i915は未queueのまま。

S1: 停止期限の起点を`stop_begin`実呼出しへ移し、実行期限内のjobが残る間は停止しない。S2: graceful closeはcommit済みjobを終端せず実結果をfenceへ公開する。S3: native仕事の有無判定、fault cancel後のsession失敗、RESERVED回収、admission拒否、control期限定数を共通層へ移した。S4/B6: monitor起床の限定と`drv_gpu_recovery_ready`除去。S5: `recovery->isolate`（ops版9）で停止未確認contextをsession隔離し、device全体は継続、idle時のchecked resetで回収。libvulkanは自contextのPOLLERRだけでdevice lossをlatchする。UAPI/HALは不変。

最終10VMは同一最終sourceの2 build（既定policy・短縮policy）でPASS/QEMU exit0: exit-delayed-003 19.698秒（producer終了後にconsumer fenceが14750 msでSUCCESS）、exit-hang-006 28.178秒（8000 msでDEVICE_LOST、context隔離、peer継続、idle openでreset回収と通常試験PASS）、producer-exit-002 24.447秒（hostの実結果を公開）、direct-002 41.796秒、wayland-002 47.258秒、submit-load-002 10.886秒、completion-delay-002 25.213秒、context-timeout-002 25.296秒、producer-stop-002 19.341秒、recovery-002 13.926秒。限定fixture（GPU core 10種、Venus 5種、libvulkan 5種、build selection）を通常＋sanitizerでPASS。失敗履歴（exit-hang-001のU側latch、exit-hang-004のreset中open拒否、producer-exit-001の旧期待値）を保持し、初回成功とは扱わない。

隔離で失った容量はidle時のresetまで戻らず自動escalationは無い。隔離contextの表示状態はresetまで残る。closeはcommit済みjobの退役まで待つ。git add/commit/pushはユーザー担当。受入記録: [p010結果コメント](https://github.com/awemorris/zedBSD/issues/397#issuecomment-5655171051)。

受入記録: local `plan/ws014/phase010/results.md`、`runtime-verification/summary.json`、`gpu-supervision-contract.md`。
