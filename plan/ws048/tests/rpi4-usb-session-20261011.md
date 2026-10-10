# RPi4 USB / sessiond boot option / 2026-10-11

Base main fbcb2b543; owned worktree `.claude/worktrees/rpi4-usb-session`, branch `codex/rpi4-usb-session`.
User scope and physical observations are in [ws048-p009](../phase009/phase.md) and [ws193-p009](../../ws193/phase009/phase.md).
Config snapshot: [selected config](rpi4-usb-session-config-20261011.mk).

## Findings and changes

- Existing VFS initializes input and publishes the console before kern_platform_input_init; RPi4 omitted drv_usb_hid_input_ready, leaving enumerated HID interfaces pending. Add the existing lifecycle callback, alongside serial input, with no HAL API change.
- RPi4 builds/registers only xHCI/HID/hub even when portable storage/UAS/CDC ECM/NCM/CCID/Bluetooth/RTL8822BU are selected. Add their existing sources and registrations before xHCI attach; keep firmware separate. PCI UHCI/EHCI are not migrated to this VL805 board.
- Image writers omit cmdline.txt. Firmware's /chosen/bootargs already reaches boot.command-line and kern.boot.login; existing sessiond only starts graphical login when login=graphical. Generate/package login=graphical or login=console from ZEDBSD_GRAPHICAL_LOGIN independently of logo/quiet animation, and validate exact FAT payload. Existing service/user/login policy unchanged.
- Use a content-stable command-line file, regenerated against a phony prerequisite, so toggling back also invalidates the image while unchanged values retain mtime.

## Validation

Current main compiler/source and host Noct are read-only; completed source/build stamps held with -o. Current arm64 sysroot copied into own build and completion held. Target Noct build in a dedicated worktree reuses the user's earlier express approval, with no toolchain source/rule edits. Current verified archives/firmware copied into own caches. Fresh target programs/rootfs/image built from current sources, no prior test image or application artifacts used.

Commands, owned worktree:

- `make -j16 build/arm64/vmunix`: PASS; final log `build/kernel-build-final.log`, compiler warnings/errors0, AArch64 ELF/Image checks PASS.
- Full `make -j16 -o build/arm64/sysroot/.zedbsd-sysroot-complete -o build/NoctLang/.zedbsd-source-2.0.3-zedbsd12 -o build/NoctLang/.zedbsd-source-verified-2.0.3-zedbsd12 -o build/host-noct-state/built-2.0.3-zedbsd12-process`: PASS; `build/image-build-2.log`. Selected USB class sources compile/link under -Werror; sessiond/Wayland/auth target programs are AArch64 ELF. Full build reports 231 existing external package/jobserver warnings; changed kernel/platform sources have no warnings.
- `make -j16 [same held stamps] check-disk-image`: PASS; `build/image-check.log`, MBR/FAT/UFS layout, exact UFS bytes and boot payloads including cmdline.txt.
- Graphical login unchanged/y→n→y image regeneration, exact FAT readback and unchanged rootfs stamp: `build/option-image-test.log` PASS.
- Rootfs contains `/sbin/sessiond`, `/bin/wayland` (the actual SESSIOND_GREETER), `/sbin/passkey`, greeter service, session/autologin/rc config and _greeter account: PASS. A first presence assertion incorrectly assumed the preview utility was the greeter; actual sessiond header/manifest confirms Wayland and the corrected assertion passes. No product defect inferred from that assertion.
- Wrong expected command-line file is rejected by image checker: PASS.
- Python AST on all three image scripts; git diff --check: PASS.

Initial build cache setup fails on absent vendor firmware submodule inputs and sandbox DNS for firmware acquisition. Copy current validated firmware/cache and retain standard size/hash/license verification; final image build succeeds. No missing source rule masked.

## Changed-source full rules review

Full plan/coding-style.md, AGENTS, Guardrail and configured formatter/automation checked against final changed paths. New C uses leading declarations, separate fallible calls and guard exits, meaningful operation/error comments; adds no interfaces or libc dependency. clang-format19 previews changed ranges; manually preserve canonical multi-line function definitions where default formatter collapses them. Style checker: rpi4-pcie.c0 findings; rpi4.c3 pre-existing findings outside changed function, reduced from6 by replacing legacy function_result wrapper. No new findings or unrelated formatting. Make recipes only remove their own temporary build output. Python changes retain existing style and optional CLI compatibility. No shared Master/Queue/history/cache writes or publication/push.

## Artifacts

| Local owned artifact | Bytes | SHA256 |
| --- | --- | --- |
| build/arm64/vmunix | 8065024 | `632fdef91f651f1a8db38d9bb325e444e6b93237ef5850e01a2eef41f777571b` |
| build/arm64/ufs-root.img | 208666624 | `b0204d24320fc5715c938930c4ade2ef542f67a2d51d2e8b2f9f2395c36b3b71` |
| build/arm64/hdd-image.img | 343932928 | `2899bdef00c466f5d6eac275b79276e3195d717c8a0cfb8f15e84e1a0bd4a38e` |
| build/arm64/rootfs/sbin/sessiond | 82968 | `d4091093e5bd74a020962e71948318ee055584e0bd235e680d14ffe371341ff3` |

## Limits / next step

User confirms VC4 initialization/login after main fbcb2b543, clearing only former boot exception p008 by follow-up. Current USB fix and desktop startup have not been booted on RPi4. USB physical acceptance remains uncleared; source/build and login-option packaging can be reviewed/integrated, then user verifies USB input and greeter. Logo/kernel animation explicitly deferred. Shared projections/GitHub pending Q1; integration requires specific commit approval, no push.
