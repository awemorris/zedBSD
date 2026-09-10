# Installer screen assets

The supplied background artwork is preserved in `background.jpg`. The editable
SVG layouts and `build-ui.cjs` precompose the crop, blur, panel, card outlines,
buttons and breadcrumb decorations at 640x480. The OS loads the checked-in
uncompressed RGB24 BMPs; it does not need SVG, Node, a compositor, or a network
connection. Text and selected image regions are drawn through BeUI at runtime.

Regeneration uses `@resvg/resvg-js` 2.6.2 and DejaVu Sans. Install the renderer
into a disposable tools directory and set NODE_PATH to that directory's
node_modules when running `node build-ui.cjs [DejaVuSans.ttf]`. Normal OS builds
consume the prepared BMPs without regenerating them. PNGs are inspection copies;
only BMPs are installed. `-focus` images are region atlases, not whole screens
with all items selected simultaneously.

UEFI commonly stores its framebuffer as XRGB32. RGB24 here describes the asset
and BeUI image format; it does not claim 24-bit physical GOP storage.
