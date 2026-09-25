zedBSD
=====

`zedBSD` is a modern, re-designed BSD-based kernel and base system
aiming to implement all `POSIX.1-2024` and `Single UNIX Specification
version 4 (SUSv4)` features with a sophisticated architecture. It is
designed and directed by a human developer, and implemented by using
AI coding agents.

It runs on the latest computers. The current focused target is 64-bit
x86 PC and Raspberry Pi series.

## Design Architecture

```
+----------------------------------------------------------------+
| Official Packages (/usr)                                       |
+----------------------------------------------------------------+
| Wayland desktop (/bin/zwl, ...)                                |
+----------------------------------------------------------------+
| Base programs (/bin, /lib)                                     |
+----------------------------------------------------------------+
| /sbin/networkd                                                 |
+----------------------------------------------------------------+
| /sbin/init                                                     |
+----------------------------------------------------------------+
| Drivers (PCI, USB, GPU, disk, ethernet, wifi, filesystem, ...) |
+----------------------------------------------------------------+
| Kernel (platform-neutral)                                      |
+----------------------------------------------------------------+
| HAL (CPU + BSP)                                                |
+----------------------------------------------------------------+
```

The kernel is built on a HAL. It keeps the platform-neutral kernel
completely portable across substantially different machines.

## GPU Support

`zedBSD` has a modern GPU drivers that features a true native Vulkan
stack. It doesn't require Linux DRM/KMS or Mesa.  Currently, Intel
iGPU (Xe-LP) is supported, and NVIDIA/AMD support is planned.

## Retro Computing

For the purpose of a demonstration of the strong compatibility, we
partially support the following:

- NEC PC-9800 i386
- IBM PC/AT i386
- sun4u/sparcv9
- Sharp X68000/m68k

## Building

The complete prerequisite, configuration, image, and QEMU procedure is
in the [build-from-source guide](docs/howto/build-from-source.md).

On amd64 the default disk image is the `native` layout: a GPT disk whose ESP
holds the UEFI loader and the kernel, a read-write UFS root partition, and a
swap partition.  It boots through UEFI (`make run` starts QEMU with OVMF and an
NVMe disk).  The `hybrid` (UEFI and BIOS), `uefi` and `bios` layouts can be
chosen in `make menuconfig`.

The build commands are:

```sh
make                   # equals to disk-image
make menuconfig        # run menuconfig to make config.mk
make disk-image        # build a disk image
make world             # build vmunix and rootfs
make rootfs            # build rootfs
make vmunix            # build vmunix kernel
make run               # build a disk image and start QEMU
make toolchain-cache   # install pinned rev-0 LLVM cache (x86_64 Linux)
make toolchain         # build a toolchain
make help              # show a short command summary
```

## Layout

| Directory            | Description                                            |
|----------------------|--------------------------------------------------------|
| `include/`           | Public HAL, kernel, and user ABI interfaces            |
| `src/hal/`           | Architecture HALs and board support                    |
| `src/kern/`          | Platform-neutral kernel                                |
| `src/drivers/`       | Device and bus driver implementations                  |
| `src/libc/`          | zedBSD `libc`                                          |
| `userland/`          | Userland programs                                      |
| `userland/base/`     | Base programs and libraries (`/bin`, `/lib`)           |
| `userland/comp/`     | Compilers                                              |
| `userland/desktop/`  | Wayland and X11 programs                               |
| `userland/firmware/` | Optional per-device firmware packages                  |
| `userland/packages/` | Third-party packages (`/usr`)                          |
| `platform/`          | Target Makefiles and tools                             |
| `vendor/`            | External programs                                      |
| `tools/`             | Development scripts                                    |
| `tests/`             | Tests                                                  |

## License

- `zedBSD` is distributed under the zlib License (see `LICENSE`).
