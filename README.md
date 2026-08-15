zedBSD
======

`zedBSD` is a small BSD re-implementation for retro and contemporary
small computers with a nice hardware abstraction layer. The HAL
approach makes zedBSD kernel very portable.

The currently implemented system-call subset is documented in
[`POSIX-R1.md`](POSIX-R1.md). It is an incremental compatibility profile,
not a claim of complete POSIX conformance.

At this moment, it can run on `IBM PC/AT` and `NEC PC-9800` with
i386SX CPU. The disk image runs on both architecture because the boot
sector checks the machine type.

## Layout

| Directory        | Contents                                                                            |
|------------------|-------------------------------------------------------------------------------------|
| `include/`       | Public HAL, kernel, and user ABI interfaces                                         |
| `src/hal/`       | HAL and PC-98 board support                                                         |
| `src/kern/`      | Platform-neutral kernel services                                                    |
| `userland/`      | crt0, libc glue, shell, network tools, and Noct runtime                             |
| `libc/`          | Freestanding libc subset                                                            |
| `softfloat/`     | Soft-float support compiled from the vendor GCC/musl sources                        |
| `platform/pc98/` | PC-9800 target: IPLs, stage 1/2, console, timer, Noct target adapter, DOS installer |
| `apps/`          | Generic Noct programs (`ls.nct`, `cp.nct`, `hello.nct`, `bmpview.nct`)              |
| `userland/noct/noct-upstream/` | NoctLang submodule                                                    |
| `vendor/`        | GCC and musl source submodules, used only by the softfloat build                    |
| `scripts/`       | Build helpers, image installer, QEMU tests                                          |
| `tests/`         | Host tests and QEMU test configurations                                             |

## License

- `zedBSD` is distributed under the zlib License (see LICENSE).
- The soft-float objects are built from GCC libgcc soft-fp sources
  (LGPL 2.1+ with a linking exception that permits unrestricted
  redistribution of linked combinations) and musl math sources (MIT)
  their license texts (GCC COPYING.LIB, musl COPYRIGHT) accompany
  binary distributions.
