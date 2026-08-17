# Post-R2 runtime / POSIX / UFS implementation results

## 1. Scope and revision

This document records the implementation and validation status of
`post-r2-runtime-storage-master.md` and Steps 01 through 15.

- Implementation branch: `codex/post-r2-runtime-storage`
- Validated source revision: `88f16fd87c6b09dec1b3ea8ff4dc28223819760c`
- Baseline revision: `cbdc60c`
- Implementation commits after the baseline: 115
- Test host: `awe@10.0.10.2`
- Test tree: `/home/awe/zedBSD-post-r2`
- Date: 2026-08-17

The result labels used below are:

- **PASS**: the stated automated or QEMU gate was executed successfully.
- **IMPLEMENTED**: code and focused tests exist, but every master-plan gate is not
  necessarily complete.
- **BLOCKED**: the gate requires hardware or external infrastructure unavailable
  during this implementation run.
- **DEFERRED**: the present implementation deliberately provides a smaller zedBSD
  contract than the master plan's final, interoperable form.

## 2. Implementation summary by plan step

| Step | Result | Implemented scope |
|---|---|---|
| 01 baseline/evidence | PASS | Common test scripts, architecture matrix, toolchain collection, hardware manifest and runbook |
| 02 rtld multiarchitecture | PASS | Dynamic loader and shared libc for i386, amd64, aarch64, and sparcv9; QEMU runtime markers on all supported machine targets |
| 03 rtld failure/dlclose | PASS | Loader-state rollback, malformed DSO and OOM injection, recursive `dlopen()`, destructor ordering, TLS release, and mapping release |
| 04 ELF features | PASS | PIE, GNU hash, symbol versions, TLSDESC on applicable 64-bit targets, and RPATH/RUNPATH resolution |
| 05 POSIX/libc conformance | PASS | R2 API/error matrices, LP32/LP64 ABI assertions, stdio locking, environment locking, and resolver locking |
| 06 PTY/terminal | PASS | PTY pairs, termios state, line discipline, controlling-terminal and job-control foundations |
| 07 POSIX timers | PASS | Process timer objects and the `timer_create()` family with lifecycle and signal delivery tests |
| 08 mount/statvfs | PASS | User-facing mount/unmount and filesystem-statistics syscall/libc paths |
| 09 locale/wide character | PASS | Locale objects, multibyte conversion state, wide-character classification and conversion APIs |
| 10 SMP/fault stress | IMPLEMENTED | Test-only fault framework and combined SMP resource stress; short QEMU stress passed |
| 11 hardware regression | BLOCKED | Reproducible PC-98 and amd64 UEFI packages generated; physical-machine execution remains operator work |
| 12 UFS multi-CG/UFS2 | PASS | Multi-cylinder-group UFS1, UFS2 formatter/parser/VFS, host image checks, and QEMU UFS root test |
| 13 UFS consistency | DEFERRED | Synchronous baseline, dependency engine, and recoverable zedBSD journal implemented; canonical interoperable journal, complete mounted soft-updates mode, and 24-hour soak remain |
| 14 UFS metadata features | IMPLEMENTED | Native xattr, POSIX ACL, fixed-table user/group quota, and one persistent block-level snapshot with permission/fault tests |
| 15 integration | PARTIAL | Automated and QEMU matrices pass; hardware, external UFS interoperability, and long soak gates remain open |

## 3. Build and static-check matrix

The following commands were run from `/home/awe/zedBSD-post-r2`:

```sh
for arch in pc98 pcat amd64 arm64 sparcv9; do
    ./build.sh clean "$arch"
    ./build.sh all "$arch" -j2
    ./build.sh check "$arch"
done
```

| Architecture | Clean build | Host/static checks |
|---|---:|---:|
| PC-98 i386 | PASS | PASS |
| PC/AT i386 | PASS | PASS |
| amd64 | PASS | PASS |
| AArch64 | PASS | PASS |
| SPARC V9 | PASS | PASS |

`git diff --check` also completed without an error after the final source changes.

## 4. Runtime and QEMU evidence

### 4.1 General runtime matrix

```sh
scripts/run-regression-matrix.sh runtime
```

- Build/check on all five architecture targets: **PASS**
- POSIX R1 on PC/AT, amd64, AArch64, and SPARC V9: **PASS**
- PC-98 POSIX R1, rerun with the configured PC-98 QEMU and BIOS: **PASS**
- amd64 SMP: **PASS**
- amd64 SMP short stress: **PASS**, 100,008 recorded events

The PC-98 scripts now default to:

```text
$HOME/qemu-pc98/build/qemu-system-i386
$HOME/qemu-pc98/roms/pc98bios
```

### 4.2 Dynamic linker

```sh
scripts/test-dynamic-qemu.sh all
```

