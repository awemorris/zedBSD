# vkdemo

The original zedBSD demo renders a textured, rotating rectangular cuboid through
Venus Vulkan commands. It executes its own vertex and fragment SPIR-V modules,
uses a sampled image and depth attachment, waits for a Vulkan fence, reads the
actual color attachment back, and presents those same bytes through the existing
GPU packed-pixel presentation interface.

The program implements the finite Vulkan graphics operations needed by this
scene. Its common wire codec and instance/device/queue bootstrap live in
[the shared client](../../gpu/venus/client.h). The application owns its graphics
resources and frame lifecycle. The finite API is not a conformant Vulkan loader
or a general-purpose Vulkan implementation.

## Build and ordinary use

The optional amd64 base package is vkdemo; select it in the image configuration.
Its normal build consumes the checked-in original shader arrays and requires
neither a shader compiler nor external runtime libraries.

    /bin/vkdemo
    /bin/vkdemo --duration=30 --device=/dev/gpu0
    /bin/vkdemo --duration=0
    /bin/vkdemo --time-ms=1000 --hold=20

Ordinary animation uses CLOCK_MONOTONIC, starting after resource setup, and
runs for ten seconds by default. --duration=0 requests continuous animation;
positive durations accept 1–3600 seconds. Rendering and readback determine the
frame rate, with a short cooperative pause after each completed frame.
--time-ms=0..3600000 renders one explicit shader time and retains it for
--hold=0..120 seconds (default ten).

Each completed frame emits a VKDEMO PRESENT line to standard output. This is
also the supported capture interface; continuous use produces continuous frame
logs, which may be redirected by an ordinary shell. Frame counters advance only
after real Vulkan completion, readback, and successful presentation. Exit
completion is reported only after resource cleanup and device close.

## Exact scene contract

The output is 320×240, RGBA8 UNORM, one sample per pixel. Clear RGB is
(16,24,40) with alpha 255. Depth is D32_SFLOAT, cleared to 1, compared with
LESS, with depth writes enabled. Culling and blending are disabled.

The original cuboid has half extents (0.75, 0.5, 0.375). Six independent faces
supply 36 vertices; each record contains three position floats followed by two
UV floats (20-byte stride).

For t = time_ms / 1000, the vertex shader applies X rotation
ax = 0.30 + 0.43*t, followed by Y rotation ay = 0.40 + 0.70*t, then adds
(0,0,3) to the result. The rotations are:

    rx = (x, cos(ax)*y - sin(ax)*z, sin(ax)*y + cos(ax)*z)
    view = (cos(ay)*rx.x + sin(ay)*rx.z,
            rx.y,
            -sin(ay)*rx.x + cos(ay)*rx.z + 3)
    clip = (1.2*view.x, -1.6*view.y,
            (10/9.9)*view.z - 1/9.9, view.z)

The positive-height Vulkan viewport is (0,0,320,240), with depth range 0–1.
The explicit negative clip Y makes world-positive Y appear toward the top of
the displayed image. The perspective has focal scale 1.6, aspect 4/3, near 0.1,
and far 10.

| Face | U coordinate | V coordinate |
|---|---|---|
| +X | (0.375-z)/0.75 | 0.5-y |
| −X | (z+0.375)/0.75 | 0.5-y |
| +Y | (x+0.75)/1.5 | (z+0.375)/0.75 |
| −Y | (x+0.75)/1.5 | (0.375-z)/0.75 |
| +Z | (x+0.75)/1.5 | 0.5-y |
| −Z | (0.75-x)/1.5 | 0.5-y |

Each face lists corners with UVs (0,0),(1,0),(1,1),(0,1) and expands them in
corner order 0,1,2,0,2,3. The fragment shader samples a real 64×64 RGBA8 texture.
For texel coordinates x,y:

- R is 32 when (floor(x/8)+floor(y/8)) is even, and 224 when odd.
- G is 32+3*x; B is 32+3*y; A is 255.

The sampler uses normalized coordinates, nearest minification/magnification,
clamp-to-edge U/V/W, and mip level zero. The checker plus independent U/V color
gradients lets the external oracle detect incorrect texture coordinates as
well as an absent texture. Shader inputs are CPU-authored; displayed framebuffer
pixels are produced by Vulkan and are never substituted or repaired on the CPU.

## Capture protocol

    /bin/vkdemo --verify-session --token=q307-example

