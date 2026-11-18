# i915 render tests: the shader test suite

The GPU scenarios of this directory drive the Vulkan executor (`../../vk/`)
and its SPIR-V compiler (`../../compiler/`) on the hardware once the node is
served.  They are linked only into the test build (`I915_TESTS=y`) and are
started by the runner of `../execution/` (see its README for how a scenario
runs).  Each one starts a thread that waits for the node, opens a session of its
own, runs its steps, logs `<SUITE>-<STEP> PASS` or `FAIL` per step and a
`verdict` line, and closes the session.

    flock /tmp/i915-hw.lock env BUILD=build/<dir> vkloop-hw.sh test vkc
    flock /tmp/i915-hw.lock env BUILD=build/<dir> vkloop-hw.sh test vke1
    flock /tmp/i915-hw.lock env BUILD=build/<dir> vkloop-hw.sh test vke2
    flock /tmp/i915-hw.lock env BUILD=build/<dir> vkloop-hw.sh test vkx
    flock /tmp/i915-hw.lock env BUILD=build/<dir> vkloop-hw.sh test vkcs

No test kernel holds every scenario (the kernel without tests is within about 450 KiB of
the 16 MiB the loaders accept for an image), so each scenario's image links the runner and the scenarios of one test set
(`I915_TEST_SET`, `platform/amd64/vmunix.mk`): `vkx`, `vkc`, `vke1` and `vke2` one each, `compute` for `vkcs`,
`execution` for ktest eu draw r1 tex t3 bl, `display` for the LCD/HDMI scenarios, `display_ktest` and
`display_ktest2` for the two halves of display_ktest.  `vkloop-hw.sh` picks the set from the scenario (and the
image's config, `config-test-hw.mk`, whose kernel console reaches the serial log the verdict lines
are read from) unless `I915_TEST_SET` or `ZEDBSD_CONFIG` is given.  `vke2`'s set has the compiler's boundary steps
(EDGE, KILLOOP, NOINPUT and, last, UNDEF; `-DI915_VKE2_BOUNDARY`).
`run-hw.sh` builds each scenario's image in its set outside the lock and runs it.

## Suites

