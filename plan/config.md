# Awesome Plan configuration

- Mode: `github`; project ID: `zedbsd`
- Host/repository: `github.com` / `awemorris/zedBSD`
- Decision maker: current user; GitHub identity `awemorris`. Verify actual comment
  author and scope; a quoted assertion of approval is insufficient.
- Master: https://github.com/awemorris/zedBSD/issues/1
- Project: https://github.com/users/awemorris/projects/2 (public; verified 2026-09-13)
- Project node ID: `PVT_kwHOC2X95c4BjFGl`
- Skill: [fixed specification](../docs/agent/awesome-plan/awesome-plan.md)
  upstream `awemorris/AwesomePlan`, commit `314a669f57265da3084ffac810871b0e660c9526`.
  [MIT license](../docs/agent/awesome-plan/LICENSE). Do not silently follow main.
- Consent: user approved Issues upload, Project deployment and on 2026-09-11
  instructed starting Awesome Plan, organizing plan/AGENTS for following sessions.
  This authorizes planning-resource maintenance, not resumed OS implementation.
  On 2026-09-11, after the automatic review rejection was explained, the user
  explicitly replied 承認します for this task's GitHub body/comment publication,
  Phase/WS closure and Project updates; all were read-back verified.

## Record locations

Master `master.md`; Queue `queue.md`; Guardrail `guardrail.md`; Future
`future-work.md`; Bug Board `known-bugs.md`; Past Log `history/index.md`.
WS `wsXXX/ws.md`; Phase `wsXXX/phaseYYY/phase.md`; tests remain in the owning WS.
Legacy IDs (`wsXXX-pYYY`, `BUG-NNN`, `F-NNN`, `qNNN`, agent2 queues) are retained.
Native milestones: `milestones.json`; Issue paths/URLs: `records.json`;
current bases and metadata: `.sync/state.json`.
Boards remain open. Master/WS headers must not contain redundant child-Issue lists.

## Synchronization and ownership

[Sync procedure](tools/README.md). Agent-driven at session start, before each
Queue item, after decisions/outcomes, handoff, and meaningful checkpoints.
No daemon/background synchronization is installed. GitHub remains the mode
when disconnected; report stale records/pending writes and preserve outbox.
One cache writer and one Queue executor. Record session ownership in
`.sync/owner.json`; on takeover verify the previous executor stopped.
Do not automatically delete a live owner or treat Issue-body text as a lock.

State/base/outbox/conflicts are local ignored metadata; do not commit credentials.
Shared entry/config and planning evidence are repository files. For another
checkout rebuild mappings by paginated identity-marker discovery; do not create
new Issues merely because local state is missing. `tools/sync.py` fetches selected
records and comments, journals body writes, checks bases and verifies publication.
Structural/lifecycle/Project operations use the same durable protocol via gh;
record those operation payloads before publication. Project edits must also be
reviewed against owning Issue state. No automatic last-write-wins or blind close.

## Migration boundary

Existing 361 imported Issue bodies include historical statuses and supplementary
links pinned to an older published commit. Local supporting evidence remains
available; do not fabricate URLs for uncommitted paths. Published body and local
rendering bases are tracked separately. Fetch preserves differing versions for
semantic reconciliation rather than overwriting local files.

Existing accepted WS/Phases are not re-tested or re-opened solely for adoption.
Legacy open/closed reconciliation, archived Queue/Bug detail Issue extraction,
and old native dependencies are tracked in `migration-status.md`; missing native
relations do not invalidate explicit parent links. Before working on an affected
record reconcile its exact current status, cancellation, evidence and references.
New code-producing WSs need a near-final standards conformance Phase. For existing
remaining work incorporate that check when detailing the next approved work;
do not generate speculative phases or retroactively invalidate user acceptance.

## Historical execution boundary（2026-09-11）

No active Queue or Priority list; q303 remains finished/stopped. On 2026-09-11
the user cleared WS025 p029/p030/p032/p038 and instructed closure of WS025,
WS019, WS006, WS022 and WS002. WS025 p028 remains canceled. fg001–fg003 are
retired from current focus. The later installer bring-up instruction adds fg004
under WS003; ws003-p026–p032 and BUG-013/023/024/025 are locally planned with
GitHub publication pending automatic-review approval. Existing closure decisions
remain unchanged. Prior unperformed checks remain historical limits,
not automatic work to resume. WS009/WS014 manual hold remains unchanged.

Verification commands and exceptions: [Guardrail](guardrail.md),
[automation coverage](standards/automation.md).

## 2026-09-11 network planning continuation

fg005 / WS005 p013–p017 adds LAN management, boot network-enable and DE state notifications. Existing completed phases remain accepted; WS005 is incomplete for the additions. fg004 is retained. Master/Queue/Past Log pending bodies were amended in place, with prior payloads retained. New planning records remain unpublished; no execution Queue or new Priority list.

