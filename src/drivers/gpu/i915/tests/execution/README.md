# i915 kernel execution tests

The in-kernel tests of the i915 driver: the unit test suite (`ktest`) and the
GPU execution tests (compute, draw, texture) on the started GT.  They are
linked only into the test build (`I915_TESTS=y` in `platform/amd64/vmunix.mk`);
the production kernel does not contain them.

## How a test runs

`device.c` calls the weak checkpoint `drv_i915_test_after_start(device)` once,
on the start worker, after the device has started (GT, interrupts, display)
and right before the node is published and served.  `runner.c` defines it in
the test build: it selects one scenario by the compile-time name
`I915_TEST_SCENARIO` (only `tests/` reads it), runs it, logs one line
`i915: test <name>: PASS` / `FAIL rc=<errno>` (display scenarios log their own
verdict and the runner logs `end`), and returns, so the node is served as in
production.  The GPU scenarios therefore own the render engine while they run;
they leave it parked on its kernel context, put their fixture pages back to
scratch, invalidate the GT TLBs and free their objects, so the served node
(for example vkdemo in the vkloop image) runs normally afterwards.  After a
hang the engines are reset and the objects are kept.

On the hardware (the machine is shared; always take the lock):

    flock /tmp/i915-hw.lock vkloop-hw.sh test ktest
    flock /tmp/i915-hw.lock vkloop-hw.sh test eu

