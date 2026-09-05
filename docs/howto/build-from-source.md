# Build zedBSD from source

Status: current for the 2026-09-05 build and image interface

This guide builds a zedBSD disk image from a source checkout and starts the
supported x86 QEMU targets. Commands are run from the repository root.

## 1. Host prerequisites

The normal build expects a POSIX-like host with:

- GNU Make, CMake, Ninja, a host C/C++ compiler, `patch`, `curl`, `tar`,
  `gzip`, `sha256sum`, and a POSIX shell; Git is required for development;
- Python 3 for the interactive `make menuconfig` frontend;
- `qemu-system-x86_64` for amd64 and `qemu-system-i386` for PC/AT i386;
- enough free space for `build/`, extracted Noct source, downloaded external
  inputs, and disk images.

PC-98 requires a QEMU build that supplies the non-upstream `pc9821` machine.
Ordinary upstream `qemu-system-i386` is sufficient for PC/AT, but not PC-98.

No host installation prefix is modified. Host tools, including the verified
Noct release extraction, remain below `build/`.

## 2. Obtain and build the host toolchain

On an x86_64 Linux host, install the permanent, digest-pinned LLVM 23.1.0
binary cache from release `rev-0` before the normal toolchain target:

```sh
make toolchain-cache
make -j16 toolchain
```

`make toolchain-cache` is an explicit acceleration path. It verifies the
tracked SHA-256, validates archive paths, installed identity, required tools,
versions and license, then installs below `build/llvm/`. It does not silently
fall back to unverified bytes. The following command remains the source-build
path on every supported host, and is used automatically when no accepted cache
was installed:

```sh
make -j16 toolchain
```

To materialize every declared source and firmware input for a redistributable
multi-license source tree, run:

```sh
make download
```

This verifies the official Noct `v2.0.1` source archive and the optional
firmware inputs even when those firmware packages are not selected. Downloaded
bytes are ignored by Git but remain inside the working tree, allowing the
post-download tree to be packed as a multi-license source distribution.

Both host and target Noct come from release `v2.0.1`, tag commit
`ed621e79139f55d06dd1a474243afbf0ce5efe0a`. The tracked archive identity is
2,524,680 bytes with SHA-256
`68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`.
Acquisition checks the size, digest, archive paths, member types, strict
zero-fuzz patches, and a complete extracted-source manifest before publication.
It never substitutes a Git checkout for that release archive.

The toolchain target verifies and extracts Noct below `build/NoctLang`, builds
the host interpreter with the host compiler, installs or accepts the patched
project LLVM under `build/llvm/`, constructs amd64/i386 target sysroots, and
runs focused smoke checks. A second invocation is incremental and must also
succeed:

```sh
make toolchain
test -x build/NoctLang/build-static/noct
```

The build ownership boundary is:

| Artifact | Builder and location | Role |
| --- | --- | --- |
| Host Noct | Host C/C++ compiler; `build/NoctLang/build-static/noct` | Runs repository-owned build, image, and validation scripts |
| Project LLVM | Host C/C++ compiler or verified `rev-0` cache; `build/llvm/` | Builds every maintained x86 zedBSD target artifact |
| amd64 sysroot | Project LLVM inputs under `build/amd64/sysroot/` | Public headers, startup objects, libc, builtins, and linker inputs for `x86_64-unknown-zedbsd` |
| i386 sysroot | Project LLVM inputs under `build/i386/sysroot/` | Shared PC/AT and PC-98 boundary for `i386-unknown-zedbsd` |
| Target Noct | Project LLVM, LLD, and amd64 sysroot; installed as `/usr/bin/noct` when selected | Runs inside zedBSD; does not replace host Noct |

The permanent x86_64 Linux cache is release `rev-0`, asset
`zedbsd-llvm-23.1.0-x86_64-linux.tar.gz`, with tracked SHA-256
`6f8e1154c73b9f2d32f16360ace107b7862f08e748c6f10c1bd75914aa6502c2`.
`make toolchain-cache` validates the archive, installed identity, tool set,
versions, and license. `make -j16 toolchain` remains the authoritative
source-build path and uses the official LLVM `23.1.0` source plus the tracked
zedBSD target patch when no accepted cache is installed.

Network access is needed only when a required verified release archive is
absent.
Each userland item also accepts `make download`, `make patch`, `make build`,
and `make install` from its own directory. In-tree items use no-op acquisition
and patch stages.