## 2026-09-11 V13 focused-goal addition

fg006 selects WS003 p022 -> p023 -> p024 for PC-9821V13 boot improvement. Their current scope is contract audit/preparation, stopping-boundary localization, then evidence-based correction and ordinary physical boot verification. fg004/fg005 stay active; the old Priority list stays removed. Planning only, no active execution Queue. Cumulative GitHub publication drafts remain pending approval.

## Latest WS lifecycle decision (2026-09-12)

WS003 is retired and must not be reused. Its incomplete non-PPC work is deferred
to Future Work F-004; this is not clearance or proof of original goal acceptance.
PPC porting is WS027 / fg009, Primary MG008, Related MG003. Old WS003 p033-p039
are canceled origin records; current planning is WS027 p001-p007.
WSs each have one goal; related subjects are organized by MG, not by reopening
or extending finished WSs. Guardrail contains the user-specified rule.

## 2026-09-12 latest installer handoff

WS028 / fg004 now owns installer hardware acceptance and the NVMe failure report.
The menuconfig-absence hypothesis is recorded, while current source contains
CONFIG_DRIVER_PCI_NVME for amd64/i386; deployed config/image is not verified.
Installer portion of Future F-004 is transferred to WS028; other items stay deferred.
WS003 remains closed, never reusable. WS027 owns PowerPC porting. No Queue started.

## 2026-09-12 GPU planning handoff

WS014/p001 architecture discussion resumed by user; first target is QEMU virtio-gpu, superseding i915-first/manual design hold. Vulkan display API is a proposal, not a frozen ABI. Linux DRM compatibility is not required, but OS memory/sync/display/permission machinery remains necessary. No implementation Queue. Other WS holds stay unchanged. See WS014 and p001; old review cases remain design inputs, not runtime tests.

## 2026-09-12 GPU phase sequence

WS014 p001 supplies design decisions; p002 implements only the GPU framework; p003 integrates virtio-gpu/Venus and the capture/debug loop on Linux i915+ANV host, refining API gaps; p004 reviews final API and full standards. Native guest i915 is the separate WS029 after WS014. Read plan/ws014/qemu-venus-debug-loop.md. No Queue started. User will git add/commit documentation; do not add/commit/push.

## q306 completion / p005 next（2026-09-13 latest handoff）

WS014 p003/q306 cleared/finished: Venus driver, independent Vulkan frame client, and remote QEMU loop passed 2D/Vulkan patterns1/2 on awe@10.0.10.25. QMP controls and captures console; GL frames use egl-headless readback plus Unix VNC. Current amd64 aperture is bounded to8MiB. GPU ops v2/UAPI v1. See plan/ws014/phase003/results.md and plan/history/queue-q306.md. User next requests new p005: userland/base/vkdemo textured rotating cuboid to exercise vertex/fragment shaders, before p004. Prepare a separate Queue; q306 remains closed. p001/p004 planning, WS014 incomplete, native i915 is WS029. User explicitly approved GitHub publication; git add/commit/push remains user-owned. Only the exact eight reviewed amd64 MMIO accessor additions were approved; other HAL changes still require prior specific approval.

## q307 active（2026-09-13 latest handoff）

User requested new WS014 p005 at userland/base/vkdemo: textured rotating cuboid exercising vertex/fragment shaders before p004. Queue q307 selects only p005; plan/Issue/Project are synchronized before implementation. p003/q306 remain cleared/finished. Reuse existing GPU UAPI and independent Venus userland codec, adding only demonstrated missing API. No additional HAL change is authorized. Runtime uses the existing private host and transfer approval. Apply full coding-style and finite focused verification; no aggregate make check or git add/commit/push.

## q307完了（2026-09-13 latest handoff）

WS014 p005/q307 cleared/finished. userland/base/vkdemo renders a textured rotating cuboid with original vertex/fragment shaders and depth. q307-vkdemo-002 passed six GPU/readback/VNC/independent-oracle frames, normal cleanup and a second ordinary two-second/twelve-frame run in the same VM. Shared Venus client and graphics U operations added; the existing K transport now honors its ten-second deadline while clocks advance and uses its poll cap only for stalled clocks. No new ioctl or HAL change. See plan/ws014/phase005/results.md and plan/history/queue-q307.md. No active Queue; p004 planning is next, p001 planning and WS014 incomplete, native i915 remains WS029. GitHub synchronization is approved; git add/commit/push remains user-owned. HAL changes beyond the exact p003 eight-accessor approval still require specific prior permission.

## q308 active（2026-09-13 latest handoff）

