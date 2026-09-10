# WS009-p008 results

Status: completed / cleared q190, 2026-09-10

Updated [boot/storage/installation guide](../../../docs/howto/boot-and-storage.md),
[formatter reference](../../../docs/reference/image-formatters.md) and
[how-to navigation](../../../docs/howto/README.md).

| Requirement | Current source and acceptance basis |
| --- | --- |
| DOC-30 boot/diagnostics | Configured-loader reference, p002/q015/q032 retained boot results; current explicit CI build selection |
| DOC-32 file-backed root | rootimage.noct identity checks, rootview, current overlay configuration and q186 target-only overlay boot |
| DOC-33 native root | nativeplan/nativeinstall: GPT ESP+UFS, PARTUUID root, attribute-preserving copy, /swapfile and fstab; WS019-p049 q182 and p007 q183 acceptance |
| DOC-34 installer | wizard/textui/graphicmain/confirmation/Makefile: public paths, modes, NO/YES versus typed coexistence confirmation, HTTP unavailable, shell restart; p029 q184 and p050 q186 acceptance |
| PC98 boundary | pc98.noct: uname suffix, FAT16, <=4 GiB partition, <=2 GiB managed files, retained bootstrap; q186-install1/result.json PASS |
| NVMe target boot | WS004-p050 q187: both enumeration orders, installed login, persistence and halt; one active namespace per controller remains |
| Formatter boundary | mkfs main/block-command/ufs-format/fat32-format and mkswap size checks; regular-file and block modes separated |
| Graphic mode | graphicui accepts 640x480 at 24/32bpp; actual q184 QEMU XRGB32 differs from RGB24 assets; PC98 CoreGraph q186 |

The steps reproduce the accepted frontend sequence in documentation, not a new
execution: source selection, mode/disk/payload selection, explicit confirmation,
copy/verification, shutdown and target-only login. No additional installation,
physical media write, clean-room build or exhaustive fault run was performed.
Existing build/boot evidence is retained and linked; unsupported hardware is
not upgraded to supported by this documentation pass.

Validation: current commands and arguments checked against their sources. A
draft `swapon -s` example was removed because zedBSD does not implement that
option; the guide uses the actual boot swap message. Product link validator
passes 293 relative links (`/tmp/zedbsd-q190-links.log`); git diff --check passes.
No kernel, driver, userland implementation or test source changed in q190.

DOC-54 still depends on manually held WS014. Its absence is documented in
control-devices.md; no current GPU ABI is fabricated. No executable WS009
phase remains, but this held future deliverable is preserved in the W book.
