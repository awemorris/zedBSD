<!-- awesome-plan project=zedbsd record=ws014-p001 -->

<!-- awesome-plan-current:start -->
Status: planning
Phase disposition: normal
Manual design hold: lifted by user; implementation Queue: none
<!-- awesome-plan-current:end -->

# WS014 Phase 001: virtio-gpu / Vulkan表示APIの設計

WSID: `ws014`
Combined ID: `ws014-p001`
Status: planning
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Queue: none

## 目的・受け入れ

virtio-gpuでのGPU bring-upに必要なカーネル/ユーザー空間/driverの責務、表示API、能力profile、検証環境、実装Phaseをレビュー可能な設計へまとめる。設計保留は今回のユーザー指示で解除。実装・クリアランスを意味しない。

## 作業と未決定事項

- 現行PCI/DMA/interruptとconsole/graphics providerを読み、virtio transportの不足を確認する。
- 最初の2D scanoutと、Vulkan描画/WSI/presentを別の検証点として定義する。
- Vulkan + VK_KHR_display/VK_KHR_swapchainを公式ユーザーAPIにする案、OS内部の最小ABI、buffer/contextの寿命・権限・submit/fenceを設計する。`/dev/gpuN`という旧案の位置付けもここで整理し、正式ABIとして未公開とする。
- Venus等のbackend、Mesa移植可否・DRM依存、ゲストWSI、host blobなど要求feature、宣言profile/拡張/制限を確認する。対応していない機能をVulkan対応と表示しない。
- display ownership、hotplug等の不足、bootfb takeover/fallback、console/panic、異常終了時の回収を決める。
- 既存GPU-D001〜D006を、オブジェクト寿命、メモリ/同期、隔離/復旧、能力、表示所有権、担当境界としてレビューする。i915/GLES固有事項はこのWSの対象外として後続先と理由を記録する。
- 上記について明示判断、受け入れ条件と必要依存を記録し、有限実装Phaseおよび終盤の適用規約全文確認を設計する。

## 調査・検証の範囲

公式仕様と現行ソースの静的調査。コード変更・build・QEMU実行はこの計画更新の範囲外。Guardrailを適用する。未知のhost/backend能力や実機動作を推測でPASSにしない。

## 現在結果・再開点

初期targetをi915からvirtio-gpuへ変更済み。Vulkanの表示APIは有望な候補だが、正式採用とOS ABIは未決定。次は2D→描画→presentの境界、backendと能力要件を具体化する。まだPhaseの受け入れ完了ではない。

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

<a id="vulkan-api-responsibility-table"></a>

[関数別責務表](../vulkan-api-responsibilities.md)。全文は上記GitHub Issueにも掲載。

## 2026-09-12 責務分類をU/Kに統一

ユーザー指示により、[Vulkan API責務表](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)の275関数を、U（ユーザー空間実装）とK（GPUドライバ）の二つの責務欄だけで整理した。旧Q/C/R分類と境界列を削除。キャッシュ・記録・転送は責務欄の説明として保持する。ドライバへの照会はK、結果の整形等はUであり、キャッシュ可能性を別分類にしない。関数集合とplanning状態、Queue未開始は維持。

<details>
<summary>2026-09-12より前の計画（i915 first・手動保留は上記判断で変更）</summary>

# WS014 Phase 001: GPU architecture discussion

Last updated: 2026-08-27

WSID: `ws014`

Phase ID: `p001`

Combined ID: `ws014-p001`

Status: Blocked (manual hold)

Parent: [WS014](../ws.md)

Reviews: [WS014 review index](../tests/README.md)

## Objective

Define the stable boundary beneath userspace Vulkan/GLES and above native GPU
drivers, together with display takeover and the first i915 target, before any
public UAPI or implementation Phase is selected.

## Baseline

WS004 already owns PCIe/DMA/MSI foundations and proposes i915 hardware work.
WS007 currently records `/dev/gpuN`, i915, Vulkan, GLES, and Wayland ideas in
one broad graphics/desktop WS. amd64 console and graphics currently use the
boot VBE/GOP linear framebuffer.

## Scope

- device discovery, version negotiation, capability profiles, and limits;
- context/process isolation, handles, buffers, images, mapping/sharing, cache
  transitions, queues, submissions, shaders/pipelines, and fences;
- validation, malformed input, hang/reset, detach, and process-exit cleanup;
- display ownership, modes, scanout, bootfb takeover, fallback, and panic path;
- kernel UAPI versus userspace Vulkan/GLES responsibility;
- `/bin/gpu` purpose and permissions;
- first Tiger Lake/i915 boundary, firmware, test double, and hardware evidence;
- transfer of responsibilities from WS004 and WS007 and later Phase ordering.

## Non-goals

- implementing a Linux DRM clone;
- copying Vulkan wholesale into the kernel ABI;
- promising compute, full Vulkan, or exact i915 hardware behavior before the
  declared profile and target inventory are fixed;
- source-code changes in this Phase.

## Open decisions

- Mandatory graphics profile and optional extensions, including compute.
- Shader input/validation boundary and whether any portable IR is UAPI.
- Buffer/image sharing, coherency, address-space, queue, and fence semantics.
- Single node with display-master rights versus separately permissioned roles.
- Atomic display-provider takeover and fallback ownership among GPU,
  `/dev/console`, and `/dev/graphics`.
- i915 kernel/user split, firmware policy, reset model, and safe physical gates.
- Exact WS004/WS007 responsibility transfer without renumbering completed
  Phases.

## Work packages

- [ ] Freeze the layer diagram and kernel/userspace/driver responsibilities.
- [ ] Freeze UAPI object lifetime, capability, memory, queue, sync, and error
      requirements.
- [ ] Freeze the reduced profile and optional compute policy.
- [ ] Freeze display takeover, fallback, panic, suspend/reset, and permissions.
- [ ] Freeze the first i915 target inputs, firmware, and evidence model.
- [ ] Decide WS004/WS007 transfer and downstream WS006/WS008/WS009 handoffs.
- [ ] Split implementation into bounded UAPI, core, display, i915, Vulkan,
      GLES, diagnostic, and integration Phases.

## Acceptance

Every review case in the [WS014 review index](../tests/README.md) has an
explicit decision, security/failure semantics, owner WS, and later acceptance
environment. This Phase makes no runtime claim.

## Actual results and evidence

The existing plans establish the desired product direction but leave the
kernel/userspace abstraction, security validation, display ownership, and
cross-WS ownership open. Discussion remains in progress outside any Queue.

## Interruption / resumption

Detailed GPU design is intentionally deferred. Resume only after the user
explicitly selects this discussion, beginning with mandatory/reduced capability
profiles and object lifetime. Do not Queue UAPI or i915 code first.

## Remaining debt and handoff

All code, fixtures, QEMU models, physical i915 validation, Vulkan/GLES work,
desktop integration, and documentation remain later extracted Phases.

</details>
