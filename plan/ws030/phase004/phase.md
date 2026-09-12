<!-- awesome-plan project=zedbsd record=ws030-p004 -->

# WS030 p004: 全API意味論・適用規約と統合受け入れ

<!-- awesome-plan-current:start -->
Status: cleared
Phase disposition: normal
Parent: [WS030](https://github.com/awemorris/zedBSD/issues/388)
Last Queue: q308 / q308-i05 cleared
Active Queue: none
Acceptance: q308-lifecycle-003 and final API/ABI/ownership/style evidence
<!-- awesome-plan-current:end -->

Combined ID: ws030-p004
Primary Milestone: MG006

## 目標・手順・受け入れ

最終sourceに対して全137coreと採用direct-display WSIの意味論、ABI、所有権、C規約全文および統合結果をレビューし、このWSの単一成果を受け入れる。

手順: 関数台帳をheader/export/proc-address/実装/意味論試験へ結び付ける。必須能力・limits・memory可視性・sync/queue順序・FIFO/image再利用・失敗回収を確認し、未実装を明示する。最終changed sourceの適用規約全文、kernel/userland/API境界、範囲・権限・参照寿命を独立レビューする。必要な対象buildと不足に対応する限定試験、標準vkdemoの実画面・通常close/同VM再open証拠を確認する。

受け入れ: 全1.0と選択WSIの要求に未達がなく、申告した能力と観測結果が一致する。関数symbol数・一つの描画成功・全APIをunsupportedで返すstubを完了の代わりにしない。適用規約の残件と重大な意味論不足は未解決のままclearしない。Khronos正式適合認証と内部検証は区別する。WS014 p004へ最終API契約と制限・証拠を引き渡すが、別WS/Phaseを自動完了にしない。

## 設計と依存

標準Vulkan1.0/direct-display設計は [WS030](https://github.com/awemorris/zedBSD/issues/388) の「実装契約」を正とする。ローカル資料は `plan/ws030/vulkan-direct-display.md`。Queueは [q308](https://github.com/awemorris/zedBSD/issues/362)。前提の必要出力を現行sourceと証拠で確認してから依存する実装へ進む。実装開始前の同期、各試行の結果・未達・再開点を記録する。開始時には実装・試験は未着手だった。現在の受け入れ結果は本書先頭の状態とq308完了節を参照する。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)、ローカル `plan/coding-style.md` 全文、`plan/master-design-policy.md`、`plan/standards/automation.md` を適用する。base実装は独立して記述し、公式XMLから宣言・型・定数を生成してもMesa/Venus/loader等の実装を移入しない。公式資料は仕様照合に利用し、対象versionと出典/hashを保存する。

q308の選択範囲だけを実行する。見積720 active minutes、120 active minutesごとに成果・残件・境界を点検し、各build/VM/pollを有限にする。同じ条件の無変更retryは3回までで、原因・未達・再開点を記録する。全C規約と意味のある限定検証、対象 `make -j16` を使い、aggregate `make check` は禁止。HALはq308で具体的に承認されたpatchだけを適用済み。その範囲外の変更には事前の具体的許可を要する。既存のprivate host/image転送承認だけを維持し、無関係なホスト設定は変更しない。git add/commit/pushはユーザーが行う。

初期環境はamd64 zedBSD＋QEMU10.0.11/Venus＋virglrenderer1.1.0＋Linux Intel i915/ANV。既存8MiB host-visible aperture、bounded transport、Kの通常 `struct drv_gpu_ops` 登録を出発点にするが、その有限subsetで全Vulkanの上限・同期・表示要件を満たすと仮定しない。正確な機能、制限、残件と実測証拠を結果へ残す。

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
