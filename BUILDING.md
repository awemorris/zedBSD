# Building zedBSD

The top-level `Makefile` is the public build interface. Running `make` without
a target opens menuconfig. For a non-interactive build, select one of `pc98`,
`pcat`, `amd64`, `arm64`, `sparcv9`, or `x68k` with `ARCH`.

```sh
make
make ARCH=amd64 world
make ARCH=amd64 disk-image
make ARCH=amd64 run
```

## Public targets

| Target | Purpose |
|--------|---------|
| `menuconfig` | Edit the configuration and write `config.mk` |
| `vmunix` | Build `build/<arch>/vmunix` |
| `bootloader` | Build or validate the platform boot components |
| `rootfs-bin` | Build `/bin` from `userland/base`, `userland/comp`, and `userland/X11` |
| `rootfs-usr` | Build `/usr` packages from `userland/packages` |
| `rootfs` | Assemble the complete root filesystem |
| `world` | Build `vmunix` and `rootfs` |
| `disk-image` | Build `world`, `bootloader`, and the bootable disk image |
| `run` | Build `disk-image` and start the platform emulator |
| `toolchain` | Check the compiler and binary tools selected by the platform |
| `check` | Run the public host, ABI, formatter, and compile checks |
| `clean` | Remove `build/<arch>` |
| `distclean` | Remove the complete `build` tree |

`make ARCH=<arch> list-targets` prints this interface and the available focused
checks. `make help` prints common command examples.

Some ILP32 host tests link 32-bit executables. On a 64-bit Debian host this
requires the multilib C development environment (normally `gcc-multilib` and
`libc6-dev-i386`); freestanding kernel and disk-image builds do not require
those host runtime objects.

Intermediate rules are named by the artifact they produce, for example
`build/<arch>/bootloader/stage1.bin` or `build/<arch>/rootfs/.stamp`. They are
implementation details rather than a second set of command aliases; there are
no `internal-*` convenience targets to keep synchronized with the public
interface.

## Artifacts

| `ARCH` | Kernel | Disk image |
|--------|--------|------------|
| `pc98` | `build/pc98/vmunix` | `build/pc98/hdd-image.img` |
| `pcat` | `build/pcat/vmunix` | `build/pcat/hdd-image.img` |
| `amd64` | `build/amd64/vmunix` | `build/amd64/hdd-image.img` |
| `arm64` | `build/arm64/vmunix` | `build/arm64/hdd-image.img` |
| `sparcv9` | `build/sparcv9/vmunix` | `build/sparcv9/hdd-image.img` |
| `x68k` | `build/x68k/vmunix` | `build/x68k/zedbsd-x68k.hd` |

The on-disk format of `vmunix` is platform-specific. In particular, ARM64's
public `vmunix` is the raw Raspberry Pi boot image; its link-time ELF is an
internal `build/arm64/kernel.elf` artifact.

## Platform delegation

The top-level Makefile includes `platform/<arch>/Makefile`. Each platform then
owns these build fragments:

| File | Responsibility |
|------|----------------|
| `vmunix.mk` | HAL, kernel, libc, and userland file rules |
| `bootloader.mk` | The public `bootloader` dependency set |
| `rootfs.mk` | `rootfs-bin`, `rootfs-usr`, and rootfs composition |
| `disk-image.mk` | The public `disk-image` and final artifact |
| `run.mk` | Emulator executable and command-line options |
| `toolchain.mk` | Platform compiler/binutils selection checks |

Common filesystem and binary-format encoders live in `tools/build`. A tool used
by only one platform lives below `platform/<arch>/tools`.

The common BIOS tools are format encoders, not firmware patchers. They finalize
the size/checksum fields in the PC-compatible second-stage loader, wrap a flat
loader as an MZ executable, and assemble or validate a BIOS HDD image. PC-98,
PC/AT, and amd64 share those formats, so the tools remain in `tools/build`.
Header or image processing unique to one target, such as the PC-98 kernel-header
patcher, lives in that platform's `tools` directory.

## Emulators

The emulator command and flags can be overridden with `QEMU` and `QEMU_FLAGS`
on the Make command line. PC-98 uses `qemu-system-i386` from `PATH` by default
and requires a build providing the `pc9821` machine. QEMU has no X68000
machine, so the X68000 `run` target uses MAME through `X68K_EMULATOR`.

Make has no dependency on `.scripts/`. That ignored directory is reserved for
local shorthand commands which call the public targets above.
