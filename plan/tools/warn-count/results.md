# Warnings of the userland programs and shared libraries (2026-11-14)

Measured on the tree at 23a9da01e5 with the toolchain 23.1.0-zedbsd11, with
`measure.patch` (`-Wall -Wextra`, no `-Werror`) and `cc-log.sh`, `-j4`.
A "unit" is one name built by `USERLAND_PACKAGE` (a directory may make
several); its objects are counted separately.

## History

- 2026-10-03 (846464d1e9, the first commit of the current history): the
  top-level build compiled the user programs with `AMD64_USER_CFLAGS` and the
  dynamic objects (PIE programs and shared libraries) with `DYNAMIC_CFLAGS`;
  both ended in `-Wall -Wextra -Werror` (and `DYNAMIC_CFLAGS` had `-Os`).
  `package.mk` compiled only static libraries itself, with
  `-Wall -Wextra -Werror`.
- 2026-11-06 (2b5a900aa6, the build-system rework): `package.mk` began to
  compile programs and shared libraries itself.  The new compile lines had no
  warning flags; the shared-library line had no `-O` level.
- 2026-11-10 (798c1b347a): the lines became `STANDALONE_PROGRAM_CFLAGS` and
  `STANDALONE_SHLIB_CFLAGS`, still without the warning flags and (for shared
  libraries) without `-O`.  The top level still passes `CFLAGS` with
  `-Wall -Wextra -Werror` (`mk/platform-*/programs.mk`), but only
  `STANDALONE_LIB_CFLAGS` reads it.

## Results with -Wall -Wextra

| build | units compiled | objects | warnings | units with warnings |
|---|---:|---:|---:|---:|
| x86_64, default config (`make userland`) | 245 | 2135 | 0 | 0 |
| rpi4, default config | 245 | 2135 | 0 | 0 |
| vf2, default config | 245 | 2135 | 0 | 0 |
| x86_64, directories outside the default config (tests, x11, installer, fpgaload, fpgareg, ghcictl, thermal, libyolox; 92 directories) | 86 | 352 | 0 | 0 |

Per flag: none.  Top packages: none (every unit is at 0).

The only failures of the default builds were the firmware data packages,
which stop at their license prompt without a terminal (no C sources):
x86_64 and rpi4 amd-navi, i915, intelax211, intelbt, nvidia-gsp, rtl8822b
(rpi4 also bcm43455, brcmbt); vf2 powervr, rtl8822b, wave511.

Outside the default config (not related to the warning flags):

- `userland/desktop/installer` (`install-admin`):
  `installer/admin/main.c:28: fatal error: 'index-sha256.h' file not found`.
- `userland/tests/gpu-share`: `main.c:14: fatal error: 'wsi-internal.h' file
  not found`.
- `userland/tests/gpu-admission`, `gpu-recovery`, `wakebench`: no target on
  x86_64 (`No targets.  Stop.`).

Out of scope, for reference: `userland/base/noct` (CMake, its own flags)
gives 3 `-Wunused-but-set-global` warnings (hir_opt_simd.c:146,
scmback.c:66, elback.c:56).  `userland/packages/*` (ported software with
their own build) were not measured.

## Check of the proposal

x86_64 `make -j4 -k userland`, the change applied only in the measuring
worktree (not committed):

- `-Wall -Wextra -Werror` on both flag sets: all 2129 objects compiled
  again, 0 warnings, 0 errors.
- then `-Os` added to the shared libraries: their 583 objects compiled
  again and 18 shared libraries linked, 0 warnings, 0 errors.

In both runs only the firmware license prompts failed.  Not run: rpi4 and
vf2 with `-Werror` (their `-Wall -Wextra` count is 0), boot or any
run-time check of the `-Os` shared libraries.

## Proposal and migration

`makefile.diff`:

- `STANDALONE_PROGRAM_CFLAGS` and `STANDALONE_SHLIB_CFLAGS` get
  `-Wall -Wextra -Werror`, as before 2026-11-06 and as static libraries and
  the kernel have.
- `STANDALONE_SHLIB_CFLAGS` gets `-Os`: the programs use `-Os` and the
  shared libraries had `-Os` before 2026-11-06; without it they compile at
  `-O0`.

Migration: no package warns today on x86_64, rpi4 or vf2, so the flags can
go in at once in one step; no intermediate `-Wall -Wextra`-only stage and no
WS to clear warnings is needed.  If the user prefers a step, the safe order
is `-Wall -Wextra` first and `-Werror` after one build of each platform with
`count.py` at 0.  The `-O` change alters the code of every shared library
(libvulkan, libbrowser, libkeiland, ...): it needs a boot and desktop check
on QEMU before it is merged; it can be split from the warning flags.
i386 and pc9821 were not measured.
