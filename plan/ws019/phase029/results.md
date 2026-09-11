# q184 results — completed / cleared

The text installer was accepted before this phase in q182/q183. q184 retains
its flow and shares source/mode/disk/shell decisions in `wizard.noct`. Text and
BeUI frontends provide review, stage and copy-progress callbacks. Both launchers
are packaged under `/sbin`; the interpreter remains `/bin/noct`.

Completed implementation/checkpoints:

- Precomposed 640x480 RGB24 BMPs from the supplied artwork and editable SVG
  templates. Runtime paints text and prepared focus regions; no compositor.
- Optional UEFI `video=WIDTHxHEIGHT` selection, canonical bounded parsing,
  duplicate/malformed/unsupported refusal, QueryMode pool ownership, SetMode
  result verification, then fresh physical framebuffer mapping validation.
  Config/handoff structures retain their ABI. Installer source profile admits
  the video directive. The image requests 640x480; actual QEMU framebuffer is
  640x480x32, distinct from RGB24 assets.
- Noct package patches 0004/0005, patch level zedbsd6: transparent text,
  actual framebuffer depth and live input attachment observation. Original
  verified host/target source trees were preserved in q184 temp backups;
  replacement trees were extracted from the verified archive plus patches.
- Shared frontend extraction and native/admission/layout Noct focused checks
  pass in host JIT. The UEFI mode mock passes ordinary and ASan/UBSan. The
  actual packaged zedBSD glyph function passes a nonuniform-background test,
  opaque-path preservation, short-bitmap refusal and fill-failure propagation.
  Its ASan/UBSan fixture disables ASan global registration so unused VM FFI
  sections can be discarded; stack/heap/undefined-behavior checks remain on.
- amd64 builds q184-amd64-1, -2 and -3 passed (later changes need their own gate).
- `temp/q184-graphic-navigation1`: PASS public graphical source/mode/disk
  navigation, non-TTY refusal, Escape, complete target hash preservation,
  console restoration and unchanged production image. Actual 640x480 source,
  mode, disk and cancelled screenshots captured; source shown to the user.
  This predates the final live-input/depth guards and empty-slot cleanup.

Native acceptance: `temp/q184-graphic-native1`, PASS against build -3.
The fixture reads actual rendered text using the kernel's public UEFI font,
not private state, hidden flags or arbitrary sleeps. It covers default NO,
real dedicated installation and two source-free boots. Both boots verified UFS
root, active `/swapfile`, persistence and checked halt. Source rootfs.img and
production image hashes remained unchanged. 265 files and 39 directories were
copied and verified. An actual mid-copy screen shows 57/265 files.

Build -4 subsequently passed with presentation-only refinements: distinguish
pre-confirmation preparation from authorized installation, remove inapplicable
input instructions while working, and mark truncated long labels with ellipses.
`temp/q184-graphic-coexist1` is running cancel/install/rerun/conflict acceptance
against this build. No production edits or builds while its VM is active.

Remaining: full coexistence graphical acceptance; pointer, held-input,
shell-return, missing-assets/input and long/empty/multiple-option fixtures;
final text regression and relevant builds; capture/inspect actual progress and
result screens, and synchronize the finished phase/queue only with evidence.

Protocol reference used to confirm GOP call signatures and ownership:
[UEFI GOP](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html)

Final review follow-up (after the active VM): defer the old initial framebuffer
layout validation until after optional mode selection. The existing duplicate
early validation unnecessarily rejects a current GOP BltOnly layout even when
a requested RGBX/BGRX mode is available. The authoritative post-selection
mapping check already covers absent/explicit requests. Add a firmware-mock
transition case and retain a final QEMU boot gate.

Also preserve the backend's returned report before presenting the final screen.
A display/input failure while drawing the completion screen must not discard a
successful (or cancelled/failed) transaction report. Report presentation/close
errors separately, restore the console, and let the stored backend report decide
installation status. Add focused final-report failure coverage before closure.

## Closure, 2026-09-10

Status: completed / cleared under the user-approved normal-path scope.
`temp/q184-graphic-coexist1` finished PASS: default NO preservation, graphical
FAT installation, rerun and conflict refusal. Log: `/tmp/zedbsd-q184-graphic-coexist1.log`.
No cancellation signal was needed. Build -4 passed. Normal native installation
and two source-free boots passed as recorded above. No further abnormal-case
fixtures or additional regression runs are required by the latest user decision.
Earlier remaining-work lists and final-review suggestions above are superseded
as closure requirements; the two presentation/firmware edge-case suggestions
are deferred observations, not claimed fixes. Prepared input/layout/installed-
boot fixtures were not run. The unimplemented presentation-result fixture is
stored in temp rather than kept as a runnable test.

PC98 support is tracked independently in p050; this acceptance proves amd64,
not PC98 or physical 24bpp GOP storage.
