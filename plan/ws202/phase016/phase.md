<!-- awesome-plan project=zedbsd record=ws202-p016 -->

# ws202-p016: Vulkan標準APIによるdecode NV12の読み戻し

Status: uncleared
Disposition: normal
Parent: [WS202](../ws.md)
Queue: [codex-ws202-20261010-readback i03](../policy-20261010.md#有限実行-codex-ws202-20261010-readback)（software部分）

## 目的とscope

libmediaからGPU固有のtiling解釈を除き、Vulkan標準のimage-to-bufferでlinear NV12を読めるようにする。2026-10-10ユーザー回答「i915／必要なlibvulkanの補完も含める（推奨）」によりこのWSで実装する。具体的理由/affected sourceは[方針記録](../policy-20261010.md#gpuを抽象化するための依存変更)。

## criteria

- profile/format queryでdecode出力のTRANSFER_SRCを正しく報告し、NV12 imageの作成を受け入れる。
- PLANE_0とPLANE_1のcopyを標準VkBufferImageCopyのsample単位で扱い、offset/stride/region/admission/寿命を確認。Intel Tile Yの知識はdriver内だけ。
- decodeからhostまでの同期/cacheを保証し、失敗時に未退役DMAの資源を壊さない。
- named build warning0と対象host、5330の実decode出力hash。hostと実機の証拠を分ける。

## 依存・残り

WS083の基点sourceにある実装出力、承認済み補完の実装、p009/p010の標準読戻し設計。HAL/UAPIを変える必要が判明したら既存の承認手順へ。software実装/host/buildは確認済み。実decode hashは実機確認待ち。p009のportable完成はこの出力なしには判定しない。

## 2026-10-10 承認と実装開始

上記質問回答を具体scope承認としてi03を開始。既存VCS0 decodeはcommand実行中に同期退役する。読み戻しは別のgraphics/transfer queueで行い、video queueに未承認のgraphics operationを混在させない。driver内copyの同期/cacheとNV12 planeの範囲を確認する。whole criteriaの実機hashは未達。

## i03 software部分の結果（2026-10-10）

Outcome: cleared（部分scope）。whole Phaseはuncleared、実機のdecode/readback hash待ち。i915のNV12 image/format/feature queryとlibvulkanのvideo format usage admissionへTRANSFER_SRCを追加。PLANE_0をR8、PLANE_1をR8G8のY-tiled surfaceとして既存GPU rectangleへ渡す。座標は各planeのsample単位。generic image operations、NV12 uploadやsamplingの対応は追加していない。image copyの既存wireはplane/offset/extent/strideを保持するためprotocol変更不要。

変更source: i915 `render/command.c`・`image.c`・`instance.c`・`video.c`・`gfx.h`、libvulkan `video.c`。NV12 planeのextent・aspect・mip/layer/depth・offset・usage・destination packingを検証し、bufferOffsetの加算による範囲判定のwrapを避けた。画像/バッファの失効には既存forget処理が同じcopy opを扱う。

同期の静的証拠: `video-mfx.c`のdecode末尾MI_FLUSH_DWと`video.c`のi915_video_runがVCS0 batchを同期退役。graphics queueでplane copyを既存RCS batchへ記録し、submit_endが同期退役した後にfence/reply。MOCS/cacheと失敗時quarantineは既存GPU run/workerを維持。新たなCPU cache aliasアクセスや未退役資源の解放を追加していない。これはsource/hostの証拠でありhardware DMA/画素の証拠ではない。

実command（own worktree）:

- `sh plan/ws202/tests/run-host-nv12-readback.sh`: 通常+ASan/UBSan PASS。既存WS031 copy/commandとWS083 video fixtureを再利用、追加NV12 cropped Y/CbCr・linear stride/offset、combined aspect/plane超過/offset wrap/pitch超過/逆向きcopy拒否、standard video queryのTRANSFER_SRC転送とTRANSFER_DST拒否を確認。GPU stand-inはsurface/commandを確認、画素を生成しない。最初のsandbox実行は通常PASS後LeakSanitizerがptraceで起動不可。許可された通常環境で同じrunnerを再実行し両mode PASS。最終出力: `build/tmp/ws202-nv12-readback.run.xgIy0v/`（local temporary）。
- `make ZEDBSD_CONFIG=config/ci/config-amd64.mk -j4 build/amd64/kern64/src/drivers/gpu/i915/render/command.o build/amd64/kern64/src/drivers/gpu/i915/render/image.o build/amd64/kern64/src/drivers/gpu/i915/render/instance.o build/amd64/kern64/src/drivers/gpu/i915/render/video.o`: exit0、warning/error0（`build/i915-readback-build.log`）。shared LLVMはread-only symlinkで利用。
- `make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j4 build/amd64/dynamic/libvulkan.so`: exit0、warning/error0、link/standard exports/ELF dependency check PASS（`build/libvulkan-build.log`）。共有sysrootはread-only参照し、その完成stampをremakeしない。shared Noct/LLVM/sysrootのbuild/install無し。
- `clang-format-19` edited range/new C、関数引数のtabと一行prototypeを全文規約どおり維持。19.1.7。新C/tableの `python3 plan/tools/style-check.py ... --summary`: total0。変更した既存5 Cは48候補、基点の同5 Cも48（new helper/差分で増加無し）。全文§1〜14で差分を手動確認。WS全体のp014 clearanceとは別。
- `git diff --check`: PASS。compiler: clang23.1.0（project）、GCC14.2.0（host）。libvulkan.so SHA-256 `ad3124f928dd240495687eeae7490a0f1c8fef77c1a2011b87ce03c7c7ac6cec`。

残件: Q1の統合、p009 native Vulkan runtimeから標準copyを呼ぶ実装、5330実decode後のlinear NV12 hash照合。physical acceptanceが届いた時に同Phaseのwholeを再評価。QEMU/実機は実装sessionで起動しない。Master/共有Queue/Guardrail/他WS/GitHubはQ1の投影待ち。
