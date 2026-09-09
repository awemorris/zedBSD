# ws019-p029: BeUI graphic installer frontend

Status: planned; user-authorized addition, 2026-09-09
Parent: [WS019](../ws.md)
Order: finish the current text installer first, then this frontend; NVMe p050
follows completion of the whole installer. This belongs to the active goal.

Provide /sbin/zedinst and /sbin/zedinst-graphic as Noct-based entry points using
/bin/noct. Current packaging places the text launcher at /bin/zedinst; migrate
its package, usage, fixtures and documentation to the newly specified /sbin
location. Do not change the interpreter's /bin/noct location. Both entry points
share discovery, source admission, install planning, validation, copying,
formatting, publication, cancellation and cleanup. Keep frontend state/input/
rendering separate from transaction decisions; neither frontend independently
decides whether an operation is safe or successful.

Visual direction: the first user attachment is a design reference, not a new
partitioning or product specification. Preserve the already approved FAT
coexistence/native-UFS layouts; do not add Home partitions, locale or account
setup solely because those labels appear in the reference image.

Reference image:
/home/awe/.codex/attachments/c64005fa-5aad-4cda-bf3a-b11a6ece16da/codex-clipboard-9349c15c-7347-42c6-a5bd-ee65e3333bab.png

Supplied background artwork:
/home/awe/.codex/attachments/94d20a3f-fb70-46f7-97b1-ff20445929e1/背景.jpg

Use the supplied second background, resized/cropped without distortion for
640x480. Precompose the background, blurred/dark panel, borders, buttons and
static decoration for each screen/selection state into RGB24 assets. At runtime
draw the selected prepared image and only dynamic labels, disk identities,
capacity, focus and progress. Restore changed regions from the prepared image
before repainting. No window compositor or runtime blur is required. Adapt font
sizes/spacing for 640x480 rather than shrinking all reference lettering until
it is unreadable; paginate disk lists and retain full identity on review.

Actual capability audit: Noct BeUI supports uncompressed 24-bit BMP loadImage,
drawImage/drawImageRegion, drawText, keyboard and pointer input. One 640x480 RGB24
image is 921600 bytes, under its 2-MiB decoded/source limits. Keep only needed
screen/state images resident. Current drawText paints an opaque background;
define foreground-only glyph drawing or background-preserving software text
rendering through BeUI, rather than covering the prepared panel with mismatched
rectangles. Noct initWithHint(24) is a preference and has no resolution argument;
configure/verify the boot/display mode and distinguish logical RGB24 from the
physical framebuffer storage. Do not claim exact 640x480/24bpp merely from the
hint's success. Record required graphics/boot capability extensions before
implementation and test their real target behavior.

Navigation: keyboard Tab/arrows/Enter/Escape plus pointer selection. Share the
same source/mode/disk/review/progress/completion/error state machine with the text
frontend. Whole-disk destruction still requires an explicit NO/YES choice,
default NO, tied to the revalidated selected disk. Source selection remains
installation disk only, with HTTP unavailable. Missing assets/display/mode or
input loss must fail before destructive actions and restore display/input state.

Acceptance: both frontends produce the same plan and refusal behavior for the
same observed disks; cancel/refusal leaves target unchanged; both installation
modes and copy progress complete through the shared backend. Capture actual
640x480 screens, including multiple disks, long labels, empty lists, errors,
confirmation and progress; inspect keyboard/pointer focus and legibility.
Define a finite queue after the text installer completion, then implement and
verify. Graphical mockups alone do not complete this phase.
