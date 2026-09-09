# q129 current UFS formatter results

Date: 2026-09-09
Status: completed

Current mkfs already emits the single 64-bit UFS format (magic 0x19540119),
accepts `-t ufs`, and rejects former ufs1/ufs2 names. No missing format migration
was found in production code. Removed the obsolete driver-fragment generator
and include path from the host fault runner; the formatter and decoder remain
independent userland sources. Updated p008 current-state note and WS019 fixture
description without rewriting q079 historical evidence.

- WS024 temp/q129-current-mkfs/results.json: 12 ordinary/sanitizer target cells,
  six C/backend/Python comparisons, two sparse maximum-size gates PASS.
- /tmp/zedbsd-q129-formatter-fault.log: 12,521 checks in each of ordinary and
  ASan/UBSan, plus maintained Noct image byte equality PASS.
- /tmp/zedbsd-q129-formatter-build.log: explicit amd64 make -j16 fixture PASS.
- WS019 temp/q129-formatter-format/result.json: PASS format. Real target mkfs,
  old type rejection, directory/device/empty/symlink refusal, swap busy refusal,
  fixed data/swap creation, preserved GPT/FAT labels/sentinel and source hash.
- WS019 temp/q129-formatter-overlay/result.json: PASS overlay. Generated UFS
  mounts writable as overlay data; write survives reboot and readback, generated
  swap is active before/after reboot. No production media modified.

Combined first attempt failed while rewriting boot configuration on the IDE
boot disk: ATA BIO_FLUSH status=C0/EIO, already known BUG001. Formatter and NVMe
operations had passed. Follow-up used the existing separate format/overlay
cells, selecting the boot configuration in the disposable host copy, to verify
formatter and persistence independently of that ATA failure. This does not
claim BUG001 fixed or combined installer acceptance complete.
