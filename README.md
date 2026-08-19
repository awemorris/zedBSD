zedBSD
======

`zedBSD` is a small BSD re-implementation for retro and contemporary
small computers with a nice hardware abstraction layer. The HAL
approach makes zedBSD kernel very portable.

The currently implemented system-call subset is documented in
[`POSIX-R1.md`](POSIX-R1.md). It is an incremental compatibility profile,
not a claim of complete POSIX conformance.

The supported targets are NEC PC-9800/i386, IBM PC/AT-compatible i386 and
amd64, Raspberry Pi 4/aarch64, and sun4u/sparcv9. FAT16 remains the boot
filesystem and the normal images carry a separate UFS1 root filesystem.
amd64 supports APIC-based SMP with 1, 2, 4, or 8 CPUs; the other targets are
currently uniprocessor.

Common validation commands are:

```sh
./build.sh check pc98
./build.sh check pcat
./build.sh check amd64
./build.sh check arm64
./build.sh check sparcv9
./scripts/test-posix-r1.sh all
./scripts/test-amd64-smp.sh
./scripts/test-amd64-smp-stress.sh
```

Run `make` without a target to open the curses build menu.  The menu selects
the architecture and board, kernel options, drivers, and installed user
programs.  Kernel, rootfs tree, rootfs image, and boot-disk builds are separate
menu operations; toolchain construction is currently a visible stub. Explicit
`make` targets and the shorter `build.sh` commands remain available for
automation.

The UFS1 implementation intentionally accepts a conservative 4.4BSD-family
profile. Read-write allocation is limited to the canonical single-cylinder-
group zedBSD image; UFS2, journaling, soft updates, ACLs, and extended
attributes are not silently accepted.

## Layout

| Directory        | Contents                                                                            |
|------------------|-------------------------------------------------------------------------------------|
| `include/`       | Public HAL, kernel, and user ABI interfaces                                         |
| `src/hal/`       | HAL and PC-98 board support                                                         |
| `src/kern/`      | Platform-neutral kernel services                                                    |
| `src/crt/`       | Architecture-specific crt0/crt1 startup code                                      |
| `userland/`      | Package Makefiles, libc glue, shell, network tools, and Noct runtime                |
| `libc/`          | Freestanding libc subset                                                            |
| `src/softfloat/` | Soft-float support compiled from the vendor GCC/musl sources                        |
| `platform/pc98/` | PC-9800 target support                                                               |
| `userland/noct/noct-upstream/` | NoctLang submodule                                                    |
| `vendor/`        | GCC and musl source submodules, used only by the softfloat build                    |
| `tools/`         | Build-menu and binary-format build tools                                            |
| `scripts/`       | Legacy, maintenance, and QEMU test scripts                                          |
| `tests/`         | Host tests and QEMU test configurations                                             |

## License

- `zedBSD` is distributed under the zlib License (see LICENSE).
- The soft-float objects are built from GCC libgcc soft-fp sources
  (LGPL 2.1+ with a linking exception that permits unrestricted
  redistribution of linked combinations) and musl math sources (MIT)
  their license texts (GCC COPYING.LIB, musl COPYRIGHT) accompany
  binary distributions.
