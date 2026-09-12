<!-- awesome-plan project=zedbsd record=ws014-p005 -->

# WS014 p005: テクスチャ付き回転直方体で3D shader/APIを検証

<!-- awesome-plan-current:start -->
Status: cleared
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Last Queue: q307 / q307-i01 cleared
Active Queue: none
Before: ws014-p004 planning
<!-- awesome-plan-current:end -->

Combined ID: ws014-p005
Primary Milestone: MG006

## ユーザー指示と単一の到達点

2026-09-13、ユーザーがp004の前に `userland/base/vkdemo` を作り、テクスチャ付き直方体を時間とともに回転させてvertex shaderとfragment shaderの実行、および実装APIの不足を確認するよう依頼した。新しいp005として扱い、順序をp003 → p005 → p004へ更新する。ユーザーは「GitHubは承認します」と明示した。

p003のclear/copyによる2帯検証を前提に、実際のgraphics pipeline、頂点入力、shader、テクスチャsample、depth、描画と回転を一つのデモで確認する。一般的なlibvulkan.soや全Vulkan適合を追加目標にしない。

## 実装・API方針

`userland/base/vkdemo/` を独立したamd64 packageとして作り、/bin/vkdemoへ配置する。初期画像は320x240程度、非等辺の直方体、独自GLSLのvertex/fragment shader、実際のVkImage/View/Samplerを用いたチェック模様texture、depth付きoffscreen描画とする。vertex shaderが時刻のpush constantから回転・透視変換を行い、fragment shaderが補間UVからtextureをsampleする。CPUが完成した直方体画像を描いて転送する方式ではない。

p003の通常GPU登録とcapset/blob/read/write/command/presentを再利用する。必要ならユーザー空間の有限Venus codec/session部分を共通化し、3D object/pipeline/frame状態はvkdemo内に置く。現時点では新K ioctlやHAL変更は不要と見込む。実利用で不足が確定した場合だけ現行U/K責務内のAPIを補い、同じ責務資料へ記録する。HALの全改変は別の具体的許可が必要であり、今回の計画を追加HAL変更の許可と解釈しない。

shaderは独自ソースをhostに既存のglslc2025.2-1/glslang15.1.0でSPIR-Vへcompileし、spirv-val2025.1で確認する。元shader、生成物、再生成手順とhashを保存する。上流Mesa等の実装をbaseへ移入しない。host toolsの利用と生成した自作shaderを区別する。

## 資源・連続実行

同一process/contextで複数frameを描画し、単調時計から時間を進める。診断用に有限duration/frame数と固定時刻sampleを指定できる。同じ描画関数を使い、固定時刻だけ別の実装へ切り替えない。必要なreply/upload/readback blob、scanout storage、pipeline/descriptor/textureは一度確保して再利用する。VkDeviceMemoryの非zero blob exportは一度だけ。32handle/session、64KiB転送、8MiB apertureの範囲を守り、VkFence完了後にcommand pool/fenceをresetして次frameを記録する。

## 受け入れと検証

1. host compiler/validator、対象version、追加wire commandとrenderer1.1.0の実dispatch条件を固定する。
2. vertexとfragment shaderを含むgraphics pipelineが成功し、直方体の複数面・正しいtexture/UV・depthが実際のGPU readbackと表示で確認できる。
3. 異なる固定時刻の複数frameについて、独立した幾何/texture期待値と実画面を比較する。面/texel境界の許容だけを明記し、GPU結果から作ったgoldenを正解にしない。clearだけ、無地面、未回転、誤UV、旧frameの偽陽性を防ぐ。
4. 同一process/contextで時刻とframeが進み、複数回のGPU readback/実画面が変化する。必要ならpresent後の有界capture待合せを診断インタフェースとして使う。通常の連続回転経路と同じrender関数を確認する。
5. QEMU制御とconsoleはQMP、実GL画像はegl-headless→VNC Unix RAWを使用。試行ごとにsource/shader/image/hash、frame/time、GPU完了、readback、実frame、rendererログを保存する。GPU readbackを表示画像と照合する。
6. make -j16対象build、C規約全文、必要な有限parser/ownership/画像判定の検査、差分レビューを行う。共通化でp003の経路を変えた場合に限って、その有効な限定回帰を再実行する。

## 実行境界

q307 / q307-i01はこのp005だけ。見積240 active minutes、120分ごとに成果と境界を点検する。各build・VM・pollを有限にし、同じ失敗の無変更再試行は3回以内に制限して原因へ進む。専用host awe@10.0.10.25と使い捨てimageを継続利用し、転送は既存のユーザー明示許可の範囲。既存VMやsystem package設定は変更しない。git add/commit/pushはユーザーが行う。aggregate make checkは使わない。p004・native i915・独立した新目標へQueueを広げない。

## 引き渡し

不足APIと実装済みshader/rendering subset、再現手順、画像・資源・versionの制限をp004へ渡す。p005の完了だけでWS014や全Vulkanを自動完了にしない。p001の未決定も維持する。

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
