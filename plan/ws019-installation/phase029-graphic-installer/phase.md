# ws019-p029: BeUI graphic installer frontend

Status: completed (cleared), q184; normal-path scope accepted by user 2026-09-10
Timebox: 240 active minutes
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

## Integration notes from p049

The native backend now accepts review/stage/progress callbacks. Noct lambdas
do not capture outer variables, so pass the frontend context explicitly to
all callbacks when generalizing this interface; do not store transaction state
in drawing globals. Move the source/mode/disk/shell loop into a shared wizard
and provide text/graphic choice adapters. Route coexistence confirmation and
Noct byte-copy progress through the same frontend contract; current tree-copy
progress already receives a caller context. Preserve explicit coexistence CLI.

Current graphics blit has an unused reserved word but rejects nonzero values;
MONO1 expands both foreground and background. Any transparent-glyph extension
requires a declared flag/capability and validation in the actual graphics driver
as well as the Noct adapter. The packaged Noct tree is acquired and manifest-
verified: maintain any API changes as a package patch, not an untracked edit to
the extracted dependency. Test opaque drawing unchanged and transparent pixels
against a nonuniform background.

On amd64 the linear framebuffer backend currently returns the firmware mode
and 32-bit physical storage regardless of hints. Merely adding preferred width
and height to Noct would not implement mode selection. Audit boot-time GOP mode
selection or a real backend mode facility before promising a 640x480 launch;
keep RGB24 assets distinct from physical XRGB32 storage.

Prefer a smaller first transparent-text implementation in the Noct adapter:
use its existing glyph bitmap and draw only contiguous foreground spans through
the existing FILL capability. This preserves every background pixel without a
new kernel blit ABI or compositor. Keep the opaque fast path unchanged. Only
add a dedicated transparent blit capability if measured UI latency warrants it.

The q182 coexistence run exposes long silent intervals after image copying and
during idempotent verification. Add shared per-artifact preparation/verification/
publication stage notifications for both frontends; keep byte/file counts tied
to actual work, not invented elapsed-time percentages. This is presentation of
the existing transaction, not an extra copy or success shortcut.

## q184 implementation boundary

1. Factor a shared source/mode/disk/shell wizard, frontend context and
   confirmation/stage/file-count/byte-count callbacks. Keep native/coexistence
   admission and transactions singular. Preserve explicit text invocation.
2. Package /sbin/zedinst and /sbin/zedinst-graphic, both using /bin/noct.
3. Build deterministic precomposed screen assets from the supplied artwork and
   editable UI templates, at 640x480 RGB24. Draw dynamic text and prepared focus/
   progress regions; no runtime blur or window compositing. Keep the reference
   image's visual character while retaining the approved five installer steps.
4. Maintain a Noct package patch for foreground-only text via existing glyph
   bitmaps/FILL spans; leave opaque rendering intact. Expose actual display
   depth if necessary to reject unsupported color modes. Preserve verified
   dependency identity rather than editing an extracted checkout invisibly.
5. Add bounded optional boot-time GOP mode selection from a video=WIDTHxHEIGHT
   parameter, without altering the fixed config/handoff structure layout. Use
   640x480 in the installer source image configuration and admit that parameter
   in installer source validation. Without video, preserve firmware mode.
   UEFI XRGB32 storage remains distinct from logical RGB24 assets. Revalidate
   the framebuffer mapping after SetMode; fail clearly if an explicit mode is
   unsupported, before ExitBootServices. Do not silently crop a 1280-wide
   screen and claim hardware mode selection.
6. Validate rendering/input/cleanup and common-plan parity; actual QEMU public
   native and coexistence paths, cancel/refusal and screenshots. Multi-option,
   long-label/empty/error views may use an explicitly labelled frontend fixture
   if current controller limits prevent that hardware topology; do not claim
   that fixture proves multiple-controller support (p050 follows this work).

No new installed provisioning helper or networking/account/Home feature is
introduced. Keyboard/pointer input must require a fresh explicit YES for erase;
restore console/input on all exits and re-admit source after shell return.

## Final acceptance scope

The user accepts normal-path installation as sufficient and explicitly ends
further abnormal-case expansion. Both modes pass actual graphical installation;
native additionally passes two source-free boots. Extra input/error fixtures
are not completion requirements. PC98 FAT-only support is a separate p050.