Result: **PASS** for PC-98/i386, PC/AT/i386, amd64, AArch64, and SPARC V9.
The suite covers dynamic startup, shared libc, TLS, `dlopen()`/`dlclose()`, loader
failure paths, PIE, GNU hash, versioned symbols, and applicable TLSDESC paths.

### 4.3 POSIX R2

```sh
scripts/test-posix-r2.sh all
scripts/test-posix-r2-remaining.sh all
```

Result: **PASS** for PC-98, PC/AT, amd64, AArch64, and SPARC V9. PC-98 support was
added to both the target selection and runtime-marker path; both scripts are now
executable.

### 4.4 Boot and filesystem

```sh
./build.sh hdd-boot-qemu-test pc98
./build.sh uefi-entry-qemu-test unified
./build.sh sparcv9-ufs-qemu-test sparcv9
```

- PC-98 bare image reaches the zedBSD `/bin/sh` prompt: **PASS**
- amd64 UEFI entry test: **PASS**
- SPARC V9 sun4u UFS1 root plus 64-bit userland test: **PASS**

## 5. UFS feature and fault matrix

| Feature | Current contract | Test result |
|---|---|---:|
| UFS1 | 512-byte sectors, multiple cylinder groups | PASS |
| UFS2 | zedBSD-supported UFS2 format and VFS mutations | PASS |
| Synchronous consistency | Metadata ordering and error propagation | PASS |
| Journal | One durable zedBSD transaction slot (`ZUJ1`) outside the filesystem extent | PASS for replay, bounds, transient error, and persistent poison tests |
| Soft dependencies | Dependency graph and retry-safe drain core | PASS for unit/fault tests; not a complete selectable UFS soft-updates mode |
| xattr | Native inode data, protected system quota attribute | PASS |
| POSIX ACL | Access checks and persistence integration | PASS |
| Quota | Per-mount user/group hard/soft accounting, fixed 64+64 record table | PASS |
| Snapshot | One active persistent block-level CoW snapshot in a reserved append-only region | PASS |

The snapshot suite injects failure at all six persistence boundaries used by create,
copy-on-write publication, flush, and delete/recovery paths. The quota policy xattr
cannot be read, changed, or removed through a user credential path, including root;
only the filesystem backend accesses it directly.

## 6. Hardware-test packages

The packages were produced from the validated revision with `dirty: false` manifests:

```sh
scripts/package-hardware-test.sh pc98 build/hardware-test/pc98
scripts/package-hardware-test.sh amd64-uefi build/hardware-test/amd64-uefi
```

They are stored on the build host under:

- `/home/awe/zedBSD-post-r2/build/hardware-test/pc98/`
- `/home/awe/zedBSD-post-r2/build/hardware-test/amd64-uefi/`

| Package | Bytes | SHA-256 |
|---|---:|---|
| `zedbsd-pc98.img` | 135,266,304 | `8746a93725e57575481ef78804f9670f70c5beda8939d14d5c0bbe4c05cc35ff` |
| `zedbsd-amd64-uefi.img` | 268,435,456 | `e3aad8ffcb1f13637378194a5e2b624672eaed978f82953e2fbcc57497d536b5` |

Each directory also contains `manifest.json` and `RUNBOOK.md`. Physical hardware
results must be added without reclassifying the existing QEMU result as hardware
evidence.

## 7. Open master-plan gates

The following requirements are not represented as PASS:

1. **PC-98 physical hardware regression.** Three cold boots and the boot, VFS,
   keyboard, timer, filesystem, and graphics cases in the runbook remain to be run.
2. **amd64 UEFI physical hardware regression.** Three cold boots plus VFS, SMP,
   timer, filesystem, and dynamic-loader cases remain to be run.
3. **Long-duration stress.** The prescribed nightly 20-seed run and 24-hour soak with
   disk-full, swap-full, and network-loss injection were not run in this session.
4. **Canonical UFS journal interoperability.** `ZUJ1` is a zedBSD extension and must
   not be advertised as FreeBSD SUJ or another canonical UFS journal.
5. **Complete soft updates.** The dependency engine is present, but full mount-option
   integration, memory-pressure draining, fsck-clean crash proof, and SUJ are deferred.
6. **External UFS interoperability.** Images were checked with zedBSD host tools and
   QEMU, but an independent FreeBSD/NetBSD/reference-tool import-export matrix was not
   run for every supported feature.
7. **Feature scalability.** Quota storage is fixed-size and snapshot management is
   limited to one active snapshot with synchronous deletion; production-scale tables,
   multiple snapshots, and resumable background reclamation remain future work.

## 8. Final verdict

The implemented post-R2 candidate is green across the available host checks and QEMU
runtime matrix, including all five architecture targets. It is suitable for source
review and physical-hardware testing.

The master plan is **not certification-complete** until the hardware, long-soak, and
external-interoperability gates above are executed, and until the deliberately limited
journal/softdep contracts are either completed or explicitly accepted as the zedBSD
on-disk extension profile.
