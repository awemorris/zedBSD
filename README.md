zedBSD
=====

`zedBSD` is a boot loader and a generic pre-boot operating environment.
In addition to a normal boot loader feature, it integrates the Noct
language and its JIT virtual machine for an extension mechanism.

The boot menu, shell, and utilities are [NoctLang](https://github.com/awemorris/NoctLang)
scripts; hardware access is exposed to scripts through Noct's NAPI.  The
graphical layer, BeUI, grew here and now lives upstream in Noct, so the
same program runs in the pre-boot environment, under MS-DOS on the
PC-9800 series, and on a desktop host used for development.
The bundled Remacs editor (a Noct sample application, with SKK Japanese
input) makes it possible to edit an operating system's configuration
files before the operating system boots.  The Holoris game
(`apps/HOLORIS.NCT`, a wireframe falling-block hologram) demonstrates
that BeUI alone carries real-time programs: it draws with the BeUI
primitives, paces itself with `BeUI.getMilliseconds`/`BeUI.sleep`, and
reads held keys with `BeUI.isKeyDown`.

The supported target today is the NEC PC-9800 series.  The tree is laid
out so that further targets (PC/AT BIOS, UEFI) can be added under
platform/ without touching the shared code.

## Layout

| Directory        | Contents                                                    |
|------------------|-------------------------------------------------------------|
| `core/`          | Platform-neutral services: filesystem, FAT, environment, namespace, kernel image loading, console interface, Noct integration |
| `libc/`          | Freestanding libc subset                                    |
| `softfloat/`     | Soft-float support compiled from the vendor GCC/musl sources |
| `platform/pc98/` | PC-9800 target: IPLs, stage 1/2, console, timer, Noct target adapter, DOS installer |
| `apps/`          | Noct programs shipped on the boot volume (`AUTOEXEC.NCT`, `LS.NCT`, `CP.NCT`, `HOLORIS.NCT`, Remacs configuration) |
| `noct/`          | NoctLang submodule: the VM, the BeUI graphical API and its PC-98 display backends, and Remacs under `apps/remacs` |
| `vendor/`        | GCC and musl source submodules, used only by the softfloat build |
| `scripts/`       | Build helpers, image installer, QEMU tests                  |
| `tests/`         | Host tests and QEMU test configurations                     |

## Building

Requirements: GNU make, binutils, gcc with 32-bit support (`gcc-multilib`
and `libc6-dev-i386` on Debian), and python3.  Image installation and the
QEMU tests additionally need `mtools` and a PC-98-capable QEMU.

```sh
git submodule update --init noct
./build.sh pc98 all check
```

`./build.sh ARCH [targets...]` wraps `make ARCH=... -j$(nproc)` and lists
the available architectures when run without arguments.  Each architecture
is described by `platform/ARCH/platform.mk` and builds into `build/ARCH/`;
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
`build/remacs`, `build/releases`).  `make check` runs the host test suite;
`make clean` removes the current architecture's tree and `make distclean`
removes all of `build/`.

`make hdd-image` builds
`build/pc98/hdd-test.img`: a disk with a NEC PC-98 partition table and a
fully installed BOOT volume, including the Remacs bytecode (requires
mtools, and cmake for the host Noct compiler).

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

The boot volume is configured by `ZEDBSD.CFG` (or `AUTOEXEC.NCT` for a
scripted startup).  Images created before the rename used `BOOT.CFG`;
stage 2 still falls back to that name for one release.

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