## 3. Select a target

Create or replace `config.mk` with the menu:

```sh
make menuconfig
```

Select one platform, its drivers, and user programs, then save. `config.mk` is
the sole selected-target input; normal build targets reject a missing or
invalid file. It is generated and should not be hand-edited.

The target menu is organized as Architecture -> Board -> Variant. PC/AT amd64
provides the following image profiles in this order:

| Menu label | Saved value | Image boot paths | Expected firmware behavior |
| --- | --- | --- | --- |
| `UEFI + BIOS (for PC/AT)` | `hybrid` | Complete GPT/ESP plus compatibility BIOS path | Boots with OVMF and SeaBIOS |
| `UEFI (for Apple)` | `uefi` | Pure Protective MBR, primary GPT, ESP, and payload FAT32; no BIOS payload | Boots with OVMF; does not boot with SeaBIOS |
| `BIOS (for PC/AT)` | `bios` | Legacy MBR and BIOS payload; no GPT or ESP | Boots with SeaBIOS; does not boot with OVMF |

The current single-profile boards save the fixed `Default` Variant. A saved
configuration from before the Variant field was introduced uses its board
default. Variant describes disk-image composition only; it does not change the
kernel, target triple, or compiled BIOS/UEFI loader artifacts. Disk capacity is
not a build-menu selection.

To build each amd64 profile, run `make menuconfig`, choose PC/AT amd64 and the
desired label, save, and run `make -j16 disk-image`. Repeat the menu/save/build
sequence for another profile; each selected profile publishes its image at
`build/amd64/hdd-image.img`, so preserve a copy elsewhere if multiple outputs
are needed simultaneously. Do not hand-edit generated `config.mk`.

The maintained six-cell positive/negative verification uses private build and
image paths and therefore does not replace `config.mk` or the ordinary output:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws020-intel-mac/tests/qemu-variant-matrix.noct \
  . plan/ws020-intel-mac/temp/p003-qemu
```

It requires the locally documented SeaBIOS and OVMF firmware files and verifies
all three profiles against both firmware families. See the
[WS020 test index](../../plan/ws020-intel-mac/tests/README.md) before running
this longer acceptance matrix.

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

For a selected BIOS-capable profile, the simplest launch is:

```sh
make run
```

On amd64, `make run` uses the default QEMU PC machine and SeaBIOS. It is
therefore suitable for `hybrid` and `bios`, but a correctly built `uefi` image
is expected not to boot through that command. Use an OVMF launch or the
maintained six-cell runner above for the UEFI path; give every run a disposable
writable OVMF variables file and never let firmware modify the source image
used as retained evidence.

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

The `UEFI (for Apple)` label describes a portable, UEFI-only disk-image layout;
it does not make QEMU emulate Apple hardware. That layout and its relocated-GPT
handling have passed the Intel Mac physical boot boundary. The separate Apple
`05ac:8406` Internal Memory Card Reader can attach with no medium and does not
publish a disk; media-insertion polling and later disk publication for that
reader are not implemented. The accepted boot used a separate USB Mass Storage
device, so the reader limitation is not a boot dependency.

## 6. Diagnostics

- `config.mk is missing or invalid`: run `make menuconfig` and save a target.
- A UEFI-only amd64 image appears not to boot under `make run`: this is the
  expected SeaBIOS-negative profile; use OVMF or the maintained Variant runner.
- Noct download/build failure: verify network access, `curl`, `tar`, `patch`,
  CMake, and the host C compiler, then rerun `make download` and
  `make toolchain`; do not replace the recorded archive with unverified bytes.
- `Missing project target tool` or `Missing target sysroot`: on x86_64 Linux,
  run `make toolchain-cache` and then `make toolchain`; otherwise run
  `make toolchain` to bootstrap LLVM from verified source.
- QEMU command not found: install the appropriate system emulator or pass
  `QEMU=/absolute/path/to/qemu-system-*` to `make run`.
- PC-98 reports that the machine is unavailable: use the documented custom
  PC-98-capable QEMU; upstream QEMU does not implement this target.
- To see the authoritative target list and build summary, run `make help` and
  `make list-targets`.

Build outputs are disposable, but `make clean`/`make distclean` removal scope
should be reviewed before use in a workspace containing locally valuable
artifacts.
