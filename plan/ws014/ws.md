<!-- awesome-plan project=zedbsd record=ws014 -->

<!-- awesome-plan-current:start -->
Status: planning
Manual design hold: lifted by user; implementation Queue: none
<!-- awesome-plan-current:end -->

# WS014: virtio-gpu bring-up

WSID: `ws014`
Status: planning
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2, O4
Parent: [Master](https://github.com/awemorris/zedBSD/issues/1)
Last verified Phase: none

## 単一の目標と完了条件

QEMUのvirtio-gpu上でzedBSDのGPU/表示経路を成立させ、宣言した描画・表示APIから描画内容を画面にpresentできること。初期環境はamd64 QEMUを候補とし、p001でdevice/backend/versionと受け入れ条件を固定する。Vulkan公開API案の可否・対象profileは未確定であり、2D出力だけをVulkan対応完了と呼ばない。

## 範囲と段階

1. p001で層分け、Vulkan表示API案、最小OS ABI、QEMU環境と有限実装Phaseを定義する。
2. virtio PCI/virtqueueと2D resource/scanout/transfer/flushによる画面更新、boot framebufferからの安全な切替を先に検証する。
3. 選定したVulkan実装とtransport/WSIを接続し、宣言したAPIで描画・presentする。画面列挙・mode選択・同期と資源回収を確認する。
4. console/graphics fallbackと権限分離を含む結合確認、変更ソースに対する適用規約全文確認を終盤Phaseへ含める。

i915実機対応、GLES2実装、デスクトップ全体の移植はこの単一目標に混ぜず、後続で選択された場合に別WSで扱う。旧案の内容と判断履歴は下に保持する。既存WS004/WS007等の実装責任はこの計画更新だけで移管しない。

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| ws014-p001 | [GPU architecture discussion](https://github.com/awemorris/zedBSD/issues/213) | planning | virtio-gpu初期環境、表示API案、OS責務、profileと実装Phase分解を確定 |

## 制約・再開点

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの適用規約に従う。HAL責務変更は既存の承認条件を守る。最初に現行PCI/DMA/interrupt/console/graphics基盤を確認し、リファクタリング前のコード配置を仮定しない。実装前に有限Queueの承認が必要。

今回の成果は資料確認と計画改訂のみ。build/QEMU/実機検証なし。Vulkan実装・API profile・ABI凍結・2D以後の受け入れ詳細をp001で続ける。

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