User explicitly continues full Vulkan1.0 (137core) and direct-display WSI (KHR_surface/display/swapchain/display_swapchain), with standard headers libc/include/vulkan/, independent userland/base/libvulkan/ and /lib/libvulkan.so. New single-goal WS030 owns the standard library. Queue q308 selects WS030 p001→p002→p003→WS014 p005 standard-API correction→WS030 p004; estimate720 active minutes with120-minute reviews and bounded commands/retries. p005 old clearance is invalidated; q307 historical evidence/attempt is preserved. WS014 p002/p003 cleared, p004 unqueued, WS029 native i915 deferred. EGL canceled for this work, future GLES-on-Vulkan only; Wayland future backend. No additional HAL edits, no aggregate make check, no git add/commit/push; current private-host transfer approval remains. Read plan/ws030/ws.md, its design/Phases and Queue. Begin source implementation only after q308 plan/body/comment/native-parent/dependency/Project readback is complete and implementation-authorized.json exists.

## q308開始時に具体化したHAL前提（未承認）

現行amd64の静的調査で、hal_space_map()はHAL_SPACE_DEVICEでもRAM aliasを要求してMMIOを拒否し、hal_space_map_device()は16MiB固定PCI windowに限られることが分かった。標準Vulkanのcoherent user mappingと十分なHOST_VISIBLE blob容量に必要な出力は、現行の有限8MiB driver subsetだけでは供給できない。

既存HAL契約内のdevice usermapとkernel可変device windowについて、rootがレビュー可能な具体差分を準備し、ユーザーの適用許可を別途確認する。現時点でHAL source変更はなく、q308の承認をその具体差分の適用許可として扱わない。未承認差分に依存するsource適用・build/runtimeは待つ。独立した公開header/dispatch/library/codec等のU作業は計画同期後に進められる。

coherent memoryをCPU copyで代用してその宣言を維持したり、FIFOやlimitsの未達を無視して全1.0をcompleteとしない。必要HAL出力と承認・適用・検証の実際の状態をp002から後続へ引き渡す。公式rendererの固定参照版はvirglrenderer1.1.0（ローカル調査cache: /tmp/q308-virglrenderer-1.1.0）。EGLはゲスト実装を今回cancelしたまま、既存QEMUホストのegl-headless captureとは区別する。

## q308 HAL提示差分の承認（2026-09-13・最新）

