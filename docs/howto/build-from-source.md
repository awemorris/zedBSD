# Build zedBSD from source

Status: current for the 2026-08-25 build interface

This guide builds a zedBSD disk image from a source checkout and starts the
supported x86 QEMU targets. Commands are run from the repository root.

## 1. Host prerequisites

The normal build expects a POSIX-like host with:

- GNU Make, Git, CMake, a C compiler, GNU binutils, and a POSIX shell;
- Python 3 for the interactive `make menuconfig` frontend;
- an x86 compiler capable of both `-m64` and `-m32` when building amd64 and
  i386 targets;
- `qemu-system-x86_64` for amd64 and `qemu-system-i386` for PC/AT i386;
- enough free space for `build/`, the Noct checkout, and disk images.

PC-98 requires a QEMU build that supplies the non-upstream `pc9821` machine.
Ordinary upstream `qemu-system-i386` is sufficient for PC/AT, but not PC-98.

No host installation prefix is modified. Host tools, including the pinned
Noct checkout, remain below `build/`.

## 2. Obtain and build the host toolchain

From a zedBSD checkout:

```sh
make toolchain
```

This checks out `awemorris/NoctLang` at the revision pinned in the top-level
Makefile under `build/NoctLang`, builds
`build/NoctLang/build-static/noct`, and runs a smoke script. A second invocation
is incremental and must also succeed:

```sh
make toolchain
test -x build/NoctLang/build-static/noct
```

Network access is needed only when the pinned checkout is not already present.

## 3. Select a target

Create or replace `config.mk` with the menu:

```sh
make menuconfig
```

Select one platform, its drivers, and user programs, then save. `config.mk` is
the sole selected-target input; normal build targets reject a missing or
invalid file. It is generated and should not be hand-edited.

The target menu is organized as Architecture -> Board -> Variant. PC/AT amd64
provides `UEFI + BIOS (for PC/AT)`, `UEFI (for Apple)`, and
`BIOS (for PC/AT)`; the current single-profile boards save the fixed `Default`
Variant. A saved configuration from before the Variant field was introduced
uses its board default. Variant describes disk-image composition only; it does
not change kernel or bootloader compilation. Disk capacity is not a build-menu
selection.

The maintained x86 output directories are:

| Selection | Build directory | Disk image | Emulator |
| --- | --- | --- | --- |
| PC/AT amd64 | `build/amd64/` | `build/amd64/hdd-image.img` | `qemu-system-x86_64` |
| PC/AT i386 | `build/pcat/` | `build/pcat/hdd-image.img` | `qemu-system-i386` |
| NEC PC-98 i386 | `build/pc98/` | `build/pc98/hdd-image.img` | PC-98-capable `qemu-system-i386` |

Changing target requires rerunning `make menuconfig`; artifacts for other
targets may coexist below `build/`.

## 4. Build

Build the selected disk image with the supported parallel build:

```sh
make -j16 disk-image
```

The default target is identical:

```sh
make -j16
```

Useful narrower targets are:

```sh
make -j16 vmunix      # kernel only
make -j16 rootfs      # selected root filesystem
make -j16 world       # kernel and root filesystem
make -j16 bootloader  # selected bootloader
```

A successful disk-image build runs the target's structural validators. The
project does not use `make check` as the acceptance interface; an active Phase
names its focused tests under `plan/wsXXX-*/tests/`.

## 5. Start QEMU

For the selected target, the simplest launch is:

```sh
make run
```

For amd64, the equivalent explicit command is:

```sh
qemu-system-x86_64 -machine pc -m 512 -smp 4 \
  -drive file="$(pwd)/build/amd64/hdd-image.img",format=raw,if=ide \
  -boot c
```

For PC/AT i386:

```sh
qemu-system-i386 -machine pc -m 128 \
  -drive file="$(pwd)/build/pcat/hdd-image.img",format=raw,if=ide \
  -boot c
```

For PC-98, point `QEMU` at the compatible binary if it is not on `PATH`:

```sh
make QEMU=/path/to/qemu-system-i386 run
```

The initial success marker for the maintained x86 images is an init sequence
followed by `login:`. Exit QEMU normally with its UI or monitor controls; do
not write to a real disk device when following this guide.

## 6. Diagnostics

- `config.mk is missing or invalid`: run `make menuconfig` and save a target.
- Noct checkout/build failure: verify Git/network access, CMake, and the host C
  compiler, then rerun `make toolchain`; do not substitute an unpinned binary.
- `-m32` compile/link failure: install the host's 32-bit compiler/binutils
  support before selecting i386.
- QEMU command not found: install the appropriate system emulator or pass
  `QEMU=/absolute/path/to/qemu-system-*` to `make run`.
- PC-98 reports that the machine is unavailable: use the documented custom
  PC-98-capable QEMU; upstream QEMU does not implement this target.
- To see the authoritative target list and build summary, run `make help` and
  `make list-targets`.

Build outputs are disposable, but `make clean`/`make distclean` removal scope
should be reviewed before use in a workspace containing locally valuable
artifacts.
