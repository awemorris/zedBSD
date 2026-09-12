<!-- awesome-plan project=zedbsd record=ws030 -->

# WS030: 標準Vulkan 1.0と直接表示ライブラリ

<!-- awesome-plan-current:start -->
Status: completed
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O2
Parent: [Master](https://github.com/awemorris/zedBSD/issues/1)
Implementation Queue: none
Last Queue: q308 finished
Phases: p001 / p002 / p003 / p004 cleared
Reuse: prohibited; new objectives require a new WS
<!-- awesome-plan-current:end -->

## 単一目標・完了条件

標準Vulkan 1.0の全137core関数と採用したdirect-display WSIを、標準headerと `/lib/libvulkan.so` によりアプリへ提供する。宣言した必須能力・limits・同期・memory可視性・表示の意味論を満たし、純粋な標準Vulkan APIアプリから実画面へpresentできることが完了条件。symbolやデモsubsetだけの実装を全1.0と呼ばない。

Primary MG006へ標準描画/直接表示のアプリ基盤を提供し、Related MG002へ共有library/ABI利用の検証結果を提供する。WS014はframework/virtio-gpu bring-upとp005デモ訂正を所有し、広い標準libraryの新成果はこのWSへ分離する。WS014 p004と別WS029 native i915は今回のQueue外。EGLは今回cancel、将来GLES-on-Vulkanの検討時へ保留。Waylandは将来backend。別目標をこのWSへ混ぜない。

## Phase registry

| Phase | 目的・出力 | 状態 | 依存 | Queue attempt |
| --- | --- | --- | --- | --- |
| [ws030-p001](https://github.com/awemorris/zedBSD/issues/389) | 公開header・共有library・dispatch | cleared | verified WS014 p002/p003 outputs | q308-i01 |
| [ws030-p002](https://github.com/awemorris/zedBSD/issues/390) | memory・同期・汎用transportとK支援 | cleared | ws030-p001 | q308-i02 |
| [ws030-p003](https://github.com/awemorris/zedBSD/issues/391) | 全Vulkan1.0 core・描画とdirect-display WSI | cleared | ws030-p002 | q308-i03 |
| [ws030-p004](https://github.com/awemorris/zedBSD/issues/392) | 全API意味論・適用規約と統合受け入れ | cleared | ws030-p003 and ws014-p005 corrected output | q308-i05 |

p003の標準library出力を [WS014 p005](https://github.com/awemorris/zedBSD/issues/387) が利用する。その標準APIデモの実画面結果を本WS p004が検証する。これはscoped-output依存であり、WS014全体の完了を本WSの前提にして循環させない。

## 実装契約

設計決定日: 2026-09-13。以下は開始時の実装契約。最終の適用範囲と証拠はq308完了節と結果資料を参照する。

## ユーザー決定と所有する成果

ユーザーはvkdemoを純粋な標準Vulkan APIアプリとして実装すること、公開headerを `libc/include/vulkan/`、独立した実装を `userland/base/libvulkan/`、共有libraryを `/lib/libvulkan.so` へ配置することを指定した。その後、Vulkan 1.0全体とdirect-display WSIの実装を明示し、作業継続とGitHub同期を承認した。EGLは今回キャンセルし、将来GLES-on-Vulkanが選択された時の課題とする。Waylandは将来のWSI backendであり今回実装しない。

WS030の単一目標は、標準Vulkan 1.0と採用したdirect-display拡張を使うアプリへ実用の共有libraryを提供すること。WS014は既存のGPU framework/virtio-gpu bring-upを保持し、p005だけを標準API利用へ訂正する。WS014自体の完了や別WS029 native i915を前倒ししない。終了WSを再利用せず、広い標準libraryの独立成果を新WSで管理する。

## 公開APIと能力宣言

Vulkan 1.0の全137 core関数、対応する型・定数・callback・ABIを対象にする。direct-display WSIは `VK_KHR_surface`、`VK_KHR_display`、`VK_KHR_swapchain`、`VK_KHR_display_swapchain` を対象とし、採用revisionと1.0に適用されるcommand集合・拡張依存条件を公式registryから固定する。旧資料のVulkan1.0〜1.4/275関数一覧は参考であり、後年の拡張やDRM専用APIを自動追加しない。

137個のsymbolを生成しただけ、全呼出しにunsupportedを返すだけ、デモが使うsubsetだけ、ホスト能力を無検証で転記するだけでは全1.0を完了にしない。必須機能・limits・format/image要件・queue・記憶可視性を実装と照合し、任意featureは実装済みの場合だけ宣言する。`vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` のinstance/device/version/extensionごとの取得規則、enumerationのcount/partial/VK_INCOMPLETE、allocator callback、device-lostと失敗後回収も契約に含める。正式なKhronos認証・商標利用が成立したとは別途証拠なしで主張しない。

公開headerは固定した公式XMLの宣言・定数等を入力に独自generatorで再現可能に作る。XML・generator・生成物のhash、出典と必要なライセンス表示を保持する。上流のC実装や生成済み実装は移入しない。

## U/Kとtransportの境界

標準Vulkanのhandle、pNext、allocator、command記録、dispatch、loader/ICD相当のユーザー実装はlibvulkan内が所有する。アプリからVenus wireやGPU ioctl、`/dev/gpuN`、kernel resource IDを直接扱わない。Uがpointer/cardinality/size/extensionを検証して転送用表現へ変換し、Kは長さ・範囲・session所有権・権限を独立して検証する。KへVulkan関数を一対一で増やす設計にしない。

必要な汎用transport、memory mapping/visibility、resource/sync/displayの支援は現行K責務内で補う。既存PCI serviceから通常 `drv_gpu_register()` / `drv_gpu_unregister()` を呼ぶ契約と `struct drv_gpu_ops` の名称を維持する。transportの受付、renderer返信、GPU実行完了、表示進行、imageの再使用可能時点を区別する。

`vkMapMemory` とcoherent/non-coherent memoryのCPU/GPU可視性、flush/invalidate、queue wait、binary semaphore、fence、event、command bufferのprimary/secondary・再利用・同時使用条件を宣言どおり実現する。Vulkan fenceはsignaled/unsignaledの同期objectでありtimeline fenceではない。timeline semaphoreは1.0の必須機能へ混ぜない。

virglrenderer1.1.0で `vkQueueWaitIdle` / `vkDeviceWaitIdle` のwire dispatchをそのまま呼べないという既存調査を引き継ぎ、標準APIを省く代わりにUが正しい完了を観測する実装を用意する。既存clientの固定object ID/bootstrap・stream/reply上限を汎用の実装能力と混同しない。

## Direct-display WSI

display/plane/modeの列挙とcapabilities、利用可能なdisplayの取得・解放、surfaceとswapchainの所有権、image列挙、acquireのtimeoutとsemaphore/fence、present待ち、swapchain再作成・oldSwapchain・資源解放を定義し、実装の裏付けがある値だけ返す。

FIFO present modeの必要な進行・順序・tearing制約は独立した受け入れ項目。現行 `GPU_PRESENT` のflush受付とユーザー空間sleepだけでFIFOや表示完了が証明されたとはしない。virtio GPUが直接供給しないvblank等は、実現可能なvirtual presentation契約を公式仕様と実装から検討し、未証明ならその条件を未達として残す。imageの再利用は表示側が解放したこととGPU実行完了の両方を守る。正常close、異常終了、複数processの所有権競合、console復帰を確認する。

DRM互換を前提にしない。Wayland、EGL、GLES/OpenGL、native i915、別GPU/実機portはこのWSの実装対象外。これらのために現在のWSを再利用しない。

## Buildと標準アプリによる証明

既存amd64 PIC・ld.so・libc.soとpackage登録を利用する。SONAMEは `libvulkan.so`、インストール先は `/lib/libvulkan.so`。公開headerの階層をsysrootで保持する。vkdemoは `/lib/ld.so` を利用し、ELFのDT_NEEDEDでlibvulkanを参照する標準APIアプリへ修正する。library内部のVenus helperを公開exportへ漏らさない。

標準headerだけで独立したアプリをcompile/linkできること、全137関数と採用WSIのheader/symbol/proc-address対応、意味論テスト、対象build、同じVMでの実表示・GPU readback・複数frame・通常終了/再起動を確認する。p005の独立ray/texture期待値と実VNC画像の検証を維持し、APIを標準化した経路で新しく実行する。旧q307の6枚一致・cleanup/reopen結果は有限Venus clientの履歴であり、標準APIの試験を代替しない。

全機能・limits・可視性・FIFOが未検証のままWS030/p004をclearedにしない。実現できない依存が判明したら具体的な未達、試行と再開条件を残し、他の独立した許可済み作業を進める。


## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)、ローカル `plan/coding-style.md` 全文、`plan/master-design-policy.md`、`plan/standards/automation.md` を適用する。base実装は独立して記述し、公式XMLから宣言・型・定数を生成してもMesa/Venus/loader等の実装を移入しない。公式資料は仕様照合に利用し、対象versionと出典/hashを保存する。

q308の選択範囲だけを実行する。見積720 active minutes、120 active minutesごとに成果・残件・境界を点検し、各build/VM/pollを有限にする。同じ条件の無変更retryは3回までで、原因・未達・再開点を記録する。全C規約と意味のある限定検証、対象 `make -j16` を使い、aggregate `make check` は禁止。HALはq308で具体的に承認されたpatchだけを適用済み。その範囲外の変更には事前の具体的許可を要する。既存のprivate host/image転送承認だけを維持し、無関係なホスト設定は変更しない。git add/commit/pushはユーザーが行う。

初期環境はamd64 zedBSD＋QEMU10.0.11/Venus＋virglrenderer1.1.0＋Linux Intel i915/ANV。既存8MiB host-visible aperture、bounded transport、Kの通常 `struct drv_gpu_ops` 登録を出発点にするが、その有限subsetで全Vulkanの上限・同期・表示要件を満たすと仮定しない。正確な機能、制限、残件と実測証拠を結果へ残す。

## q308開始時に具体化したHAL前提（未承認）

現行amd64の静的調査で、hal_space_map()はHAL_SPACE_DEVICEでもRAM aliasを要求してMMIOを拒否し、hal_space_map_device()は16MiB固定PCI windowに限られることが分かった。標準Vulkanのcoherent user mappingと十分なHOST_VISIBLE blob容量に必要な出力は、現行の有限8MiB driver subsetだけでは供給できない。

既存HAL契約内のdevice usermapとkernel可変device windowについて、rootがレビュー可能な具体差分を準備し、ユーザーの適用許可を別途確認する。現時点でHAL source変更はなく、q308の承認をその具体差分の適用許可として扱わない。未承認差分に依存するsource適用・build/runtimeは待つ。独立した公開header/dispatch/library/codec等のU作業は計画同期後に進められる。

coherent memoryをCPU copyで代用してその宣言を維持したり、FIFOやlimitsの未達を無視して全1.0をcompleteとしない。必要HAL出力と承認・適用・検証の実際の状態をp002から後続へ引き渡す。公式rendererの固定参照版はvirglrenderer1.1.0（ローカル調査cache: /tmp/q308-virglrenderer-1.1.0）。EGLはゲスト実装を今回cancelしたまま、既存QEMUホストのegl-headless captureとは区別する。

## 既存MMIO APIの補完案（レビュー待ち）

現在のVenusも `hal_space_map_device()` を利用している。新しいHAL APIを増やす案ではなく、既存APIのamd64実装に可変kernel device windowと明示DEVICE usermapを補う。[未適用の具体差分・静的レビュー・許可後の検証](https://github.com/awemorris/zedBSD/issues/390#issuecomment-5647425525) を提示済み。HAL sourceは未変更、適用許可は未取得。独立U作業を続行する。

## q308 HAL提示差分の承認（2026-09-13・履歴）

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
