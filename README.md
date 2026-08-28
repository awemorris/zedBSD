zedBSD
======

`zedBSD` is a small BSD re-implementation for retro and contemporary
computers. Its hardware abstraction layer keeps the platform-neutral kernel
portable across substantially different machines.

Supported targets are the following:

- NEC PC-9800 i386
- IBM PC/AT i386
- IBM PC/AT amd64
- Raspberry Pi 4 aarch64
- sun4u/sparcv9
- Sharp X68000/m68k

## Building

The complete prerequisite, configuration, image, and QEMU procedure is in the
[build-from-source guide](docs/howto/build-from-source.md).

The build commands are:

```sh
make                   # equals to disk-image
make menuconfig        # run menuconfig to make config.mk
make disk-image        # build a disk image
make world             # build vmunix and rootfs
make rootfs            # build rootfs
make vmunix            # build vmunix kernel
make run               # build a disk image and start QEMU
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
| `src/crt/`           | Architecture-specific crt0/crt1 startup code           |
| `src/softfloat/`     | zedBSD's integer-only soft-float/compiler runtime      |
| `userland/`          | Userland programs                                      |
| `userland/base/`     | Base programs                                          |
| `userland/comp/`     | Compilers                                              |
| `userland/X11/`      | Xzed programs                                          |
| `userland/packages/` | `/usr/bin` third-party packages                        |
| `libc/`              | zedBSD `libc`                                          |
| `platform/`          | Target Makefiles and tools                             |
| `vendor/`            | External programs                                      |
| `tools/`             | Development scripts                                    |
| `tests/`             | Tests                                                  |

## Standards status

The implemented POSIX/SUS surface and known limitations are tracked by
[WS001](plan/ws001-posix/ws.md). Focused acceptance evidence is owned by each
workstream under `plan/wsXXX-*/tests/`; `make check` is not the project
acceptance interface.

## License

- `zedBSD` is distributed under the zlib License (see `LICENSE`).
