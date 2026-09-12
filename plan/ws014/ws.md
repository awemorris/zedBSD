<!-- awesome-plan project=zedbsd record=ws014 -->

<!-- awesome-plan-current:start -->
Status: incomplete
Implementation Queue: none
Last verified Phases: ws014-p002 / p003 / p005 corrected standard API
Last Queue: q308-i04 cleared
Next: p004 planning, not queued; p001 unresolved decisions retained
<!-- awesome-plan-current:end -->

# WS014: virtio-gpu bring-up

WSID: `ws014`
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2, O4
Parent: [Master](https://github.com/awemorris/zedBSD/issues/1)
Last verified Phase: ws014-p005 (q308-i04, corrected standard API)

## 単一の目標と完了条件

QEMUのvirtio-gpu上でzedBSDのGPU/表示経路を成立させ、宣言した描画・表示APIから描画内容を画面にpresentできること。初期環境はamd64 QEMUを候補とし、p001でdevice/backend/versionと受け入れ条件を固定する。Vulkan公開API案の可否・対象profileは未確定であり、2D出力だけをVulkan対応完了と呼ばない。

## 範囲と段階

1. p001で層分け、Vulkan表示API案、最小OS ABI、QEMU環境と有限実装Phaseの必要判断を供給する。
2. virtio PCI/virtqueueと2D resource/scanout/transfer/flushによる画面更新、boot framebufferからの安全な切替を先に検証する。
3. 選定したVulkan実装とtransport/WSIを接続し、宣言したAPIで描画・presentする。画面列挙・mode選択・同期と資源回収を確認する。
4. console/graphics fallbackと権限分離を含む結合確認、変更ソースに対する適用規約全文確認を終盤Phaseへ含める。

i915実機対応、GLES2実装、デスクトップ全体の移植はこの単一目標に混ぜず、後続で選択された場合に別WSで扱う。旧案の内容と判断履歴は下に保持する。既存WS004/WS007等の実装責任はこの計画更新だけで移管しない。

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| ws014-p001 | [設計判断](https://github.com/awemorris/zedBSD/issues/213) | planning | interface/PCI/所有権の必要判断を供給。未決定を自動clearしない |
| ws014-p002 | [ws014-p002](https://github.com/awemorris/zedBSD/issues/383) | cleared | frameworkのみ |
| ws014-p003 | [ws014-p003](https://github.com/awemorris/zedBSD/issues/384) | cleared | QEMU＋VenusループとAPI改善 |
| ws014-p005 | [標準APIの3D shader/API検証](https://github.com/awemorris/zedBSD/issues/387) | cleared | WS030標準library＋q308実測で訂正完了。p004へ引き渡し |
| ws014-p004 | [ws014-p004](https://github.com/awemorris/zedBSD/issues/385) | planning | 最終API整理・規約全文確認 |

## 制約・再開点

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの適用規約に従う。HAL責務変更は既存の承認条件を守る。最初に現行PCI/DMA/interrupt/console/graphics基盤を確認し、リファクタリング前のコード配置を仮定しない。実装前に有限Queueの承認が必要。

q308でp005を標準Vulkan APIへ訂正し、3D shader/texture/depth、回転、正常・異常終了後の再open、console/所有権を実測確認した。p004の最終framework/API整理は別の未queue作業。p001の未決定を保持する。

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

<!-- traceability:start -->

## Goal traceability

- Primary Milestone: **MG006 — グラフィカルな操作環境を利用できる**
- Related Milestones: MG003
- Objectives: O2, O4
- 貢献する成果: ネイティブGPUを支える。手動保留を維持。
- 上位定義: [MasterのObjectives / Milestone Goals](https://github.com/awemorris/zedBSD/issues/1)

既存Phaseは本WSを親として上位成果に接続する。Primaryは分類と責任の所在であり、
各PhaseがRelatedすべてを満たすという意味ではない。成果・検証・限界は各Phaseの
現行記録を根拠とする。今回の対応付けは状態変更・未定義作業の追加・実行許可ではない。

<!-- traceability:end -->


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

2026-09-13のユーザー確定指示に従い、標準Vulkan 1.0全137core＋VK_KHR_surface/display/swapchain/display_swapchainを提供する単一目標の [WS030](https://github.com/awemorris/zedBSD/issues/388) を新設した。公開headerはlibc/include/vulkan/、独立実装はuserland/base/libvulkan/、配置は/lib/libvulkan.so。EGLは今回cancelし将来GLES-on-Vulkan時へ、Waylandは将来backendとする。上流実装は移入せず、固定した公式XMLから宣言・定数を独立生成する。

[WS014 p005](https://github.com/awemorris/zedBSD/issues/387) のq307旧clearは、直接Venus wire/GPU ioctlを使う有限clientであり「純粋な標準Vulkan APIアプリ」を満たさないため失効（uncleared）。q307の6画像・正常回収・同VM再openという実測と当時の試行履歴は保存し、新しいq308-i04で標準API化を訂正する。p005はin-progressとして再開し、Queue itemは必要library出力までpending。p002/p003のclear、WS014 incomplete、p004未実行、別WS029 i915後段を維持する。

[q308](https://github.com/awemorris/zedBSD/issues/362) の順序はWS030 p001→p002→p003→WS014 p005→WS030 p004。ユーザーは全実装・作業継続・GitHub同期を明示承認済み。見積720 active minutes、120分ごと点検、各command/VM/poll有限、無変更retry3回まで。HALの追加変更・aggregate make check・git add/commit/pushは許可しない。既存private hostへの転送許可を維持する。

全API、必須能力/limits、memory可視性、同期、FIFO/image再利用の意味論を未検証のままcompleteとしない。詳細はWS030の実装契約。依存するsource変更は計画・native lifecycle/親子依存・Projectの同期とreadback後に開始する。

## q308完了: 標準Vulkan・直接表示libraryと標準APIデモ（2026-09-13）

WS030 p001/p002/p003/p004とWS014 p005の標準API訂正をclearedとし、WS030 completed、q308 finished、active Queueなしとする。WS014はincomplete、p001/p004 planning、p004未queue、native i915は別WS029のまま。q307の旧scopeの実測と履歴は保持する。

`libc/include/vulkan/` にVulkan1.0の公開header、`userland/base/libvulkan/` に独立した全137 core＋選択direct-display WSI18の実装を提供し、`/lib/libvulkan.so` に配置した。vkdemoは標準Vulkan/WSIだけを使い、GPU ioctl/Venus codecをアプリへ持ち込まない。ABI、Noct再生成、155実exportとproc-address、全familyの限定意味論試験、U/Kの所有権・権限・失敗回収、適用C規約の独立レビューを実施した。正式CTS認証は主張しない。

最終 `q308-lifecycle-003` は実QEMU10.0.11/virglrenderer1.1.0/Intel ANVで6枚の回転直方体を描画し、実VNC/GPU readback/独立ray-texture oracleが一致（評価対象不一致0）。通常終了後6frame再起動、SIGINT後6frame再起動、640×480文字画面への復帰とechoによる画面更新、別processの表示競合拒否とowner35frame/DONEを確認した。42.671秒、QEMU exit0。最終書式変更後のkernel/appは実行済みbinaryと一致する。

承認済みHAL patch SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d` のみを適用し、既存hal_space_map_device/device usermapを補完した。追加HAL APIはない。PCI cache属性、queue総数63、allocator破棄、console/query/通知の修正と、先行失敗・再実行理由を保存した。公開coherent HOST_VISIBLE、256MiB aperture、native watchdog等の制約は能力監査へ記録した。

結果は `plan/ws030/results-q308.md`、155行の台帳は `plan/ws030/phase004/api-verification.md`、最終証拠は `plan/ws030/phase004/final-evidence/verification.json`、p005訂正は `plan/ws014/phase005/results-q308.md`、履歴は `plan/history/queue-q308.md`（いずれもlocal/uncommitted）。GitHubは計画Issue/Project/結果コメントの同期であり、source/doc/imageのgit add/commit/pushはユーザーが行う。EGLは今回cancel、Waylandは将来VK_KHR_wayland_surface backendとして追加する。
