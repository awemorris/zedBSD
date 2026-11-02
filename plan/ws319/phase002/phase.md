<!-- awesome-plan project=zedbsd record=ws319-p002 -->

# ws319-p002 H.264 の Video Player の GPU の道（VP_GPU_PRESENT）を i915（5330）で確かめて直す

- 親: [WS319](../ws.md)。
- Status: cleared（2026-11-02 P4 q1926-i01、Q1 の受け入れ待ち）
- 依存: WS083（i915 の H.264 decode）、WS318・WS322 の libmedia の GPU の絵（pictures the GPU holds）と Video Player の presenter。

## 目的

5330 の Video Player が H.264 を GPU の道（decode した絵を GPU のまま presenter が sample する）で再生することを確かめ、壊れていれば直す（2026-11-02 ユーザーの指示）。

## 見つけた事

最初の run（main 373e3480b そのまま）: hash は全部一致、1080p30 は late 1 だが、player の DRAW の行が gpu=0 で、GPU の道を使っていなかった（CPU の readback と CPU の拡大縮小、picture_us 12〜18 ms、1080p60 は 600 中 520 枚・late 80）。原因は 5 つ:

1. libmedia の `vkvideo_gpu_capabilities` が「decode の family と transfer の family が同じ」を求め、i915 は video の family が decode だけ（他の executor は 1 つの family）なので黙って readback に落ちた。GPU の絵の道は transfer の queue を使わない（images は 2 family の CONCURRENT）ので条件を外した（Q1 の承認、083b6189c5）。
2. i915 の NV12 の decode の image が SAMPLED と MUTABLE_FORMAT・EXTENDED_USAGE を拒み、video の format の能力も SAMPLED を出さなかった。
3. NV12 の plane の view（PLANE_0 の R8、PLANE_1 の R8G8）が無かった。Y-tiled の 2D の surface を plane の位置で作る。
4. libvulkan の memory の export が vkGetPhysicalDeviceProperties2（ID の properties）を使い、i915 に無かった。i915 の node に GPU_CAP_ALLOCATION_SHARE も無かった（share の export・import の ops は既に allocation を扱える）。
5. presenter の後始末の vkFreeDescriptorSets が i915 に無かった（opcode 78）。

## 結果（2026-11-02 P4）

| commit | 中身 |
| --- | --- |
| 083b6189c5 | libmedia `vkvideo-runtime.c`（GPU の絵の条件）、host 試験 `plan/ws319/tests/gpu-hold/`（6 checks、旧条件では FAIL） |
| 4686006ccc | i915: vk-image.c・vk-gfx.h・vk-state.c（sample できる NV12 と plane の view）、vk-video.c（format の能力）、vk-instance.c（Properties2 の ID）、session.h（ALLOCATION_SHARE）、vk-descriptor.c・vk-objects.c（FreeDescriptorSets）、`plan/ws319/tests/run-5330-video.sh`・`make-clips.sh` |

- host: `sh plan/ws319/tests/gpu-hold/run.sh build/tmp/gpu-hold` → gpu-hold: PASS。WS322 の `p004/run.sh`（全部 0 failed）、`p006/run.sh`（p006: PASS (147 streams)）。
- 5330（`PARTS=h264 sh plan/ws319/tests/run-5330-video.sh build/tmp/p319-run5 build/p4 build/tmp/p319-clips build/tmp/p319-hevc`、NVMe の kernel を差し替え、終わりに Debian へ戻し kernel cksum 1363937226・zedbsd.cfg 2629068843 を戻した）:

| 項目 | 前（run1） | 後（run5） |
| --- | --- | --- |
| media-probe hash 1080p30・60 | 600/600・600/600 | 600/600・600/600 |
| player の GPU の絵 | gpu=0（readback） | gpu=60〜63 /s、「pictures the GPU holds, 14 slots」 |
| 1080p30 16 s | shown 480、late 1 | shown 481、late 0 |
| 1080p60 10 s | shown 520 / 600、late 80 | shown 581 / 600、late 19 |
| compositor の誤り、kernel の refused | 0、0 | 0、0 |

- 画面: `build/tmp/p319-run4/h264-1080p60.png`（GPU の絵を plane の view で sample した絵、正しい）。

## 未実施・残り

- 1080p60 の残りの late 19（swapchain の acquire の待ち 12〜13 ms/frame が主）。調べていない。
- run の compositor は test の素の compositor（navy の背景、上の bar 無し）で、session の desktop ではない。
