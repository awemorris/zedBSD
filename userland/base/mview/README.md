# mview

A Wayland model viewer using standard Vulkan 1.0, `VK_KHR_surface`,
`VK_KHR_wayland_surface` and `VK_KHR_swapchain`, with the standard
`wl_compositor`, `xdg-shell` and `wl_seat` (`wl_pointer`, `wl_keyboard`) client
interfaces. Like `wltest` it contains no GPU ioctl, Venus command or zedBSD
buffer ABI; the selected Vulkan library owns its WSI transport.

It shows a model converted by `tools/fbx2mview.py` (format:
[`models/README.md`](models/README.md)). The package installs the converted
`qs40` model to `/usr/share/mview/qs40/`, which is the default.

```
/bin/zdesktop --socket=/tmp/wayland-0 --width=640 --height=480 &
/bin/mview --display=/tmp/wayland-0
/bin/mview --display=/tmp/wayland-0 --model=/usr/share/mview/qs40 --token=run1 --timeout-s=300
```

## Options

| Option | Meaning |
| --- | --- |
| `--display=NAME` | Wayland display; without it the standard environment selects one |
| `--model=DIR` | directory holding `model.txt` and its textures (default `/usr/share/mview/qs40`) |
| `--token=NAME` | label of every log line (1–64 of `A-Z a-z 0-9 - _`, default `manual`) |
| `--frames=N` | stop after N presented frames and draw continuously; `0` (default) draws on demand until quit |
| `--timeout-s=N` | stop normally after N seconds; `0` (default) means no deadline |
| `--spin=N` | turn the model for N seconds, drawing every frame, and report the frame rate |
| `--shading=vertex\|pixel` | lighting per vertex (default, the p013 images) or per pixel (see Rendering) |
| `--windowed` | a window the compositor places instead of asking for fullscreen |
| `--size=WxH` | the window's size when the compositor leaves it to the viewer (64–4096 each, default 640x480) |

## Controls

| Input | Action |
| --- | --- |
| left drag | orbit: yaw 0.5° per pixel, pitch 0.5° per pixel (clamped to ±89°) |
| right or middle drag | pan in the view plane; the model follows the cursor |
| wheel | zoom: distance × 1.1 per notch (down = away), clamped to 0.05–20 × model radius |
| arrows | orbit 5° per press |
| `+` / `=` / keypad `+`, `-` / keypad `-` | zoom one step in / out |
| `R` | reset to the initial view |
| `Q`, `Esc` | quit |

Keys are Linux evdev codes (zdesktop sends no keymap); keyboard repeat is not
synthesized. Without a `wl_seat` the viewer still renders, without input.

## Log

```
MVIEW START run=<t> model=<dir> vertices=<n> triangles=<m> materials=<k> textures=<t>[ shading=pixel]
MVIEW INPUT run=<t> kind=motion|button|axis|key <event fields> yaw=<deg> pitch=<deg> distance=<d> pan=<x>,<y>
MVIEW FRAME run=<t> frame=<n> yaw=<deg> pitch=<deg> distance=<d> pan=<x>,<y>
MVIEW DONE run=<t> frames=<n> reason=quit|closed|timeout|frames
MVIEW FAILED run=<t> api=<operation> result=<VkResult> cleanup=<VkResult> errno=<e> frames=<n>
```

`MVIEW FRAME` is printed for the first presented frame and for the first
frame presented after the view changed; an unchanged view is not redrawn.
Drag motions are coalesced into one `kind=motion` line per frame. A model that
cannot be read prints `MVIEW MODEL ERROR` with the file, line and reason before
`MVIEW FAILED api=mview_model_load`.

The first frame is deterministic (no time-based animation), and `R` restores
every view parameter exactly, so the frame after `R` equals the first frame
pixel for pixel.

## Rendering

- One device-local vertex buffer (position, normal, UV; 32-byte stride) and
  one 32-bit index buffer hold every mesh. Indices are sorted into one draw
  group per material, ordered opaque, cutout, blend.
- Six pipelines: alpha mode (opaque; cutout, which discards alpha < 0.5;
  blend, straight alpha without depth writes) × cull mode (back, none).
  Viewport and scissor are dynamic.
- One descriptor set per texture (one combined image sampler, trilinear,
  repeat); untextured materials sample a 1×1 white texture.
- Push constants (128 bytes, both stages): the clip transform and the normal
  rotation as columns, and the material colour.
- Lighting: Lambert from a fixed view-space direction plus 0.35 ambient,
  per vertex, times texture × material colour. Clear colour is dark grey.
- `--shading=pixel` lights every pixel instead (`shaders/pixel.vert`,
  `pixel.frag`, `pixel-cutout.frag`): a host-visible uniform buffer, binding 1
  of every texture's set, holds the scene block (`struct mview_scene`, std140:
  model, view and projection matrices and the normal matrix, whose product is
  the per-vertex path's clip transform; an ambient term; three lights). The
  fragment shader loops over the lights -- one directional, two point lights
  with constant/linear/quadratic attenuation in units of the model radius --
  with Blinn-Phong (`normalize`, `dot`, `max`, `pow`), multiplies texture ×
  material colour by the diffuse light and adds the specular light; the cutout
  variant discards alpha < 0.5. The lights are fixed in view space, so `R`
  still restores the first frame exactly. The block is rewritten before each
  frame is recorded (the previous frame's fence has been waited on).
- Textures are uploaded through one reused host-visible staging buffer (the
  Venus host-visible window is small) and mipmapped with `vkCmdBlitImage`, or
  with a CPU box filter when the format cannot be blitted with linear
  filtering. Depth is D32 (or D24S8 / D32S8).
- Shaders are `shaders/*.vert|frag`, compiled offline by
  `shaders/regenerate.py` (glslc, spirv-val) into `shaders/*.spv` and
  `shaders.h`; `shaders/provenance.json` records tool versions and hashes.

## Limits

- Bind pose only: no skinning, bones or blend shapes.
- Simple lighting: no normal maps, shadows or MToon-style shading; specular
  highlights only with `--shading=pixel`.
- Textures are sampled as UNORM because the surface is UNORM; the stored
  sRGB colours reach the screen unchanged, and lighting is applied in that
  encoding.
- Blend materials are drawn after all others but are not sorted far to near.
- A resize keeps the camera; the initial fit is computed for the first size.

## Test

`plan/ws031/tests/run-mview-host-test.sh` builds the model reader, camera and
input handling on the host (ASan/UBSan) and checks the fixture in
`models/test/` (a textured cube with opaque, cutout and blend faces and a
second mesh), malformed models, the initial fit, the front-face winding, and
that `R` restores the view bit for bit. It also loads `models/qs40/` when
present.
