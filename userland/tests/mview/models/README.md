# mview models

Each directory below holds one model converted for `/bin/mview` by
`../tools/fbx2mview.py`. The viewer reads only these files; it never sees FBX,
zlib or PNG.

| File | Content |
| --- | --- |
| `model.txt` | the polygon list (format `mview 1`, below) |
| `tex/<index>.pam` | one uncompressed RGBA texture per `texture` line |
| `provenance.json` | source file and SHA-256, converter path and SHA-256, the exact regeneration command, downscale rule, counts, copyright |
| `source/` | the original FBX the model was converted from |

`qs40/` is the user's own character model (`QS-40-49244 r4.fbx`); the user
holds its copyright and permitted committing it and its conversion.

## Format `mview 1`

UTF-8 text (in practice ASCII), LF line ends, one record per line, fields
separated by one space, no comments and no blank lines. The records appear in
this order: the header, `bounds`, all `texture` lines, all `material` lines,
then each mesh as a `mesh` line followed immediately by its `v` lines and its
`t` lines.

```
mview 1
bounds <minx> <miny> <minz> <maxx> <maxy> <maxz>
texture <index> <file> <width> <height>
material <index> <name> texture <index|-> alpha opaque|cutout|blend cull back|none color <r> <g> <b> <a>
mesh <name> vertices <n> triangles <m>
v <x> <y> <z> <nx> <ny> <nz> <u> <v>
t <material> <i0> <i1> <i2>
```

### Coordinates

- Right-handed, +Y up, metres. The converter applies each mesh's bind-pose
  global transform (FBX `Lcl` translation/rotation/scaling, pivots, pre/post
  rotation, geometric transform, parent chain) and the file's `GlobalSettings`
  axes and `UnitScaleFactor`. Skinning, bones and blend shapes are not applied.
- A character model faces +Z (toward a camera placed on +Z looking at -Z) and
  stands on y = 0.
- Front faces are counter-clockwise when seen from the front (Vulkan:
  `VK_FRONT_FACE_COUNTER_CLOCKWISE` with a projection that keeps +Y up on
  screen; flip the setting if the projection flips Y).

### Records

- `bounds`: the axis-aligned box of every `v` in the file.
- `texture`: `index` counts 0, 1, 2, ... in file order. `file` is relative to
  the directory of `model.txt`. `width` and `height` repeat the PAM header.
- `material`: `index` counts 0, 1, 2, ... in file order. `name` has no spaces
  (anything but ASCII letters, digits, `.`, `-`, `_` became `_`). `texture` is
  a texture index or `-` for none. `color` is the diffuse colour and opacity,
  each 0..1; the fragment colour is `texture * color` (only `color` when the
  texture is `-`).
  - `alpha opaque`: alpha is ignored.
  - `alpha cutout`: discard fragments with alpha < 0.5; no blending.
  - `alpha blend`: straight (non-premultiplied) alpha blending, drawn after
    opaque and cutout geometry, far to near.
  - `cull back`: back faces are culled. `cull none`: drawn double-sided.
- `mesh`: `name` as for materials; `vertices` and `triangles` count exactly
  the `v` and `t` lines that follow.
- `v`: position (metres), unit normal, texture coordinate. Several vertices of
  one mesh are never byte-identical: identical vertices were welded.
- `t`: `material` is a material index; `i0 i1 i2` index the `v` lines of the
  **same mesh**, counted from 0. Within a mesh, triangles are grouped by
  ascending material index, so each material is one contiguous index range.

### Numbers

Fixed precision, so conversion is byte-for-byte reproducible: positions and
texture coordinates `%.6f`, normals `%.5f`, `bounds` and `color` `%.6f`
(negative zero is written as `0.000...`). Indices and sizes are decimal
integers.

### Texture coordinates

`u` runs left to right. `v` is already flipped for Vulkan: `v = 0` is the
first (top) row of the PAM image (the converter writes `v' = 1 - v` because FBX
puts `v = 0` at the bottom of the image). Values outside 0..1 wrap (repeat
addressing).

### Textures

PAM (netpbm `P7`), header exactly:

```
P7
WIDTH <w>
HEIGHT <h>
DEPTH 4
MAXVAL 255
TUPLTYPE RGB_ALPHA
ENDHDR
```

followed by `w * h * 4` bytes: rows from top to bottom, each pixel R, G, B, A,
straight alpha, sRGB-encoded colour (sample as `VK_FORMAT_R8G8B8A8_SRGB`).
Only diffuse (base colour) textures are exported; normal, emission and
separate alpha textures of the source are dropped. Identical embedded images
are stored once. Images whose longer side exceeds 1024 are shrunk to 1024
keeping the aspect ratio.
