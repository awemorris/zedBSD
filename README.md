zedBSD
=====

`zedBSD` is a boot loader and a generic pre-boot operating environment.
In addition to a normal boot loader feature, it integrates the Noct
language and its JIT virtual machine for an extension mechanism.

The shell is a native user process and optional utilities are
[NoctLang](https://github.com/awemorris/NoctLang) scripts; hardware access is
exposed to scripts through Noct's NAPI.  The
graphical layer, BeUI, grew here and now lives upstream in Noct, so the
same program runs in the pre-boot environment, under MS-DOS on the
PC-9800 series, and on a desktop host used for development.
Product menus and applications such as Remacs and Holoris are maintained by
linux-pc98 and installed as an overlay.  They are intentionally not part of a
standard zedBSD image.

The supported target today is the NEC PC-9800 series.  The tree is laid
out so that further targets (PC/AT BIOS, UEFI) can be added under
platform/ without touching the shared code.

## Layout

| Directory        | Contents                                                    |
|------------------|-------------------------------------------------------------|
| `include/`       | Public HAL, kernel, and user ABI interfaces              |
| `src/hal/`       | HAL and PC-98 board support                              |
| `src/kern/`      | Platform-neutral kernel services                         |
| `userland/`      | crt0, libc glue, shell, Linux loader, and Noct runtime   |
| `libc/`          | Freestanding libc subset                                    |
| `softfloat/`     | Soft-float support compiled from the vendor GCC/musl sources |
| `platform/pc98/` | PC-9800 target: IPLs, stage 1/2, console, timer, Noct target adapter, DOS installer |
| `apps/`          | Generic Noct programs (`ls.nct`, `cp.nct`, `hello.nct`, `bmpview.nct`) |
| `userland/noct/noct-upstream/` | NoctLang submodule                       |
| `vendor/`        | GCC and musl source submodules, used only by the softfloat build |
| `scripts/`       | Build helpers, image installer, QEMU tests                  |
| `tests/`         | Host tests and QEMU test configurations                     |

## Building

Requirements: GNU make, binutils, gcc with 32-bit support (`gcc-multilib`
and `libc6-dev-i386` on Debian), and python3.  Image installation and the
QEMU tests additionally need `mtools` and a PC-98-capable QEMU.

```sh
git submodule update --init userland/noct/noct-upstream
./build.sh all pc98
./build.sh check pc98
```

`./build.sh COMMAND PLATFORM [make options or additional targets...]` wraps
`make ARCH=... -j$(nproc)`.  Run `./build.sh help` to list the common commands,
examples, and available platforms.  The first argument may be any Make target,
so individual artifacts can be built with commands such as
`./build.sh vmunix pc98` and `./build.sh NOCT.ELF pc98`.  Each platform is
described by `platform/PLATFORM/platform.mk` and builds into `build/PLATFORM/`;
for pc98 the main artifacts are `build/pc98/vmunix` (stage 2),
`build/pc98/IO.SYS` (stage 1), and the IPL binaries.

`vmunix` is a two-segment ELF: a loader-closure segment at 0x20000
(everything that may run while a kernel loads, capped below the boot
parameters at 0x80000) and the Noct/BeUI bulk at 0x100000, capped below
the PC-98 15 MiB hole.  Stage 1 streams the segments into place with a
tiny fixed-subset ELF loader; `scripts/patch-stage2.py` enforces the
subset at build time.  Booting requires an IDE/CF-style disk with
512-byte sectors — FDD boot retired when vmunix outgrew a flat image
(floppies can still be mounted from the running system).  Architecture-neutral
host artifacts stay shared at the top of `build/` (`build/host-noct`,
`build/releases`).  `make check` runs the host test suite;
`make clean` removes the current architecture's tree and `make distclean`
removes all of `build/`.

`make hdd-image` builds
`build/pc98/hdd-test.img`: a disk with a NEC PC-98 partition table and a bare
BOOT volume.  It starts `/bin/sh` directly and contains no automatic startup
script or product GUI (requires mtools).

Kernel console messages are maintained as UTF-8 text in
`src/kern/messages.txt`.  Every non-clean `build.sh` command first updates
`build/PLATFORM/kern/messages.h`; it can also be generated on its own with:

```sh
./build.sh messages pc98
```

The underlying generator accepts an input file followed by an output file:

```sh
python3 scripts/generate-messages.py \
    src/kern/messages.txt build/pc98/kern/messages.h
```

Input lines use `identifier<TAB>UTF-8 message`.  Blank lines and lines beginning
with `#` are ignored.  The identifier must begin with a lowercase ASCII letter
and may contain lowercase letters, digits, and underscores.  The generated
header is deterministic and must not be edited by hand.

The softfloat build compiles GCC soft-fp and musl math sources directly
from source trees.  Either initialize the submodules:

```sh
git submodule update --init vendor/gcc vendor/musl
```

or point the build at existing checkouts (for example the linux-pc98
toolchain trees):

```sh
make ZEDBSD_GCC_ROOT=../linux-pc98/toolchain/gcc \
     ZEDBSD_MUSL_ROOT=../linux-pc98/toolchain/musl all check
```

## Configuration

The kernel starts `/bin/sh`.  If an integrator explicitly installs
`/etc/zinit.rc`, the shell offers a one-second cancellation window and sources
it; the standard zedBSD image does not install this file.  Linux-pc98 owns its
product startup script and GUI overlay.

## QEMU tests

The `scripts/test-*.sh` suites run zedBSD under an emulated PC-98.  They
expect `QEMU` (a PC-98-capable `qemu-system-i386`), `PC98_BIOS_DIR`, and
`ZEDBSD_TEST_BASE_IMAGE` (a release disk image to install into) in the
environment, and drive the milestone `*-verify` targets in the Makefile.

## History

zedBSD grew inside the [linux-pc98](https://github.com/awemorris) project as
its `bootloader/` directory; this repository carries that history, extracted
when zedBSD was promoted to a standalone project.

## License

zedBSD is distributed under the zlib License (see LICENSE).  NoctLang is
also zlib-licensed.  The soft-float objects are built from GCC libgcc
soft-fp sources (LGPL 2.1+ with a linking exception that permits
unrestricted redistribution of linked combinations) and musl math
sources (MIT); their license texts (GCC COPYING.LIB, musl COPYRIGHT)
accompany binary distributions.
