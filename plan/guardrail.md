<!-- awesome-plan-current:start -->
Policy: current
Active Queue: none; q308 finished
HAL: every new source change requires prior specific user approval
q306 approval: exact eight amd64 MMIO accessors only; applied and verified
q308 approval: exact device-mapping patch e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d; applied and verified
No further HAL changes authorized
Git publication: user owns add/commit/push
<!-- awesome-plan-current:end -->

# Guardrail

Current project contribution rules, adopted 2026-09-10. User decisions prevail
when more recent or more specific; archived approvals are not standing authority.

## Scope and architecture

- Work only within the approved Phase scope. Kernel src/, include/, bootloader/,
  libc/, userland/, platform/, build/config and tools have different ownership;
  inspect current code and Phase before adding files or changing module boundaries.
- `include/hal/hal.h` and HAL responsibilities require explicit applicable user
  approval. The old VM ownership move, vmap removal plan and pmem argument expansion
  were superseded by the user's rollback/refactor. Do not replay them. Existing
  `hal_space_*` and shared kernel mapping support are not permission to redesign HAL.
- Keep the manually reviewed driver organization and `drv_` global-symbol policy.
  Trust verified refactoring over stale test assumptions; repair tests as appropriate.
- RTL8822B `.inc` files separate licensing and MUST remain separate. The earlier
  general instruction to merge `.inc` does not apply to these files.
- Base-system implementation/licensing boundaries: [design policy](master-design-policy.md).
  That document also contains historical milestones; current Master and source
  supersede outdated installer/HAL implementation descriptions.
- Do not copy kernel implementation into userland build dependencies. mkfs tools
  must remain independently usable; preserve the user-authorized source-copy split.

## Standards and verification

Full C standard: [coding-style.md](coding-style.md). Use the full applicable
sections before code generation; no unverified condensed substitute is installed.
The user's WS025 instruction allowed approximate style adherence followed by
later cleanup; preserve that scoped exception, not as a blanket exemption for
new WSs. Preserve evaluation order, ownership and behavior in style changes.
Tool coverage/versions: [automation](standards/automation.md).

- No `git commit` or push without user instruction; keep unrelated modifications.
- Do not read repository `.internal/`. No credentials in plans, Issues or sync state.
- Never run aggregate `make check`. Use meaningful focused checks for the Phase.
- Supported build gate: `make -j16` with the selected platform configuration.
  amd64 runtime uses `qemu-system-x86_64`; destructive tests use disposable images.
- User acceptance scope prevails: do not reintroduce rejected exhaustive abnormal
  tests, repeated physical boots, or hardware gates already waived by the user.
  QEMU and physical evidence remain distinct; describe precisely what was observed.
- Explain only actual external approval blockers. Existing exact-scope authorization
  persists; this adoption does not add a confirmation step for routine plan updates.

## Decision sources

User thread: HAL responsibility/hal.h restrictions and subsequent rollback review;
RTL8822B licensing exception; refactor/test trust; no commit; WS025 style flexibility;
normal-path installer acceptance and QEMU-only UAS acceptance. Retained source
records: [post-rollback review](ws025/post-rollback-review.md),
[WS025](ws025/ws.md), [WS019](ws019/ws.md). Older MWP-Q instructions are in old/.
New rules must update this registry, applicable full standard, tool coverage and
impacted plans; do not silently replace agreed architecture or scope.

## WSの単一目標と終了後の扱い（2026-09-12ユーザー指示）

WSは一つの具体的な到達目標を持つ。目標を達成したWS、またはユーザーが終了したWSは再利用・再開して別の目標を追加しない。似た領域だからという理由で一つのWSへまとめない。機種対応などの上位分類・到達点はMGが担い、インストーラ実機動作、PowerPC移植などは別のWSを作る。
一つの目標に必要な依存作業をPhaseへ分解することは可能だが、独立した別目標をPhaseとして混ぜない。WS終了時は子Phaseを全件照合し、未完了は完了に改変せず、ユーザー指定の保留先または別WSへ引き継いで元Phaseを終了する。旧ID、結果、転送先を残す。今回WS003は終了・再利用禁止、PPC移植はWS027へ、その他の未完了はFuture Workへ移す。

## 2026-09-12 HAL変更の承認条件（ユーザー確認）

ユーザーが「HALの改変には許可が必要です」と明示。HAL責務やhal.hの変更に限らず、src/hal/配下の既存宣言への実装追加・補完も、適用可能な明示許可を得てから行う。既存契約の補完を理由に承認不要と解釈しない。レビュー可能な具体差分を用意し、未許可のHAL変更に依存する実装適用・実行試験は待つ。

## q306 HAL変更の承認・再開（2026-09-12）

ユーザーが提示済み差分に「許可します。」と回答した。`plan/ws014/phase003/amd64-mmio-proposal.patch` の8個のamd64 MMIO read/write accessorの適用・検証を許可されたため、同一差分を適用し、build/QEMU検証を再開する。hal.hやHALの責務は変更しない。直前の「HAL変更の許可待ち」は解消済み。今後の別のHAL変更には、その具体差分に対する事前許可を引き続き必要とする。

PCI BARのcapability部分だけをmapして失敗する問題をdriver側で修正し、register BARを一度だけ全体mapして各capabilityに範囲を渡す。driver単体・ASan/UBSan試験は通過済み。実際のVulkan描画は引き続き未検証で、p003/q306はin-progress。

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