| Scenario | Function | What it checks |
| --- | --- | --- |
| `ktest` | `drv_i915_test_execution_ktest` | the unit test suite (below); PASS when no check failed |
| `eu` | `drv_i915_test_execution_eu` | multicast workaround probe, compute (GPGPU_WALKER, one SIMD8 thread, A64 store), then 3 more requests on the same context and 2 on a new one |
| `draw` | `drv_i915_test_execution_draw` | the single-colour RECTLIST draw, 1024 pixels |
| `r1` | `drv_i915_test_execution_r1` | draw repeats and compute/3D switching on one and on new contexts (12 steps) |
| `tex` | `drv_i915_test_execution_tex` | the first textured draw, 1024 pixels, texture untouched |
| `t3` | `drv_i915_test_execution_t3` | texture update, binding switch, redraw on the same and a new context (9 steps) |
| `bl` | `drv_i915_test_execution_bl` | bilinear filtering, exact comparison (4 steps) |
| `vkx` | `drv_i915_test_render_executor` (`../render/executor.c`) | the Vulkan executor once the node is served: a thread opens a session and records command buffers through the wire -- 16/32-bit indexed draws (bind offset, first index, vertex offset, instances), dynamic viewport and scissor, `vkCmdCopyBuffer` (byte-exact), a 134-operation command buffer whose fragment shader reads the pushed colour at byte 112; a 64x64 texture of 7 levels filled level by level with distinct colours and sampled minified (nearest level at 32/16/8 pixels, LOD held at 1.5 with linear mip blending, LOD bias 2, a view of level 2 on), and a mip chain made by linear `vkCmdBlitImage` from level to level and checked as the box filter of the level above; each step `VKX-<name> PASS/FAIL`, then `vkx: verdict` (the runner logs `end` before it) |
| `vkc` | `drv_i915_test_render_compiler` (`../render/compiler.c`) | the shader compiler on the GPU once the node is served: each step compiles a shader pair of `../render/compiler-shaders/` (and mview's shipped vertex shader), draws straight through `drv_i915_gfx_draw` into a 64x64 R32G32B32A32_SFLOAT target the CPU cleared, and checks every component of every pixel against the bounds `regenerate.py` computed (exact, or the precision Vulkan requires): GLSL.std.450 functions over 256 inputs, division, the comparisons and selections (NaN included), vertex-stage normalize/max/clamp, mview.vert with its push constants, nested if/else diverging inside a SIMD8 dispatch, discard (discarded pixels keep the clear value); each step `VKC-<name> PASS/FAIL`, then `vkc: verdict` |
| `vke1` | `drv_i915_test_render_features` (`../render/features.c`) | colour blending, uniform buffers and several sampled images through the wire once the node is served: BLEND (16 pipelines over a target cleared to a known colour -- factors ZERO/ONE/SRC/DST colour and alpha/CONSTANT, ops ADD/SUBTRACT/REVERSE_SUBTRACT/MIN/MAX, independent alpha, write masks, pipeline and `vkCmdSetBlendConstants` constants, each cell within 1 of the Vulkan blend equation), UBO (descriptors by `vkUpdateDescriptorSets`; a vertex shader placing its quad by a std140 mat4 read as columns plus an offset, a fragment shader colouring from a vec4, an array element and a mat4 column; the second draw through a dynamic uniform buffer and its dynamic offset), TEX3 (3 combined image samplers at set 0 bindings 0/2 and set 1 binding 1, linear and nearest samplers, both sets in one bind); each step `VKE1-<name> PASS/FAIL`, then `vke1: verdict` |
| `vke2` | `drv_i915_test_render_generality` (`../render/generality.c`) | the compiler's generality (p014 E2) through the wire once the node is served; every fragment shader writes one 32-bit word per pixel of a 64x64 RGBA8 target, checked against `../render/generality-shaders/regenerate.py`: MATRIX (a vertex chain transpose(row-major) * column-major of a uniform block; fragment products of a std140 block (column-/row-major, mat3) and of push constants (row-/column-major), a local matrix written by column, transpose, outer product, matrix * scalar, vector * matrix), INT (16 operations over hashed 32-bit values with negatives: add/sub/mul wrap, SDiv/SMod/UDiv/UMod by powers of two and not, shifts, bitwise, abs/sign/min/max/clamp signed and unsigned, 10 comparisons, conversions), FLOAT (mod by +/- divisors, roundEven/round, trunc, ceil, sign, step, floor, smoothstep, fract, abs; 4 ulps), LOOP (per-pixel trip counts: for to x % 17 with continue/break, for around while with if/else and break, do-while, while (true)), VARY16 (16 varyings), SUBSET (5 of 16 read), VIN16 (16 vertex attributes), SPILL (a fragment shader keeping 96 values live, over the GRF, with a per-pixel loop among them: spilled to scratch memory, 2 KiB a thread, the draw's scratch buffer as the general state base), VIO16 (a vertex shader with 16 attributes and 16 varyings: the VUE gathered at the end, spilling, read back by vary16.frag); each step `VKE2-<name> PASS/FAIL`, then `vke2: verdict`; the shader suite's index is `../render/README.md` |
| `lcdb` ... `display_ktest` | `drv_i915_test_display_<name>` | the display scenarios of `tests/display/` (weak in the runner's table until linked) |

On a Gen9 GT `draw` builds the Gen9 form of its fixture (Gen9 packets, a Gen9 pixel shader assembled from
`draw-fixture-ps-gen9.asm`).  `r1`, `tex`, `t3` and `bl` refuse the GT with a log line and
return `I915_TEST_SKIPPED` (`scenarios.h`), which the runner logs as `SKIP`: their shaders, states and compute batch
were made for Gen12 only.

## Files

- `runner.c` — the checkpoint and the scenario table.
- `ktest.c`, `ktest.h` — the tally and the order of the unit test parts.
- `ktest-sync.c` — completions, work queues, MSI vectors, write-combining, reset, PCODE, DRAM/bandwidth, runtime PM, sleep-range, preemption, late fuse.
- `ktest-gt.c` — GT fuses and interrupts, workarounds/MOCS/PAT/RC6/RPS, GT memory, execlists, contexts, requests, defaults, PXP, HPD setup, the draw/texture fixtures and the page-table walk.
- `eu-test.c`, `eu-internal.h`, `eu-test.h` — the compute test and the request path all GPU tests share.
- `draw-test.c` — draw, R1, texture, T3 and bilinear tests.
- `fhd-render.c`, `fhd-render.h` — the full-HD textured draw into a caller's buffer (used by the display scenarios).
- `ppgtt-walk.c` — a read-only walk of the GT address space, as the GPU does it.
- `firmware-override.c`, `firmware-override.h` — the test build's answer to the firmware provider's weak request checkpoint (an image it serves stays the test's memory; the release does not free it).
- `../fixtures/` — the draw/texture fixtures (`draw-fixture.c`, the generated `tex-fixture*-gen.inc`) and `vkref-generated.inc`.
- `../render/compiler.c` — the compiler scenario `vkc`; its shaders are `../render/compiler-shaders/*.{vert,frag}`, compiled by `../render/compiler-shaders/regenerate.py` into `.spv` (read by the host fixtures `i915-vk-lower-test.c` and `i915-vk-compile-test.c`) and `../fixtures/compiler-shaders-gen.inc` (the SPIR-V, mview's shipped vertex shader, the cell inputs and the expected output bounds).
- `../render/features.c` — the feature scenario `vke1`; its shaders are `../render/feature-shaders/*.{vert,frag}`, compiled by `../render/feature-shaders/regenerate.py` into `.spv` (read by the host fixtures `i915-vk-lower-test.c` and `i915-vk-compile-test.c`) and `../fixtures/feature-shaders-gen.inc` (the SPIR-V, the blend cases with their expected pixels, the uniform blocks, the texels and the expected images).
- `../render/generality.c` — the generality scenario `vke2`; its shaders are `../render/generality-shaders/*.{vert,frag}`, compiled by `../render/generality-shaders/regenerate.py` into `.spv` and `../fixtures/generality-shaders-gen.inc` (the SPIR-V, the uniform blocks and push constants, the vertex data and the expected words), which the host fixtures `i915-vk-lower-test.c`, `i915-vk-compile-test.c` and `i915-vk-pipe-test.c` include as well.
- `../render/executor.c`, `../render/scenarios.h` — the executor scenario `vkx`; its shaders are `../render/shaders/*.{vert,frag}`, compiled by `../render/shaders/regenerate.py` into `.spv` (read by the host fixture `i915-vk-pipe-test.c`) and `../fixtures/executor-shaders-gen.inc`.

## Dropped tests

Tests of retired code, of test hooks that production no longer has, and of
paths that cannot be exercised while the real device runs.  The unit suite
keeps one check per old check otherwise, so the counts stay comparable.

### Not ported (other owners)

- The display unit tests that lived in their own files (eDP, eDP sync, LCD
  modeset, scanout, LCD show, LCD-G, OpRegion, HPD) are run by the display
  scenario `display_ktest` (`tests/display/`), not by `ktest`.

### ktest-sync.c

| Old test | Reason |
| --- | --- |
| K5 `complete_all` (2 checks) | the new `sync.h` has no complete-all; nothing in the driver uses it |
| Time-source fault handling (4 checks) | the hook that swapped in a fake time source was retired; a fault would also latch the driver-wide time-base fault (`sync.c`) of the live device |
| `skl_pcode_request` time-base anomaly (1) | same |
| PCODE preemption-off region, part (b): counter fault inside the region (1) | same; part (a) runs on the real clock |
| Timer next-event / tick calculation (8) | the calculation (`timer_calc`) is retired with the diagnostic runner |
| IRQ-context completion one-shot (4, skipped at run time) | `kern_diag_oneshot_arm` was a kernel hook built only with the removed `CONFIG_DRIVER_PCI_I915_PARITY`; the hook was deleted with the option |
| Real preemption A/B (1, skipped at run time) | same one-shot hook |

### Execution tests and fixtures

| Old | Reason |
| --- | --- |
| `selftest.c`: `drv_i915_selftest`, clear, RCS, RT, compute and draw selftests, golden run, power probe, statistics read, engine WA application | legacy-hardware selftests of the retired legacy engine/GGTT/LRC code; the draw/texture fixtures they carried moved to `tests/fixtures/draw-fixture.c` byte-for-byte |
| `probe.c` `PARITY_*_TEST` branches | replaced by the runner's scenarios `eu`, `draw`, `r1`, `tex`, `t3`, `bl` |
| Reference-kernel build (`I915_VK_REFERENCE_KERNELS`, `vkref-generated.inc`) and gfx census | not provided: the render path has only the after-draw checkpoint (`drv_i915_gfx_draw_checkpoint`); substituting reference kernels changes the kernels, the URB entry size and the 3DSTATE_VS/SBE/WM/PS words at several places in `vk/`, which needs new production seams.  The generated table is kept in `tests/fixtures/vkref-generated.inc` |

### ktest-gt.c

| Old test | Reason |
| --- | --- |
| P6B-RC6, second check (1) | the `skip_rc6` test switch is gone from production |
| P6C3B-PARSE (1) | `gen12_csb_parse` is static in `submit.c`; the same property is covered by P6C3B-CSB through the public CSB processing |
| P6C4B-SUBMIT, WAIT, PARK x2, DEFAULT (5) | the default-context submit/poll/finish helpers are static in `defaults.c`; INIT, INHERIT (with an image the test builds) and WEDGE remain |
| P7-IPC (1) | `i915_skl_watermark_ipc_init` is static in `display/display.c` |
| SSEU, CLOCK, FAULT, P6C2-OFFSETS/SIZE/RPCS | kept, but observed through the public API (`drv_i915_gt_init_mmio`, the laid-out context image, `drv_i915_lrc_update_regs`) instead of the production statics |