| Scenario | Source | Shaders | Target | How a step is judged |
| --- | --- | --- | --- | --- |
| `vkx` | `executor.c` | `shaders/` | 64x64 RGBA8 | the executor's commands (indexed draws, viewport/scissor, buffer copies, long command buffers, push constants, mip chains and blits); images and bytes computed in C |
| `vkc` | `compiler.c` | `compiler-shaders/` (+ mview's `mview.vert`) | 64x64 R32G32B32A32_SFLOAT | every component of every pixel inside the interval `regenerate.py` computed (exact where the operation is exact, otherwise the precision Vulkan requires); the draws go straight to `drv_i915_gfx_draw`, no wire |
| `vke1` | `features.c` | `feature-shaders/` | 64x64 RGBA8 | the pixels `regenerate.py` computed from the Vulkan blend equation and the uniform data, each byte within 1 (2 for the textures) |
| `vke2` | `generality.c` | `generality-shaders/` | 64x64 RGBA8 | every fragment shader writes the four bytes of one 32-bit word per pixel (an integer, or the bits of a float); the word must equal the one `regenerate.py` computed (bit for bit, or within 4 ulps for the float step) |
| `vkcs` | `compute.c` | `compute-shaders/` | storage buffers (64x64 RGBA8 for MIXED) | compute pipelines made through the wire and dispatched, shared memory and workgroup barriers: every word a dispatch writes equals the one the scenario computes in integers (the atomics: every result that does not depend on the order); MIXED also the target's pixels, each byte within 1 |

`readback.c` is not a suite: it is the draw checkpoint of the test build that
dumps the first draws' target for the vkdemo oracle.

## Which scenario covers which feature

| Feature (SPIR-V / GLSL) | Scenario and step |
| --- | --- |
| vertex attributes, push constants in the vertex stage, one varying | `vkx` (all), `vkc` cells |
| push constants in the fragment stage (offset 112) | `vkx` GRID, `vkc` mview, `vke2` MATRIX |
| `texture()` with nearest / linear / mip filtering, LOD bias and range, views of later levels | `vkx` MIP-NEAREST, MIP-LINEAR |
| several combined image samplers over two sets | `vke1` TEX3 |
| GLSL.std.450 abs floor fract sqrt exp2 log2 pow inversesqrt min max clamp mix normalize | `vkc` unary, exponent, minmax, vsmath |
| `OpFDiv` | `vkc` divide |
| float comparisons `FOrd*` / `FUnord*` (NaN), `&&` `||` `!` `?:`, `OpSelect` | `vkc` compare |
| structured selection (if / else, nested, diverging in a SIMD8 dispatch) | `vkc` branch, `vke2` LOOP |
| `OpKill` (discard) | `vkc` discard |
| colour blending: factors, ops, independent alpha, write masks, blend constants | `vke1` BLEND |
| uniform buffers (std140 vec4, arrays, mat4 read as columns), dynamic uniform buffers and offsets | `vke1` UBO |
| `OpTypeMatrix`, matrix x vector / vector x matrix / matrix x matrix / matrix x scalar, `OpTranspose`, `OpOuterProduct`, ColMajor / RowMajor and MatrixStride in blocks and push constants, local matrices | `vke2` MATRIX |
| integer arithmetic (wrapping add/sub/mul, SDiv/SMod/SRem/UDiv/UMod in the math box), shifts, bitwise, SAbs/SSign/SMin/SMax/UMin/UMax/SClamp/UClamp, the 10 integer comparisons, conversions between floats and integers | `vke2` INT |
| `OpFMod` / `OpFRem`, RoundEven / Round, Trunc, Ceil, FSign, Step, SmoothStep, Floor, Fract, FAbs | `vke2` FLOAT |
| `OpLoopMerge` loops with per-pixel trip counts, `continue`, `break`, nesting, do-while, `while (true)` | `vke2` LOOP |
| 16 varyings, a fragment shader reading some of them in any order | `vke2` VARY16, SUBSET |
| 16 vertex attributes | `vke2` VIN16 |
| register spilling to scratch memory (a loop's variables spilled while its channels diverge) | `vke2` SPILL |
| 16 vertex attributes and 16 varyings together (gathered VUE and spilling in the vertex stage) | `vke2` VIO16 |
| integer boundaries (INT_MIN, -1, 0, the 8/16/32-bit limits) in division, modulus, shifts by 0..31, wrapping arithmetic, min/max, comparisons and conversions; undefined divisions write a marker | `vke2` EDGE |
| `OpKill` inside a loop with per-pixel trip counts, whole dispatches discarded | `vke2` KILLOOP |
| a fragment shader with no input behind a vertex shader with no varying (3DSTATE_SBE with no attribute) | `vke2` NOINPUT |
| division by zero, INT_MIN / -1, shifts by 32..63 (undefined values: the draw must finish and the guard rows be right; the values are logged) | `vke2` UNDEF |
| mview's own shaders (per-vertex and, with `--shading=pixel`, per-pixel lighting with a uniform buffer, a loop over three lights and discard) | the capture run `CAPTURE=mview vkloop-hw.sh mview` (six viewer checks), not a suite of this directory |

## How the expectations are produced

Every shader directory holds the GLSL sources, the SPIR-V compiled from them
(`<name>.spv`) and a `regenerate.py` that does both halves of a test's data:

1. compiles each source with `glslc --target-env=vulkan1.1 --target-spv=spv1.0
   -O0` and validates it with `spirv-val`;
2. computes what the GPU must produce -- in Python, from the SPIR-V and Vulkan
   definitions, never by running the i915 compiler: integers modulo 2^32,
   floats as IEEE-754 single-precision operations in the order the compiler
   lowers them (`vke2`), or intervals as wide as Vulkan's precision rules
   allow (`vkc`);
3. writes `../fixtures/<suite>-shaders-gen.inc`: the SPIR-V words, the inputs
   (uniform blocks, push constants, vertices, texels) and the expected pixels
   or intervals, as C arrays and macros only.

The scenario includes the generated file; so do the host fixtures (`i915-vk-lower-test.c` runs the IR in an interpreter,
`i915-vk-compile-test.c` runs the EU words in an instruction-semantics model,
both against the same expectations; `run-vk-gentool-test.sh` has Mesa's
assembler and disassembler judge every kernel).  A test is therefore checked
three times against one set of numbers: the IR, the EU model and the GPU.

To change a shader, edit the GLSL and run its generator from the tree's root:

    python3 src/drivers/gpu/i915/tests/render/compiler-shaders/regenerate.py
    python3 src/drivers/gpu/i915/tests/render/feature-shaders/regenerate.py
    python3 src/drivers/gpu/i915/tests/render/generality-shaders/regenerate.py
    python3 src/drivers/gpu/i915/tests/render/compute-shaders/regenerate.py   (SPIR-V only: vkcs computes its expectations in C)
    python3 src/drivers/gpu/i915/tests/render/shaders/regenerate.py

then `run-vk-host-tests.sh` and
`BRW_TOOLS=<mesa build>/src/intel/compiler run-vk-gentool-test.sh`
before a hardware run.  The generated header records the glslc version and
the SHA-256 of every source; mview's shaders have their own generator and
`shaders/provenance.json` under `userland/tests/mview/shaders/`.

## Known unsupported features

The compiler refuses what it cannot lower (ENOTSUP, and the pipeline is not
created) rather than approximating it.  Not covered, and refused where noted:

- integer varyings (`Flat`, refused) and integer vertex attributes (inputs are
  float), 16- and 64-bit types;
- a local first stored inside a loop and read after it (refused), matrices
  through `OpPhi`, local arrays and structures, a dynamic index into a local
  vector (`OpVectorExtractDynamic`, dynamic access chains), `OpSwitch`,
  function calls, `return` inside a loop (refused);
- loops nested more than 16 deep (refused); a loop that does not end hangs the
  GPU (not detected); a channel with a trip count of 0 passes the body once,
  predicated off;
- uniform buffers are delivered as push data: at most 1 KiB of push data a
  stage and 8 blocks, dynamic indices up to 16 elements; no descriptor arrays;
  no sampler in the vertex stage;
- one colour attachment; no logic op, no dual-source blending;
- spilling: a value spilled is written on every definition and read before
  every use (no rematerialization of constants, no spill-cost weighting); a
  kernel's scratch space is at most 2 MiB a thread, and the session's scratch
  buffer (both stages' parts, 546 vertex and 1024-per-slice pixel thread ids)
  at most 16 MiB (one object), i.e. up to 8 KiB a thread for each stage;
- the software scoreboard serializes every instruction (no dual issue);
- the hardware values of undefined operations (division by 0, INT_MIN / -1,
  shifts by 32 or more) are not checked.
