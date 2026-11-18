# mview build-host tools

## fbx2mview.py

Converts a binary FBX 7.x file (7.4 32-bit and 7.5+ 64-bit record headers,
zlib-compressed arrays) into the `mview 1` model described in
`../models/README.md`. It runs on the build host only and needs Python 3 and
Pillow; nothing it uses enters the OS image.

```
python3 userland/tests/mview/tools/fbx2mview.py <file.fbx> <model directory> \
    [--max-texture-side 1024] --date <YYYY-MM-DD>
```

Run from the top of the tree so the paths recorded in `provenance.json` are
repository-relative. The output directory receives `model.txt`, `tex/*.pam`
and `provenance.json`; `.pam` files of an earlier run that are no longer
produced are removed. The same input always gives byte-identical output.

Regenerating the committed model (the exact command is also stored in
`models/qs40/provenance.json`):

```
python3 userland/tests/mview/tools/fbx2mview.py \
    userland/tests/mview/models/qs40/source/qs40-r4.fbx \
    userland/tests/mview/models/qs40 --max-texture-side 1024 --date 2026-09-22
```

What the converter does:

- Reads `Geometry` (Mesh) `Vertices`, `PolygonVertexIndex`, and layer 0 of
  `LayerElementNormal`, `LayerElementUV` (ByPolygonVertex, ByVertice/ByVertex,
  ByPolygon, AllSame; Direct or IndexToDirect) and `LayerElementMaterial`
  (AllSame, ByPolygon).
- Places each `Model` (Mesh) at its bind pose: the FBX SDK local-transform
  formula along the `Connections` parent chain, times the geometric transform;
  then converts `GlobalSettings` axes and `UnitScaleFactor` to right-handed
  Y-up metres. A mirroring transform reverses triangle winding. Skin
  deformers, bones and blend shapes are ignored. The computed matrix is
  compared with the file's `BindPose` and the difference is printed.
- Numbers materials in the order meshes first use them (a Model's connected
  materials, in connection order, are its material slots) and takes the
  diffuse colour, opacity and the `DiffuseColor` texture's embedded `Video`
  PNG. Normal maps (`_nml`, `Normal` in the file name) are never exported.
- Fan-triangulates polygons, writes vertices at fixed precision, and welds
  vertices whose written line is identical. Triangles of a mesh are grouped by
  material.
- Converts textures to RGBA, shrinks the longer side to the maximum (Pillow
  LANCZOS), and writes PAM. Alpha class per material: `opaque` when every
  texel has alpha 255; `cutout` when at least 98% of texels have alpha <= 8 or
  >= 247; otherwise `blend`; a material opacity below 1 forces `blend`.
- Culling: `none` for material names containing `hair`, `eye`, `facebrow`,
  `faceeyeline`, `facemouth` (any case) and for every `cutout`/`blend`
  material; `back` otherwise.

The summary printed on standard output lists per mesh the FBX polygon count,
the expected fan triangle count (sum of polygon size - 2), the written triangle
and vertex counts, and per texture and material the chosen classes.