This documented diagnostic mode uses the same initialization, draw, readback,
presentation, and teardown functions as ordinary animation. One process retains
all resources across six frames:

1. Fixed times 0, 1000, and 2500 milliseconds.
2. Three frames driven by a new CLOCK_MONOTONIC epoch.

After each presentation the program prints and flushes:

    VKDEMO PRESENT run=q307-example mode=fixed sample=1 frame=1 time_ms=0 rgb_sha256=<64 lowercase hex digits> width=320 height=240

Frame is globally 1–6; sample is 1–3 within each mode. The SHA256 covers all
320×240 RGB pixels in top-to-bottom row order, omitting alpha from the unmodified
RGBA readback. Every readback alpha byte must be 255. The program then waits at
most thirty seconds for one newline on stdin. The host captures and verifies
the retained image before sending that acknowledgment. EOF, invalid response
text, timeout, or any render failure exits unsuccessfully.

Live time includes acknowledgment waits between its frames. A capture harness
can therefore impose a minimum delay before acknowledgment and independently
check increasing times and changed images. After the sixth acknowledgment and
successful resource close:

    VKDEMO DONE run=q307-example frames=6

The external harness compares the guest RGB hash with the VNC image and applies
an independent ray-box / face-UV / nearest-texture oracle. This program does not
generate or compare a CPU reference rendering.

## Resource and protocol boundaries

The program uses the current GPU capset/blob/copy/command/presentation UAPI
without adding a kernel ioctl or changing HAL. Each invocation opens a fresh
ordinary GPU session and creates one Venus context. Shared bootstrap IDs 1–4
belong to instance/physical device/device/queue; application IDs begin at 16.

The upload and readback Vulkan allocations require HOST_VISIBLE and
HOST_COHERENT memory. Each allocation is exported to a blob **once**, then reused
for the entire session. Re-exporting the same device-memory identity on each
frame is invalid for the selected renderer. The 8 MiB mapped host aperture used
by the current remote loop contains a 4 KiB reply blob plus the approximately
20 KiB upload and 300 KiB readback allocations.

The original 64×64 texture is transferred once into its optimal sampled image.
Each frame resets only a completed command pool/fence, records the graphics
draw and image-to-buffer copy, submits, and polls the real Vulkan fence.
Command-stream trailer completion and GPU execution completion remain separate.
No queue/device-idle or CPU map/unmap Vulkan command is used.

Normal teardown destroys exported GPU copies, destroys non-memory Vulkan objects
in reverse creation order, and frees memory only after all bound buffers and
images are gone. The reply blob remains alive until common client close.
Partial initialization, a fatal command error, or a pending submission delegates
cleanup to context close instead of issuing more potentially invalid commands.

## Shader provenance and regeneration

[cuboid.vert](shaders/cuboid.vert) and [cuboid.frag](shaders/cuboid.frag) are
original zedBSD GLSL under the Zlib license. Their SPIR-V binaries and
[generated C arrays](shaders.h) are compiled forms of those sources.
[provenance.json](shaders/provenance.json) records exact source/binary hashes and
compiler/validator versions. No Mesa, virglrenderer, or shader compiler
implementation is incorporated into the base.

On a host with Shaderc glslc and Khronos spirv-val:

    python3 userland/base/vkdemo/shaders/regenerate.py

The script selects Vulkan 1.1, SPIR-V 1.0, and -O0, validates both modules, and
publishes their generated artifacts after both stages succeed. Tool calls are
bounded. Normal guest builds use the committed generated arrays.

The public wire command numbers, structure tags, pointer cardinalities, and
field ordering were checked against virglrenderer 1.1.0 commit
1aeaf5e10a9c89096e96d09599aa419d5c50712f, embedded Venus protocol
git-ca1e9220, wire format 1, Vulkan XML 1.3.269.
The encoding implementation here is independently written.

Primary references:

- [Pinned official Venus protocol and renderer](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/1aeaf5e10a9c89096e96d09599aa419d5c50712f/src/venus)
- [Shaderc command-line compiler](https://github.com/google/shaderc/blob/main/glslc/README.asciidoc)
- [Khronos SPIR-V Tools](https://github.com/KhronosGroup/SPIRV-Tools)

Focused parser validation:

    sh plan/ws014/tests/run-vkdemo-cli-test.sh

Its stub deliberately fails before acquiring a GPU; parser success is distinct
from the real guest rendering and image evidence recorded by the remote loop.
