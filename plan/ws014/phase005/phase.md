<!-- awesome-plan project=zedbsd record=ws014-p005 -->

# WS014 p005: 標準Vulkan APIによるテクスチャ付き回転直方体

<!-- awesome-plan-current:start -->
Status: cleared
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Last Queue: q308 / q308-i04 cleared
Active Queue: none
Acceptance: q308-lifecycle-003 and final API/ABI/ownership/style evidence
<!-- awesome-plan-current:end -->

Combined ID: ws014-p005
Primary Milestone: MG006

## 訂正理由と単一の到達点

ユーザーが指定するvkdemoは純粋な標準Vulkan APIアプリであり、直接Venus wire/GPU ioctlを符号化する有限clientではない。旧q307は実shader/texture/depth/回転/6画面一致/正常回収を検証したが、このAPI境界を満たしていなかったためp005の現在clearを失効してunclearedへ戻す。今回q308で標準API化する訂正をin-progressとして再開する。依存するlibrary出力が揃うまでq308-i04はpendingで、依存実装へ先行しない。

単一到達点は、標準headerと `/lib/libvulkan.so` を使う `userland/base/vkdemo` が、テクスチャ付き非等辺直方体をvertex/fragment shader・depthで描画し、時刻とともに回転させdirect-display surface/swapchainへpresentできること。汎用library全体の実装責任は新しい [WS030](https://github.com/awemorris/zedBSD/issues/388) が持つ。WS014のGPU bring-up目標へ別の標準library目標を混ぜない。

## 実装と受け入れ

- アプリは標準 `<vulkan/vulkan.h>` のvk APIだけでGPUへアクセスし、Venus/K内部header、GPU ioctl、kernel resource ID、backend command番号に依存しない。物理device・display/plane/modeを標準APIで選択し、surface/swapchain/imageを管理する。
- vkdemoをlibvulkan.soへ動的linkし、DT_NEEDEDと `/lib/ld.so`、公開headerだけのcompile/linkを確認する。独自GLSL→SPIR-V、texture、36頂点、depth、同じrender関数による固定/実時間frameという元の描画要件を維持する。
- acquire/submit/present、memory mapping/可視性、VkFence等を標準APIで使い、GPU readbackと実VNC画像を独立したray/texture期待値へ照合する。複数frame・通常アニメーション・正常終了・同VMの再openを最終sourceで新しく確認する。
- 必要なWS030 p003出力が現行sourceにあり、宣言能力と同期・表示意味論が成立することを確認してから実行する。新しい試験を旧q307成功で代用せず、追加API不足・変更範囲・検証・制限を記録する。

## 設計・実行境界

[WS030](https://github.com/awemorris/zedBSD/issues/388) の標準Vulkan1.0/direct-display契約と [q308](https://github.com/awemorris/zedBSD/issues/362) を適用。EGLは今回cancel、Waylandは将来backend。HAL追加変更は未許可。全C規約、独立実装、有限の対象build/試験、既存private host転送承認を維持。aggregate make checkとgit add/commit/pushは行わない。WS014 p004とnative i915は未実行の後段とする。

## q307の過去の実測・試行履歴

以下は旧scopeで実行したq307の結果。画像・回収の観測と当時のclearは履歴として保持する。現在のp005をclearedとする証拠や標準Vulkan対応の根拠として読み替えない。`plan/history/queue-q307.md` と既存results/evidenceは不変。

## q307完了: p005 cleared（2026-09-13 JST）

userland/base/vkdemoにテクスチャ付き回転直方体を実装。独自vertex/fragment shader、実texture、depth、Vulkan pipelineを使用する。q307-vkdemo-002で固定3時刻と実時間3枚のGPU readback/VNC hashが一致し、独立したray/texture期待値との照合も不一致0。正常終了後、同じVMで通常2秒・12frameの回転を再openしてDONE/shell復帰、QEMU exit0まで確認した。

新規ioctlは不要。Uの共通Venus clientとgraphics操作を追加し、実測で発見したKのblob unmap待機の早期timeoutを修正した。clock進行中は10秒deadlineを維持し、clock停止中だけ連続poll上限を使う。HAL追加変更なし。有限host tests、shader/CLI/画像検証、専用amd64 build、p003回帰がPASS。

q307 finished、q307-i01/p005 cleared、active Queueなし。p003/q306の完了を維持し、次はp004（planning、未実行）。p001 planning、WS014 incomplete。native i915は別WS029。汎用libvulkan/ICD・全Vulkan適合・汎用WSI/zero-copyは未実装。

実測結果とAPI表は[p005](https://github.com/awemorris/zedBSD/issues/387)。ローカルのplan/ws014/phase005/results.md、api-coverage.md、evidence/とplan/history/queue-q307.mdへ保存。GitHubは計画/結果本文を同期し、source・資料・画像のgit add/commit/pushはユーザーが行う。

# WS014 p005 / q307 実行結果

2026-09-13 JST。`userland/base/vkdemo/` を実装し、QEMU/Venusでテクスチャ付き非等辺直方体の回転、vertex/fragment shader、depth、連続frameと終了・再openを確認した。q307-vkdemo-002が受け入れ成功。p005をcleared、q307/q307-i01をfinished/clearedとする。p004はplanningのまま次の候補、p001の未決定を保持し、WS014はincomplete。

## 実装

320x240のRGBA8 color、D32 depth、36頂点と64x64 checker texture。独自GLSLのvertex shaderが時刻push constantから回転・透視変換を実行し、fragment shaderが補間UVから実textureをsampleする。shadercでSPIR-Vにcompileしspirv-valで検証した元shader、生成物、hash、再生成手順を保存した。shader再生成は有限timeout・同一source snapshot・両shader検証後の公開・失敗時の復元を備える。

`/bin/vkdemo` は既定10秒回転する。`--duration=30`、`--duration=0`（継続）、`--time-ms=1000 --hold=20` も指定できる。全モードが同じ描画関数を使う。frameごとに完了したVkFence/command poolをresetし、保持したtexture・descriptor・pipeline・upload/readback/presentation資源を再利用する。各VkDeviceMemoryのblob exportは一度だけ。CPUは元頂点・textureを作り、GPU描画後のreadbackとcopy表示を行う。完成した直方体画像をCPUで描いて代用しない。

共通のwire/reply/bootstrapを `userland/gpu/venus/client.[ch]` に抽出し、caller所有のsessionへ状態を保持した。`venus-frame` も共通clientを使用する。sceneとVulkan objectの寿命は各アプリが扱う。

## 六つの実画面

| frame | mode | time_ms | 独立RGB照合数 | 境界除外数 | 不一致 |
| --- | --- | --- | --- | --- | --- |
| 1 | fixed | 0 | 75,841 | 959 | 0 |
| 2 | fixed | 1000 | 76,045 | 755 | 0 |
| 3 | fixed | 2500 | 76,089 | 711 | 0 |
| 4 | live | 0 | 75,841 | 959 | 0 |
| 5 | live | 640 | 75,907 | 893 | 0 |
| 6 | live | 1250 | 76,166 | 634 | 0 |

各画像の全76,800 RGB pixelについて、GPU readback SHA256とVNC実画面SHA256が一致した。その上でcamera rayと直方体の交差・nearest texelから独立に期待値を求め、表の画素を完全一致で照合した。除外は幾何境界0.30pixel以内・texel境界0.02texel以内のみで、実測は画面の約0.83〜1.25%、上限5%以下。clearのみ、無地、未回転、誤UV、古いframeを拒否する有限fixtureも通過した。

固定3時刻と、実際に単調時計を進めたlive3枚を同じprocess/contextで描画した。各PRESENT後に最大30秒のstdin ACK待合せでその画像を保持し、hostは検証後に次frameを許可した。固定画像とlive画像を別のrender実装へ切り替えていない。

## 通常実行と回収

6枚の検証後に正常終了してshellへ戻り、同じVMで `/bin/vkdemo --duration=2` を再openした。実時間0〜1980msの12frameが進行し、DONEとshell復帰を確認。Vulkan資源とGPU sessionを解放した後に新しいcontextで実行できた。QEMU全体は13.964秒、終了code0。frame rateはreadback/copy経路を含むこの試行の結果であり、性能保証ではない。

## 発見・修正した問題

| 検出 | 原因 | 対応と確認 |
| --- | --- | --- |
| 最初のimage buildにvkdemoが含まれない | package分類graphicsが既存のbasic/network選択に入らない | 通常のbasic programとして/binへ登録。wrapperはbinaryの存在とhashを必須確認 |
| q307-vkdemo-001は6画像成功後にcleanup ENODEV | QEMU10のblob unmapはMR/RCU/BHによる非同期処理。旧transportは総50,000,000poll上限が10秒deadlineより先に尽き、readback blobをquarantineした | clockが進む間は10秒deadlineまで待ち、poll上限はclock停止中の連続回数だけへ変更。q307-vkdemo-002で正常cleanupと再openを確認 |

修正は既存の `src/drivers/gpu/venus/transport.c` の待機判定だけ。timeout時のDMA保持とreset確認後の回収を維持し、command・elapsed・stalled poll数をログへ加えた。旧上限を越える50,000,004回目の遅延完了と、時計停止時の有限timeoutを本番transportで検証した。新ioctl、内部ops追加、HAL変更は不要だった。HAL差分はp003で明示許可された8accessor proposalと完全一致する。

q307-vkdemo-001の失敗を成功扱いせず、画像・ログ・source/hashを別に保存した。修正→再build→別のimage/新規QEMU→同じ独立検証→正常終了を実行した。

## 有限検証と範囲

- shared client: 独立2session、bootstrap途中失敗・close、wire/reply境界、chunk転送、返信marker・pending・timeout。通常/ASan/UBSan/leaks/guest C89がPASS。
- transport: 遅延完了、時計停止、queue wrap、capset/parser、異常返信、DMA quarantine/resetを通常/ASan/UBSan/leaksで確認。
- 画像oracleとwrapperの12fixture、shader再生成の7fixture、CLIの8受理・20拒否、既存RFBの8peer caseがPASS。
- 共通化後のp003回帰: q307-regress-2d-001（赤緑）、q307-regress-venus-001（青黄）、最終transport後のq307-regress-2d-002（青黄）。すべて全49,152RGB pixel一致、QEMU exit0。
- 専用amd64 image build、vmunix/image checker、host/guest C89診断、規約全文と独立wire/lifetime review、差分/新規ソースの空白検査がPASS。aggregate make checkは使用しない。

runtime hostは引き続きawe@10.0.10.25、QEMU10.0.11、virglrenderer1.1.0、Intel i915/ANV25.2.6、KVM、OVMF、egl-headless、8MiB hostmem。QMPが起動/入力/consoleを制御し、実GL画面はVNC Unix RAWで取得する。通常config・system package・他VMは変更していない。

[追加Vulkan操作の表](api-coverage.md) と同じWSのU/K責務資料へ反映した。Uのgraphics pipeline/descriptor/shader/binding/drawを追加し、Kは既存capset/blob/copy/command/presentで足りた。一般のlibvulkan.so/loader/ICD、全Vulkan適合、汎用WSI/zero-copy/mmap、present完了通知、native i915は未実装。p004へ対象subset・制限と待機修正の根拠を引き渡す。

## 再現と証拠

```sh
python3 -B plan/ws014/tests/run-vkdemo-remote.py --attempt q307-vkdemo-NNN
```

NNNには未使用名を指定する。既定で専用configとbuild directoryから `make -j16 disk-image` を実行し、転送・画像取得・hash再照合まで行う。[実行手順](../tests/README-vkdemo-remote.md) と `userland/base/vkdemo/README.md` を参照。

- kernel: `61c0b6c502f6c848f5b99ddc64530427dcc61cb7de3d5e00eb2884dab7b201cf`
- application: `70af6dd5304e031a2f77c8c3a3d49e39f93d176495a1f2adb16bd17f12a0bf56`
- harness: `a3c830c3471c4c25966c1da74a3ef8b9f6cf76e2182af9c31d78f41f6517bb0c`
- oracle: `a1d7fba8c873f5fa845ff6fc78159a70611b68eea6fed1ee06b1dd61b96a1a03`
- disposable image: `5d6d2237fbeb7fda1d6e710a0d604bbb1493619aa4980cc0d80d9c8ceb993c22`
- source manifest: `346dfa34727a604bab262c1e77e0087c7da12b6c59ffe2aabd175b291914c245`

`evidence/q307-vkdemo-002/` に完全なJSON、6枚の実画像PNG、oracle診断、console/QMP/guest/rendererログを保存した。PNGはPPMのRGB byteを変えず可逆に形式変換したもの。元PPM/build/transferログは `plan/ws014/temp/remote/`。最終sourceは実測manifestの全hashと一致し、追加の同一試験は行っていない。

GitHubには計画・結果・API表とhashを掲載する。source・資料・画像ファイルのgit add/commit/pushはユーザーが行う。ローカルファイルをGitHubへ公開済みのリンクとは扱わない。

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
