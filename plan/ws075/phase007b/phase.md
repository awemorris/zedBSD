<!-- awesome-plan project=zedbsd record=ws075-p007b -->

# ws075-p007b: GL 3.2 の stage の実行器（render/）: 増分 b1〜b5

Status: in-progress（q833、P1。b3・b1・b2・b4 を実装と host 試験（b4 は 2026-10-09 P1、ベータ3 の合間の仕事）。次は b5（T1 の場面と依頼文、実機の 5330 の i915 が要る））
Disposition: normal
Parent: [WS075](../ws.md)
設計: [phase007/design.md](../phase007/design.md)（§5・§14 が優先）、増分の表は [phase007/phase.md](../phase007/phase.md)。

## 承認

Q1（2026-10-07）: 判断 1〜8 を既定どおりで承認、Phase の ID は p007a・p007b で可、b3 は p007a より先でよい。b3 の範囲の ACK（2026-10-07 夜）。

## 記録

- 2026-10-07 夜 増分 b3（layered の描画、p007a に依存しない）:
  - `render/gfx.h`: framebuffer に `layers`、clear_attachment の op に `base_layer`・`layer_count`。
  - `render/render-pass.c`: `VkFramebufferCreateInfo.layers` を持つ（0 は 1）。
  - `render/state.c`: `i915_state_target_range` は view の `layer_count` が 2 以上（3D の image でない）なら layer_count に（Surface Array、Depth と Render Target View Extent = layers − 1）。3DSTATE_DEPTH_BUFFER と 3DSTATE_STENCIL_BUFFER の Depth を「image の全 slice − 1」から「view の layer − 1」に、dword 7 に Render Target View Extent（bits 31:21）を足した（isl `isl_emit_depth_stencil.c` 147〜163、gen120.xml bits 245〜255、出典は code の注に sha256）。
  - `render/command.c`: pass begin の clear は attachment ごとに framebuffer の layers（view の layer 数で頭打ち）の各 layer を 1 layer の view で fill（本体を `i915_execute_clear_layer` に分けた）、vkCmdClearAttachments は VkClearRect の `baseArrayLayer`・`layerCount` を記録し layer ごとに fill（本体を `i915_execute_clear_rect` に、count 0 は 1 と読む）。
  - `render/instance.c`: `maxFramebufferLayers` 2048。
  - 試験（新）: `plan/ws075/tests/host-layered.c`・`run-host-layered.sh`（framebuffer の layers の decode と 0 → 1、2 layer の framebuffer で begin の clear が 2 layer・ClearAttachments の 2 layer の rectangle が 2 fill（各 layer の address と rectangle）、1 layer の framebuffer では begin が 1 layer、2 layer の色の target の surface state の Surface Array・Depth 1・RTVE 1、2 layer の depth view の 3DSTATE_DEPTH_BUFFER の Depth 1・RTVE 1・QPitch）。

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws075/tests/run-host-layered.sh` | plain・ASan/UBSan PASS、leak 0 |
| `sh plan/ws031/tests/run-vk-host-tests.sh "res resdispatch pipe cmdbuf"` | PASS（design §12 の 13 の cmdbuf の既存の失敗 `test_blend_state` は今の main では PASS、直す物は無い） |
| `sh plan/ws083/tests/run-host-video-roundtrip.sh` | PASS（command.c の変更の後も） |
| `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-k/vmunix` | 成功 warning 0 |

未実施: 実機（RTAI による layer の書き分け、design §12 の 10。b5 で T1）。libvulkan の wire が `layers`・`VkClearRect` の layer を運ぶことは codec の decoder（generic）と host の wire で確かめた（§12 の 11 の b3 の分）。

残り: b2・b4・b5（b1 は下）。

- 2026-10-07 増分 b1（pipeline の 3 stage、P1 の新しい世代、base main b009070c8 + P1 の 051f4bce8・66b1f037f）。再開の前に 10 個の host 試験を流し全て PASS（4 分）。
  - `render/gfx.h`: pipeline に `geometry`（module）と `gs_binary`。`render/pipeline.c`: `VK_SHADER_STAGE_GEOMETRY_BIT` を `pipeline->geometry` に。
  - `render/pipeline-prepare.c`: VS → GS（`drv_i915_shader_compile_stage(ir, vs_binary)`）→ FS の順。GS は compile の前に IR の `LOAD_VERTEX_INPUT` の location（Position・PointSize を除く）が VS の varying にあるかを確かめ、無ければ「the geometry shader reads location N, which the vertex shader does not write」と ENOTSUP。FS の入力は最後の stage（GS があれば GS）の varying と照合し、log は stage の名前を出す。GS の fit（新 `i915_pipeline_geometry_fits`）: code ≤ PS − GS（16 KiB）、push constant ≤ 一つの command buffer の block、push data ≤ 1 KiB（minor 8）、sampler 0。VS の上限は GS − VS（S7）。compile の refuse の log に GS の IR の事実（入力の頂点の数・最大の出力の頂点・topology）を足した。release は `gs_binary` も。`drv_i915_gfx_pipeline_kernels` は GS の field を写し、`varyings`・`vs_point_size`（S3）・`ps_input_slots` を最後の stage から。
  - 決め（p007a の再開の情報の b1 の項）: compiler の API は変えない。prepare で判る物（producer の location、push data、sampler、window の枠）は prepare で理由付きに検査。URB entry > 32 KiB など compiler の中の refuse は「refused by the compiler: error 95」と IR の事実（`the refused geometry shader: 1 vertices in, at most 256 vertices out (topology 1)`）の 2 行で、理由の文字列は compiler に持たせない（diagnostic の口を compile に足すのは後の候補）。
  - `render/heap.h`: window 64 KiB（`I915_GFX_INSTRUCTION_BYTES` 0x10000）、VS 0x0000、新 `I915_GFX_GS_KERNEL` 0x4000、PS 0x8000。kernel object は 32 × 64 KiB（+512 KiB、minor 4。instruction heap の clear も window の大きさで回る）。`render/state.h`: kernels に GS の field（code・bytes・grf start・push・vertices in・output topology・vertex/control の hword・control format・URB entry・PrimitiveID・layer・scratch）。
  - `render/draw.c`・`draw.h`: `drv_i915_gfx_window` に gs_code/gs_bytes（GS の枠に写す）。呼び出し（draw.c、compute.c、blit.c の 3 つ）を更新。**draw は GS のある pipeline を b2 まで refuse**（「draw refused: the geometry stage is not programmed yet」、ENOTSUP。3DSTATE_GS・URB を出さずに走らせない）。feature `geometryShader` は b4 まで出さない（今のまま FALSE）。
  - 試験（新）: `compiler-shaders/varyings.vert`（varyings.geom が読む 0・1・2 を書く VS、regenerate.py の GEOMETRY の表に足した。他の `.spv`・`.inc` は不変）。`plan/ws031/tests/i915-vk-pipe-test.c` の `test_geometry_pipeline`（wire で cells.vert + points.geom + passthrough.frag: geometry の module、3 つの kernel、GS の field（vertices in 1、triangle strip、varying 1、URB entry 5 × 64 B）、kernels の GS の field と最後の stage からの slot、destroy で leak 0）と `test_geometry_interfaces`（varyings.vert+varyings.geom+primitive-id.frag は gl_PrimitiveID を GS の VUE の slot 2 から、GS 無しの primitive-id.frag は refuse、GS の入力が VS に無い・FS の入力が GS に無い（VS にはある）・72 KiB の URB entry は refuse と log の行）。既存の 2 stage の pipeline の検査は不変（kernels の GS の field が 0 も足した）。

| コマンド（b1） | 結果 |
| --- | --- |
| `sh plan/ws031/tests/run-vk-host-tests.sh`（10 個、b1 の前と後） | 前・後とも plain・ASan/UBSan 全て PASS |
| `python3 src/drivers/gpu/i915/tests/render/compiler-shaders/regenerate.py` | 成功、既存の `.spv` と kernel の `.inc` は不変、`varyings.vert.spv` を追加 |
| `sh plan/ws075/tests/run-host-layered.sh`・`sh plan/ws083/tests/run-host-video-roundtrip.sh` | PASS |
| `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-k/vmunix` | 成功 warning 0（include check・vmunix check PASS） |
| `I915_TESTS=y I915_TEST_SET=vkx`・`vkc`・`vke1`・`vke2`・`compute` の vmunix（BUILD=build/p1-kt-SET） | 全て成功 warning 0 |
| `python3 plan/tools/style-check.py`（変えた file） | 増えない（pipeline-prepare.c は 1 → 0） |
| `git diff --check` | 問題無し |

- 範囲外（Q1 へ）: `plan/ws101/tests/host/compute-batch-test.c` の `drv_i915_gfx_window` の stub に gs_code/gs_bytes の 2 引数が要る（P1 の範囲外なので差分を Q1 に送った）。また `plan/ws101/tests/host/run.sh` は今の main で既に link に失敗する（executor の一覧に `render/video.c` が無い、WS083 の後）。
- 未実施: 実機（b5 で T1）。b1 だけでは GS の draw は refuse（b2 で 3DSTATE_GS）。


- 2026-10-07 増分 b2（draw の state、base main の ef625f03b の merge の後）:
  - `render/state.c`: `drv_i915_gfx_emit_urb(batch, vs_entry_size, gs_entry_size)`（int）: PUSH_CONSTANT_ALLOC を常に VS 8 KiB（0）・GS 8 KiB（8）・PS 16 KiB（16）（判断 2）。GS 有りは VS が chunk 4 から 21 chunk、GS が chunk 25 から 6 chunk（48 KiB）、entries = min(48 KiB / (size × 64), 1548)、size < 9 なら 8 の倍数、2 未満は ENOTSUP（何も出さない）。GS 無しは今の VS の分配のまま。`drv_i915_gfx_emit_constants` に GS の va・regs（CONSTANT_GS、push data は slot の 0x2c00 = 新 `I915_GFX_GS_PUSH_BUFFER`、`drv_i915_gfx_write_state` が書く）。新 `drv_i915_gfx_emit_geometry_shader`（§3.4: kernel 0x4000、Expected Vertex Count、scratch、dword 6 の grf（[3:0] と [5:4]）・Include Vertex Handles・read length 0・topology・vertex size、dword 7 の enable・Include Primitive ID・statistics・SIMD8（3）・control header、dword 8 の 335 threads と control format。GS 無しは 0 の packet）。CLIP dword 3 bit 5 Force Zero RTA Index（最後の stage が layer を書かなければ 1、GS 無しでも 1）、SF dword 2 の deref は GS 有りで PER_POLY。`drv_i915_gfx_topology` は GS のある pipeline だけ adjacency を 9〜12 に（GS 無しは今のまま 0 で refuse）、新 `drv_i915_gfx_topology_vertices`。
  - `intel/genxml.h`: `GEN12_3DPRIM_*_ADJ`（gen70.xml）、URB の分配の定数（`GEN12_URB_VS_START_CHUNK` 4、`_SPLIT_CHUNKS` 21、`GEN12_URB_GS_CHUNKS` 6、`_MIN_ENTRIES` 2、`_ENTRIES` 1548、intel_urb_config.c・intel_device_info.c）、`GEN12_CLIP_FORCE_ZERO_RTA_INDEX`（gen80.xml start 101）、`GEN12_GS_DISPATCH_MODE_SIMD8`（gen110.xml）。出典は file と sha256。`render/heap.h`: `I915_GFX_GS_PUSH_BUFFER` 0x2c00、`I915_GFX_MAX_GS_THREADS` 336（XXX: 1 target 固定）、heap の注の window の並びを b1 に合わせた。
  - `render/draw.c`: b1 の「the geometry stage is not programmed yet」の refuse を外し、`i915_draw_geometry_check`（S6: draw の topology の頂点の数 ≠ GS の入力の頂点の数は ENOTSUP と log、GS の scratch は b4 まで ENOTSUP）。URB の entry の不足は「draw refused」の log。3DSTATE_GS を `drv_i915_gfx_emit_geometry_shader` で、constants に GS。VS の URB entry は GS 有りなら VS の varying（新 kernels の `vs_varyings`）で測る（`varyings` は最後の stage の GS の物）。push の block の flush の判定と storage の判定に GS の block。`render/blit.c` は emit_urb(1, 0) と constants の新しい引数。
  - 試験: pipe fixture の `test_geometry_state`（3DSTATE_GS の dword 1・3・6〜9、points.geom の CUT の形、URB_ALLOC_VS・GS（start 25、76 entries）と 32 KiB の entry の ENOTSUP、GS 無しの URB、PUSH_CONSTANT_ALLOC 8/8/16、CONSTANT_GS、CLIP bit 5（layer を書く GS で 0、GS 無しで 1）、SF の deref、SBE・SWIZ の slot 2、adjacency の 9〜12 と頂点の数、GS 無しの GS packet が 0）、`test_geometry_interfaces` に `vs_varyings`。
  - GS 無しの batch の差分（受け入れ）: HEAD（b1、ef625f03b）と b2 の emitter（urb・constants・vs・gs・raster・pixel）を cuboid と cells+passthrough の 2 つの pipeline で dump して比べた（一回の比較、tree に入れない program、build/tmp）: 違いは PUSH_CONSTANT_ALLOC_VS（16 → 8 KiB）、PUSH_CONSTANT_ALLOC_GS（0 → 8 KiB at 8）、CLIP dword 3（0x0003ffc0 → 0x0003ffe0、bit 5）だけ。draw.c の順は不変（GS の packet は 0 のまま、CONSTANT_GS は regs 0 で今と同じ空）。

| コマンド（b2） | 結果 |
| --- | --- |
| `sh plan/ws031/tests/run-vk-host-tests.sh`（10 個） | plain・ASan/UBSan 全て PASS |
| `sh plan/ws075/tests/run-host-layered.sh`・`sh plan/ws083/tests/run-host-video-roundtrip.sh` | PASS |
| `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-k/vmunix` | 成功 warning 0 |
| `I915_TESTS=y I915_TEST_SET=vkx`・`vke2` の vmunix | 成功 warning 0 |
| style-check（変えた file）、`git diff --check` | 増えない、問題無し |

- 未実施: 実機（b5 で T1、GS の hang があれば engine reset で device lost）。draw.c の `i915_draw_geometry_check` と URB の refuse の log は host で通らない（draw.c は host の fixture に入らない、読んで確かめた）。

- 2026-10-09 増分 b4（P1 の新しい世代、base main 0e5e63cac。始める前に 10 個の host 試験を流し全て PASS）:
  - PrimitiveID（§5.4・§14 S4）: `render/pipeline-prepare.c` の fit の照合で、最後の stage が書かない gl_PrimitiveID（location 69）を除く。`drv_i915_gfx_pipeline_kernels` はその FS の入力に `kernels->ps_primitive_id_mask`（`state.h` の新しい field）の bit を立てる。`render/state.c` の `drv_i915_gfx_emit_pixel_shader` で、SBE_SWIZ のその attribute を `GEN12_SBE_SWIZ_PRIMITIVE_ID`（Constant Source PRIM_ID 3 << 9、Component Override X..W 0xF << 12 = 0xF600）にし、3DSTATE_SBE dword 1 に Primitive ID Override（attribute select bits 4:0、X..W bits 19:16、新しい `i915_state_primitive_id_override`）を足した（anv の `emit_3dstate_sbe()` と同じ。出典は `intel/genxml.h` の注に gen90.xml・gen60.xml・genX_pipeline.c の sha256）。
  - GS の scratch（§5.2 の scratch の行）: `render/draw.c` の scratch の buffer に geometry の部分（`I915_DRAW_SCRATCH_GEOMETRY`、thread id は `I915_GFX_GS_SCRATCH_IDS` = max_gs_threads 336、`heap.h`）を compute の部分の後に足した（`draw.h` の配列 3 → 4）。`kernels->gs_scratch_offset` を渡す。`i915_draw_geometry_check` の「not given scratch space yet」の refuse を外した。3DSTATE_GS の dword 4〜5 は b2 の emitter のまま（`i915_state_scratch`）。
  - log: GS が sample する時の refuse に理由の行（「the geometry shader samples N images, which the geometry stage cannot yet (no binding table)」）。GS の無い adjacency は今の「XXX unimplemented path: primitive topology N」のまま。
  - feature と limit（§5.5・S8）: `render/instance.c` で `geometryShader = VK_TRUE`、`maxGeometryShaderInvocations` 32・`maxGeometryInputComponents` 64・`maxGeometryOutputComponents` 64・`maxGeometryOutputVertices` 256・`maxGeometryTotalOutputComponents` 1024。`shaderTessellationAndGeometryPointSize` は FALSE のまま。libegl はこの feature を device に有効にするだけで、GS を使わない app の経路は変わらない（`userland/desktop/libegl/vulkan.c` を読んだ）。
  - 試験: pipe fixture の `test_geometry_interfaces`（GS の無い cells.vert + primitive-id.frag が refuse されず、mask 1、slot 0、SBE dword 1 の select 0 と bits 19:16 = 0xF、SWIZ = 0xf600）と `test_geometry_state`（spill する GS の dword 4 = offset | 1、dword 5 = 0。points.geom の kernels に 2 KiB を与えた形）。
  - 分かったこと: compiler の spill.geom（96 値）の code は 38560 byte で、GS の window の枠（16 KiB）に入らず、pipeline の fit で refuse される。spill する大きな GS は scratch の前に window の枠で止まる（GS の枠を広げるのは後の候補）。

| コマンド（b4） | 結果 |
| --- | --- |
| `sh plan/ws031/tests/run-vk-host-tests.sh`（10 個、b4 の前） | 全て PASS |
| `sh plan/ws031/tests/run-vk-host-tests.sh "pipe cmdbuf res resdispatch sync cmd"`（b4 の後、plain・ASan/UBSan） | PASS |
| `sh plan/ws075/tests/run-host-layered.sh` | PASS |
| `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/vmunix`（-Werror） | 成功 warning 0 |
| style-check（変えた file）、`git diff --check` | 数は前と同じ、問題無し |

- 未実施: 実機（b5 で T1、5330 の i915。ユーザーの規則で UAT 待ち）。draw.c の scratch の geometry の部分は host の fixture に入らない（読んで確かめた）。feature の reply の host 試験は無い。

### 再開の情報（2026-10-07、P1。b2 の後に更新、ユーザーの指示で q855 へ切り替え）

- 済み: b3・b1・b2。次: b4（design.md §5.4・§14 S4・S8、phase007/phase.md の表: PrimitiveID の SBE_SWIZ と SBE dword 1 の override、GS の scratch の stage（draw.c の `i915_draw_geometry_check` の scratch の refuse を外す）、sampler・adjacency（GS 無し）の log、feature `geometryShader` と `maxGeometry*`）、その後 b5（T1 の場面）。
- 再開の前に: main の今を merge、10 個の host 試験。

### 再開の情報（旧、2026-10-07、P1。b1 の後）

- 済み: b3・b1。次: b2（design.md §5.2、§14 S6、phase007/phase.md の表）。draw.c の「the geometry stage is not programmed yet」の refuse を b2 で外す。

### 再開の情報（旧、2026-10-07、P1。ws113-p011a の merge を受け、Q1 の指示で ws051-p004b の code を優先して中断）

- 済み: b3。base main 3b004a2c6 で `sh plan/ws031/tests/run-vk-host-tests.sh`（10 個）を流し plain・ASan/UBSan 全て PASS（3 分 49 秒）。
- b1 は code に未着手（変更無し）。次: b1（design.md §5.1・§14 S3・S7・minor 8、phase007/phase.md の増分の表）。p007a の再開の情報の b1 の項（prepare の GS の検査と compiler の refuse の理由の log の決め）もここで扱う。
- 再開の前に: main の今を merge、10 個の host 試験を一度流す。
