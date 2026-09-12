# vkdemo

The original zedBSD demo is a standard Vulkan 1.0 application. It uses the
public `<vulkan/vulkan.h>` declarations and links against `libvulkan.so`.
Its vertex and fragment SPIR-V modules draw a sampled, textured cuboid with
a depth attachment. The application contains no GPU ioctl, device-node access,
Venus encoding, protocol resource identifiers, or zedBSD graphics ABI calls.

Direct display uses `VK_KHR_surface`, `VK_KHR_display`, and `VK_KHR_swapchain`.
The app enumerates devices, displays, modes and planes, creates a compatible
320×240 display surface and FIFO swapchain, and renders directly into each
acquired swapchain image. An ordinary offscreen option uses the same graphics
pipeline, texture, depth and readback operations without acquiring a display.

## Build and ordinary use

The optional amd64 base package is vkdemo; select it in the image configuration.
Selecting it also selects the `base/libvulkan` dependency. Its normal build
consumes the checked-in original shader arrays, links a PIE executable using
`/lib/ld.so`, and installs `/lib/libvulkan.so`. No shader compiler is needed for
a normal build. Public Vulkan headers are provided in `libc/include/vulkan/`
and are copied into the target sysroot by its ordinary header installation.

    /bin/vkdemo
    /bin/vkdemo --duration=30 --device-index=0
    /bin/vkdemo --duration=0
    /bin/vkdemo --time-ms=1000 --hold=20

Ordinary animation uses CLOCK_MONOTONIC, starting after resource setup, and
runs for ten seconds by default. --duration=0 requests continuous animation;
positive durations accept 1–3600 seconds. Rendering and readback determine the
frame rate, with a short cooperative pause after each completed frame.
--time-ms=0..3600000 renders one explicit shader time and retains it for
--hold=0..120 seconds (default ten).

Each completed display frame emits a VKDEMO PRESENT line to standard output. This is
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

## Portable offscreen verification

On Linux with Vulkan development headers and a system Vulkan implementation:

    sh plan/ws014/tests/run-vkdemo-vulkan-test.sh /tmp/vkdemo-evidence

The runner compiles these same application sources against the system
`libvulkan`, renders six offscreen frames, checks the actual exported GPU pixels
with the existing independent ray/texture oracle, closes the session, then runs
ordinary animation in a second invocation. Standard loader configuration such
as `VK_DRIVER_FILES` can select an installed driver. The result records the
reported device and driver selection; software Vulkan and physical GPU results
remain distinct. This verifies portable rendering and readback, not direct
display ownership or guest WSI correctness.

The equivalent explicit application options are:

    vkdemo --offscreen --time-ms=1000 --hold=0 --output=/tmp/frame.ppm
    vkdemo --offscreen --verify-session --output=/tmp/frame.ppm

`--output` writes the latest actual GPU RGB readback as a binary PPM before its
marker is emitted. The six-frame capture protocol waits for acknowledgment
before overwriting that file. Offscreen markers use `VKDEMO OFFSCREEN` to avoid
claiming a display presentation.

## Resource and synchronization boundaries

The application allocates and binds image/buffer memory through ordinary Vulkan
API calls. Upload and readback require host-visible memory; host-coherent memory
is preferred. If the selected type is noncoherent, whole-allocation flush and
invalidate operations provide the required visibility. No memory property is
inferred from a platform device name or a transport capability.

The original texture is uploaded once. Each frame reuses a completed command
pool and fence, records the draw and image-to-buffer readback, submits, and waits
up to ten seconds for its Vulkan fence. Direct display waits on an acquire
semaphore before color attachment use, transitions the image to
`PRESENT_SRC_KHR`, and submits presentation with a semaphore dedicated to that
swapchain image. A presentation semaphore is reused only after its image is
acquired again. FIFO and display ownership are library/backend responsibilities.

Normal close waits for the device to become idle, destroys command and graphics
objects, unmaps allocations, and frees memory after its bound objects are gone.
Every successful partial allocation is retained in the same cleanup ledger, so
initialization failures also release acquired resources. The application uses
only standard Vulkan completion semantics; it does not inspect transport reply
buffers or poll protocol command identifiers.

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

Primary references:

- [Shaderc command-line compiler](https://github.com/google/shaderc/blob/main/glslc/README.asciidoc)
- [Khronos SPIR-V Tools](https://github.com/KhronosGroup/SPIRV-Tools)

Focused parser validation:

    sh plan/ws014/tests/run-vkdemo-cli-test.sh

Its stub deliberately fails before acquiring a GPU; parser success is distinct
from the real guest rendering and image evidence recorded by the remote loop.
