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
