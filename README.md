zedBSD
======

`zedBSD` is a small BSD re-implementation for retro and contemporary
computers. Its hardware abstraction layer keeps the platform-neutral kernel
portable across substantially different machines.

The implemented POSIX/SUS API inventory is maintained in
[`tests/posix-r2-api.csv`](tests/posix-r2-api.csv), with negative/error-path
coverage in [`tests/posix-r2-errors.csv`](tests/posix-r2-errors.csv). These are
implementation matrices, not a claim of complete standards conformance.

Supported targets are NEC PC-9800/i386, IBM PC/AT-compatible i386 and amd64,
Raspberry Pi 4/aarch64, sun4u/sparcv9, and Sharp X68000/m68k. FAT16 remains the
boot filesystem for most targets and normal images carry a separate UFS root
filesystem. X68000 uses its own Human68k-compatible boot image layout. amd64
supports APIC-based SMP with 1, 2, 4, or 8 CPUs; the other targets are currently
uniprocessor.

The primary build commands are:

```sh
make                              # open menuconfig
make ARCH=amd64 world             # build vmunix and rootfs
make ARCH=amd64 disk-image        # build one bootable disk image
make ARCH=amd64 run               # build and start QEMU
make ARCH=amd64 toolchain         # check the selected toolchain
make ARCH=amd64 check             # run public host/compile checks
make ARCH=amd64 list-targets      # list the supported public targets
make help                         # show a short command summary
```

See [`BUILDING.md`](BUILDING.md) for the complete target inventory and artifact
locations. The normal build, QEMU launcher, and `make check` are self-contained
in this repository. Make never reads the ignored `.scripts/` directory; it may
be used locally for shortcuts that invoke the public Make interface.

The UFS1 implementation intentionally accepts a conservative 4.4BSD-family
profile. Read-write allocation is limited to the canonical single-cylinder-
group zedBSD image; incompatible filesystem features are not silently accepted.

## Layout

| Directory | Contents |
|-----------|----------|
| `include/` | Public HAL, kernel, and user ABI interfaces |
| `src/hal/` | Architecture HALs and board support for i386, amd64, aarch64, sparcv9, and m68k |
| `src/kern/` | Platform-neutral kernel services |
| `src/crt/` | Architecture-specific crt0/crt1 startup code |
| `userland/base/` | Base-system commands, libc glue, shell, and network tools |
| `userland/comp/` | Compiler packages |
| `userland/X11/` | X11 server packages |
| `userland/packages/` | Optional language runtimes, editors, and other packages |
| `libc/` | Freestanding libc subset |
| `src/softfloat/` | Soft-float support built from vendor GCC/musl sources |
| `platform/` | Target Makefiles, image tools, and integration for each platform |
| `vendor/` | External source submodules used by selected builds |
| `tools/build/` | Public image, filesystem, and binary-format build tools |
| `tools/` | Build menu, audits, and other public development tools |
| `tests/` | Public host tests and test data |
| `.scripts/` | Ignored local shortcuts; never used by Make |

## License

- `zedBSD` is distributed under the zlib License (see `LICENSE`).
- The soft-float objects are built from GCC libgcc soft-fp sources (LGPL 2.1+
  with a linking exception that permits unrestricted redistribution of linked
  combinations) and musl math sources (MIT). Their license texts (GCC
  `COPYING.LIB`, musl `COPYRIGHT`) accompany binary distributions.
