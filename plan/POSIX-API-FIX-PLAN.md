# POSIX kernel API correction plan

## Purpose and completion state

This plan consolidates the findings produced by the mandatory kernel-facing
POSIX.1-2017 API review.  All planned corrections have been applied.  The
authoritative per-interface status remains
[`posix-required-kernel-api.csv`](posix-required-kernel-api.csv).

| Work area | Interfaces | Correction | State |
|---|---|---|---|
| Asynchronous I/O | `aio_suspend`, `lio_listio` | Honor timeout waiting and suppress per-request notification for list I/O. | Complete |
| Program execution | `execlp`, `execvp`, `fexecve` | Add the required `ENOEXEC` shell fallback and validate descriptor-backed executable objects before dereferencing an inode. | Complete |
| Thread ABI | `pthread_mutex_init` | Keep the static mutex initializer synchronized with the public mutex object layout. | Complete |
| Process CPU accounting | `clock`, `times` | Account scheduled CPU ticks per process, accumulate reaped-child ticks, and return those values through the syscall boundary. | Complete |
| Calendar formatting | `strftime`, `strftime_l` | Implement ISO week-number and ISO week-year conversions `%V`, `%g`, and `%G`. | Complete |
| Time-zone rules | `tzset` | Parse POSIX standard/daylight names, offsets, and daylight transition rules instead of accepting fixed offsets only. | Complete |

## Implementation sequence

1. Correct libc-only behavioral errors in AIO, PATH execution, mutex
   initialization, calendar formatting, and time-zone parsing.
2. Add process and waited-child CPU counters to the kernel process model.
3. Account running processes from the scheduler tick and roll a reaped child's
   counters into its parent.
4. Add `sched_yield`, `fexecve`, and `times` syscall interfaces and expose the
   required libc declarations and wrappers.
5. Extend the POSIX regression and header-compilation tests.
6. Regenerate the API inventory, reapply review annotations, and require all
   rows to read `implemented` and `reviewed`, with no pending fix.

## Acceptance criteria

- 283 of 283 scoped interfaces are `implemented`.
- 283 of 283 scoped interfaces are `reviewed`.
- Every non-empty review finding is `fixed`; no finding remains pending.
- AMD64 kernel, root filesystem, regression binary, and HDD image build.
- Public POSIX headers compile in both ILP32 and LP64 modes.
- Kernel and libc changes compile for every available zedBSD architecture; a
  missing external cross-toolchain is reported separately from a source error.

## Final verification

| Verification | Result |
|---|---|
| `make ARCH=amd64 build-kernel build-rootfs build/amd64/POSIX-R2-REMAINING.ELF posix-header-check build-boot-disk-image` | Pass |
| `make ARCH=amd64 uapi-abi-layout-check` | Pass for ILP32 and LP64 |
| AMD64 release HDD boot under `qemu-system-x86_64` | Pass; reached `root@zedbsd:/$` |
| AMD64 temporary regression HDD under QEMU | Pass; `R2R:01-09:PASS` |
| `build/amd64/tests/sched-host-test` | Pass, including per-process CPU-tick accounting |
| PC/AT kernel and root filesystem | Pass |
| PC-98 kernel and root filesystem | Pass |
| ARM64 kernel and root filesystem | Pass |
| SPARCv9 build | Not run: configured `sparc64-unknown-elf-gcc` is absent |
| X68k build | Not run: configured `m68k-linux-gnu-gcc` is absent |
| AMD64 libc host test | Not run: the host lacks 32-bit C startup objects and `libgcc` |
| API inventory regeneration and review application | Pass; 283 implemented, 283 reviewed, 11 findings fixed, 0 open |
| `git diff --check` | Pass |

The QEMU regression image was generated separately with the test executable in
its root filesystem.  The normal release HDD image remains free of regression
test binaries.  The CSV is the machine-readable completion record; this
document records the remediation decisions and acceptance gates.