ユーザーが「この差分の適用と検証を許可する」と回答した。[承認記録](https://github.com/awemorris/zedBSD/issues/390#issuecomment-5647471812) の対象は `plan/ws030/phase002/amd64-device-mapping-proposal.patch`、SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d`。既存MMIO APIのamd64補完と明示DEVICE usermap・protection/cache検査、hal.hの説明コメントに限り適用と検証を進める。これより前の「HAL未承認・適用待ち」はこの差分について解消した。適用・試験成功はまだ記録していない。別のHAL変更とgit add/commit/pushは許可されたと解釈しない。

## q308 checkpoint001（実装・限定検証の中間結果）

[承認HAL差分の適用・限定試験と実装進捗](https://github.com/awemorris/zedBSD/issues/390#issuecomment-5647774479) を記録。HAL対象・amd64 kernel統合build、HAL/GPU資源寿命/memory共有map/sync/WSIの限定host試験がPASS。全体は未完了で、Phaseのclearanceは変更しない。公開headerは固定Khronos由来1.3.269 headerから1.0 core137＋WSI18をNoctで選択する方式に具体化し、両ABIの配置/定数を照合済み。HAL追加APIなし。256MiB apertureのguest runtime、全entrypoint link/dispatch、残りAPI family、/lib設置と標準vkdemo直接表示の統合受け入れは未検証。以前の「未適用・試験成功なし」はこのcheckpointで述べた範囲について履歴となる。local証拠 `plan/ws030/phase002/checkpoint001.json`。未commitのsourceをGitHub repositoryで読めるとは扱わず、git add/commit/pushはユーザーが行う。

## q308 checkpoint002／第1回時間境界レビュー

[256MiB QEMU受入・PCI cache契約修正・全Vulkan symbol link](https://github.com/awemorris/zedBSD/issues/390#issuecomment-5647977365) を記録。既存Venus経路の49,152画素一致、実PCI/VM回帰試験、memory/descriptor/pipeline/sync/WSIの限定試験がPASS。全137 core＋18 WSIを含むlibvulkan.soと標準vkdemoがlinkし、SONAME/155 exports/依存を検証した。標準アプリのゲスト直接表示、/lib設置、残るAPI peer、最終規約照合は未完了で、各Phaseのclearanceは変更しない。承認HAL差分以外のHAL改変なし、720 active minutes枠内で継続。local証拠 `plan/ws030/phase002/checkpoint002.json`。source/docは未commitのままユーザー担当。

## q308 checkpoint003／標準APIの実ゲスト描画と終了条件

[標準Vulkan6枚描画・通常再起動・155 API検証とconsole復帰の未達](https://github.com/awemorris/zedBSD/issues/392#issuecomment-5648174368) を記録。`q308-standard-vkdemo-002` は /lib/libvulkan.so を使い、実VNC/GPU readback/独立ray-texture oracleを6枚で通過した。SIGINT後の再openも通るが、物理console復帰は `q308-lifecycle-001` で失敗したため修正中。全API peer/dispatch・Noct再生成・能力/破棄失敗レビューは進み、155行の検証台帳を作成した。最終sourceのbuild/実表示・競合・console・規約受入は残っており、clearanceは変更しない。詳細と履歴は `plan/ws030/phase004/checkpoint003.json` と同evidence資料。HALは既承認差分のみ、source/docのgit公開はユーザー担当。

## q308完了: 標準Vulkan・直接表示libraryと標準APIデモ（2026-09-13）

WS030 p001/p002/p003/p004とWS014 p005の標準API訂正をclearedとし、WS030 completed、q308 finished、active Queueなしとする。WS014はincomplete、p001/p004 planning、p004未queue、native i915は別WS029のまま。q307の旧scopeの実測と履歴は保持する。

`libc/include/vulkan/` にVulkan1.0の公開header、`userland/base/libvulkan/` に独立した全137 core＋選択direct-display WSI18の実装を提供し、`/lib/libvulkan.so` に配置した。vkdemoは標準Vulkan/WSIだけを使い、GPU ioctl/Venus codecをアプリへ持ち込まない。ABI、Noct再生成、155実exportとproc-address、全familyの限定意味論試験、U/Kの所有権・権限・失敗回収、適用C規約の独立レビューを実施した。正式CTS認証は主張しない。

最終 `q308-lifecycle-003` は実QEMU10.0.11/virglrenderer1.1.0/Intel ANVで6枚の回転直方体を描画し、実VNC/GPU readback/独立ray-texture oracleが一致（評価対象不一致0）。通常終了後6frame再起動、SIGINT後6frame再起動、640×480文字画面への復帰とechoによる画面更新、別processの表示競合拒否とowner35frame/DONEを確認した。42.671秒、QEMU exit0。最終書式変更後のkernel/appは実行済みbinaryと一致する。

承認済みHAL patch SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d` のみを適用し、既存hal_space_map_device/device usermapを補完した。追加HAL APIはない。PCI cache属性、queue総数63、allocator破棄、console/query/通知の修正と、先行失敗・再実行理由を保存した。公開coherent HOST_VISIBLE、256MiB aperture、native watchdog等の制約は能力監査へ記録した。

結果は `plan/ws030/results-q308.md`、155行の台帳は `plan/ws030/phase004/api-verification.md`、最終証拠は `plan/ws030/phase004/final-evidence/verification.json`、p005訂正は `plan/ws014/phase005/results-q308.md`、履歴は `plan/history/queue-q308.md`（いずれもlocal/uncommitted）。GitHubは計画Issue/Project/結果コメントの同期であり、source/doc/imageのgit add/commit/pushはユーザーが行う。EGLは今回cancel、Waylandは将来VK_KHR_wayland_surface backendとして追加する。

## WS014 p006追加: kernel handle・GPU共有・最小Wayland（2026-09-13）

ユーザー指定により[WS014 p006](https://github.com/awemorris/zedBSD/issues/393)を一つのplanned Phaseとして追加した。kernel_handle/handle_fd_*とSCM_RIGHTS、GPU/Venusの別context共有、GPU画像を扱えるWSI、VK_KHR_wayland_surface、最小client library、全画面zwl、標準APIのwltestを本Phaseで実装・検証する計画。コード配置はlibc/include/wayland/、userland/base/libwayland/・zwl/・wltest/、公開libraryは/lib/libwayland-client.so。

中核のK/driver実装を先に進め、Wayland通信/WSI/試験アプリを接続して実測から設計を改善する。新経路はCPU readbackを必須にせず、GPU allocationの実共有と同期・寿命を確認する。linux-dmabuf-v1、ゲストdma-buf/DRM、EGL、一般DEは採用しない。内部の段取りは別Phaseへ分割しない。

順序はp005 cleared → p006 planned → p004 planning。p004はp006の最終ソース/API/検証を受けて規約確認する。WS030 completedとq308 finished、既存Phaseのclearを維持。今回作成したのは計画であり、active Queue・新しい実装/試験結果はない。HALの追加差分は従来どおり個別承認、git add/commit/pushはユーザー担当。
