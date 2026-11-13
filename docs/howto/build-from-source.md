# Build zedBSD from source

Status: current for the build system of 2026-11-13 (the top-level `Makefile`
and `mk/`)

This guide builds a zedBSD disk image from a source checkout.  Commands are
run from the repository root.  The Raspberry Pi 5 is used as the example;
the other platforms differ in the configuration file and the image they
write.

## 1. Host prerequisites

The build expects a POSIX-like host (x86_64 Linux for the prebuilt LLVM of
section 3) with:

- GNU Make 4.4 or later (the `Makefile` uses `.WAIT`), a host C/C++
  compiler, CMake, `patch`, `curl`, `tar`, `gzip`, `xz`, `sha256sum` and a
  POSIX shell; Git for development;
- Python 3 (the image tools);
- `mtools` (`mformat`, `mcopy`, `mmd`) for the FAT partition of the
  Raspberry Pi images;
- enough free space for `build/` (the toolchain, the downloaded sources and
  the images).

No host installation prefix is modified: the toolchain, the downloaded
sources and every output stay below `build/`.

## 2. Choose a configuration

A build reads one configuration file, `config.mk` by default.  The tree
carries one per platform in `config/`:

| Platform | Configuration | Output directory |
| --- | --- | --- |
| PC (x86_64) | `config/config.x86_64.mk` | `build/x86_64/` |
| Raspberry Pi 5 and Raspberry Pi 500 | `config/config.rpi5.mk` | `build/rpi5/` |
| Raspberry Pi 4 | `config/config.rpi4.mk` | `build/rpi4/` |
| PC/AT (i386) | `config/config.i386.mk` | `build/i386/` |

The other files of `config/` (`config.pc98.mk`, `config.visionfive2.mk`,
`config.odroidc5.mk`) name platforms the new `Makefile` does not build yet.
The files below `config/ci/` belong to the build system that was replaced and
are not read by this `Makefile`.

Either name the file on every command with `CONFIGMK=`:

```sh
make CONFIGMK=config/config.rpi5.mk <target>
```

or copy it to `config.mk` once and leave `CONFIGMK=` out:

```sh
cp config/config.rpi5.mk config.mk
```

The file sets `PLATFORM` (with `ARCHITECTURE` and `BOARD`); the platform's
defaults, `mk/platform-<platform>/defaults.mk`, choose its drivers and
programs, and `mk/config.mk` lists every setting with its description.

## 3. Install the toolchain

On an x86_64 Linux host, install the prebuilt LLVM below `build/llvm/`:

```sh
make CONFIGMK=config/config.rpi5.mk llvm-toolchain-cached
```

It downloads the archive of release `rev-0` of the zedBSD repository (the
asset named in `mk/llvm/version.mk`) once to `build/releases/` and unpacks it;
when `build/llvm/bin/clang` is already there it does nothing.  On other hosts,
or to build LLVM yourself from the official LLVM 23.1.0 source and the zedBSD
patch in `mk/llvm/patches/`, run `make CONFIGMK=... toolchain` instead; it
takes much longer.  The host Noct interpreter that runs the image scripts is
built from source below `build/NoctLang/` by the first build that needs it
(`make CONFIGMK=... toolchain-cached` does both at once).

## 4. Build the sysroot

The C library and the headers the kernel and the programs are compiled
against:

```sh
make CONFIGMK=config/config.rpi5.mk sysroot
```

Run it again after the headers in `include/` change: the kernel reads
`build/<platform>/sysroot/usr/include` before the tree's `include/`.

## 5. Accept the firmware licenses

Some platforms need firmware that is not part of zedBSD and carries its own
license.  The Raspberry Pi 5 image needs the boot device trees (Raspberry
Pi's firmware repository, `COPYING.linux`) and the firmware of its BCM43455
Wi-Fi chip (the Cypress licence).  Each one is fetched, checked against its
recorded size and SHA-256, and used only after you have read and accepted its
license.  Settle them before the build, since a parallel build cannot ask:

```sh
make CONFIGMK=config/config.rpi5.mk firmware-licenses
```

Each license is shown in full and asked about once; the answer is kept in
`build/sources/firmware/<package>/license-accepted` and not asked again.
Answering no stops the build.  `yes | make CONFIGMK=... firmware-licenses`
accepts every license without reading it; use it only when you have read
them.

## 6. Build the image

```sh
make CONFIGMK=config/config.rpi5.mk -j8 disk
```

`disk` (also the default target) builds the kernel, the programs, the root
file system, the firmware and the boot files, then the image.  For the
Raspberry Pi 5 the image is `build/rpi5/hdd-image.img`: an MBR disk whose
FAT32 partition holds `config.txt`, `cmdline.txt`, `vmunix`, the device trees
of the Pi 5 and the Pi 500 and their license, followed by the UFS root.  The
bootloader in the board's EEPROM picks the device tree that matches the
board.  Write the image to a microSD card (replace `/dev/sdX` with the card,
not a disk you use):

```sh
sudo dd if=build/rpi5/hdd-image.img of=/dev/sdX bs=4M conv=fsync
```

Narrower targets:

```sh
make CONFIGMK=config/config.rpi5.mk vmunix      # the kernel, build/rpi5/vmunix
make CONFIGMK=config/config.rpi5.mk userland    # the programs
make CONFIGMK=config/config.rpi5.mk rootfs      # the root file system
make CONFIGMK=config/config.rpi5.mk bootloader  # the files of the FAT partition
```

`make help` shows the main targets.  Tools that run their own parallel builds
(the CMake builds) use `BUILD_JOBS` jobs, 16 or the machine's processor count
when that is smaller: `make BUILD_JOBS=4 ...`.

## 7. Diagnostics

- `config.mk: No such file or directory`: give `CONFIGMK=` or copy a file of
  `config/` to `config.mk` (section 2).
- `No rule to make target 'mk/platform-<name>/Makefile'`: the configuration
  names a platform this `Makefile` does not build (section 2).
- `PLATFORM is not set` followed by a prompt: the configuration file does not
  set `PLATFORM`; start from a file of `config/`.
- A missing `clang` or sysroot: run sections 3 and 4.
- `rpi5-boot-firmware: cache must contain exactly 4 entries`: the boot files
  in `build/sources/firmware/rpi5-boot/<revision>/` were fetched by an older
  tree that listed three (before the Pi 500's device tree was added).  Remove
  that `<revision>` directory and build again: the four files are fetched and
  checked, and the license already accepted is not asked again.
- A firmware license was declined: run `make CONFIGMK=... firmware-licenses`
  again and accept it.
