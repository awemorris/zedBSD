# Noct userspace, minimal syscall, devfs, and Linux handoff plan

## 0. Document status

- Status: **implemented and verified; uncommitted review requested**
- Written: 2026-08-12
- Authoritative repository: `/home/awe/boots`
- Baseline branch: `kernel`
- Baseline commit observed while writing this plan:
  `5b7148b633afb8a260bf4084b6df5c779cdf1a44 WIP: Move to HAL/Kern configuration`
- The baseline worktree was clean when this plan was written.
- Implementation policy: do not create a commit, do not stage files, and leave
  the complete working-tree diff for review.
- Verification completed on 2026-08-12: full host `check`, ring-3 syscall and
  fault paths, HDD AUTOEXEC/shell, graphical Remacs editing/completion/save,
  and Linux point-of-no-return handoff all passed with the configured PC-98
  QEMU and a copied `~/images` disk image.

This document follows `docs/plan/07-process-thread-elf-init.md`.  That earlier
milestone already supplies process/thread/vmspace objects, timer scheduling,
static ELF32 loading, a ring-3 task, and an `INT 0xc2` probe.  This milestone
turns that probe into a small real syscall ABI, moves Noct into a statically
linked user ELF, provides `/dev/console` and `/dev/graphics` through devfs, and
then makes Linux loading a point-of-no-return operation.

The steps are deliberately ordered so embedded Noct remains available until
the user ELF passes the same CLI, menu, graphics, and Remacs tests.  Do not
remove the embedded implementation early merely to reduce BOOT.SYS size.

## 1. Required end state

The milestone is complete only when all of these statements are true.

1. `NOCT.ELF` is a static, non-PIE ELF32/i386 `ET_EXEC` loaded from the boot
   FAT filesystem into a normal `struct process`, `struct vmspace`, and one
   `struct thread`.
2. `NOCT.ELF` contains its own `_start`, minimal libc, Noct runtime, JIT,
   Boots Noct APIs, Term backend, and BeUI userspace backend.  It includes no
   kernel or HAL private headers and executes no port I/O.
3. Noct obtains files, memory, time, console operations, and graphics only
   through the `INT 0xc2` ABI described below.
4. The initial kernel syscall set is limited to `_exit`, file descriptors,
   cwd, anonymous `mmap`/`munmap`/`mprotect`, `ioctl`, monotonic time, and
   sleep.  `fork`, `execve`, signals, sockets, dynamic linking, and file-backed
   mappings are not added.
5. The synthetic root contains `/disk1`, `/disk2`, ... for every mountable
   partition and also `/dev`.  devfs supplies at least `/dev/console` and
   `/dev/graphics`; these are not FAT directory entries.
6. `/dev/console` supports UTF-8 text output, keyboard input, cursor/screen
   ioctls, and normalized PC-98 key-event ioctls.
7. `/dev/graphics` is exclusive, never maps VRAM to userspace, and supports
   mode entry, capabilities, fill, line, patterned brush fill, blit, flush,
   and UTF-32 glyph bitmap retrieval through ioctls.  It works with both GDC
   and Cirrus without exposing which VRAM organization is active.
8. A child process closes all descriptors on normal exit or user fault.  The
   graphics owner is therefore released even when Noct crashes.  The parent
   can block, collect exit status, reap the thread, destroy the vmspace, and
   run Noct repeatedly without leaking kernel heap or physical pages.
9. Existing commands continue to work.  `noct`, an unqualified `.NCT`/`.NAP`
   application, `emacs`, and `AUTOEXEC.NCT` launch `NOCT.ELF` and wait for it.
   AUTOEXEC's `BOOT_ACTION` behavior is preserved without adding a
   Boots-specific syscall.
10. After parity is proven, the in-kernel Noct interpreter/JIT, temporary Noct
    arena, and user-facing BeUI glue are removed from BOOT.SYS.  Only the
    privileged console and PC-98 graphics/font drivers remain in the kernel.
11. Linux loading validates all inputs before committing, runs only from the
    low loader closure after commit, and never returns to the shell after the
    first Linux destination byte may have overwritten the disposable high
    kernel image/heap.
12. All host tests, staged QEMU tests, final GUI/Remacs/Linux regressions, and
    `git diff --check` pass.  No files are staged and no commit is made.

## 2. Fixed scope and non-goals

Do not implement these facilities in this milestone:

- `fork1()`, `thread_fork()`, `vmspace_fork()`, copy-on-write, or a user
  `fork()` syscall;
- multiple userspace threads, TLS allocation policy, pthreads, or SMP;
- `execve()`, dynamic ELF, `PT_INTERP`, `PT_DYNAMIC`, PIE, shared libraries,
  relocations, demand paging, ASLR, stack growth, or swapping;
- POSIX signals, process groups, sessions, credentials, permissions, pipes,
  sockets, networking, `poll`, or a general character-device hotplug model;
- `brk()`; userspace allocation is based on anonymous `mmap()`;
- file-backed `mmap`, `MAP_SHARED`, or `MAP_FIXED`;
- VRAM, CGROM, or I/O-port mapping into ring 3;
- a general font server, scalable fonts, Unicode shaping, or antialiasing;
- moving the GDC/Cirrus/CGROM drivers out of the kernel;
- changing disk enumeration, partition numbering, FAT on-disk behavior, IDE
  geometry, boot-sector formats, or the Stage 1 handoff ABI.

The first implementation is deliberately single-CPU and single-user-process
at a time.  Structures must not make future work impossible, but do not add
unused abstraction for future `fork()` or SMP.

## 3. Baseline facts which the implementation must respect

- `src/kern/exec.c` currently has `process_spawn_init()` and builds only
  `argc=1`, one `argv[0]`, and an empty environment.
- `src/kern/thread.c::thread_wait()` returns `EBUSY` instead of sleeping and
  is the only place which destroys a completed HAL task.
- `src/kern/process.c::exit1()` marks the process zombie but does not close
  descriptors, wake a parent, or reap anything.
- `src/kern/vmspace.c` maps only a caller-selected, page-aligned anonymous
  range.  It has no address search, unmap, protection-change, or uaccess
  validation API.
- `src/hal/i386/int.c` converts `INT 0xc2` into a temporary observation
  callback containing only EAX, while `struct interrupt_frame` already holds
  all six argument registers and permits EAX to be changed before `iret`.
- `include/hal/hal.h` declares an unused `hal_syscall_set_handler()` signature
  which does not match the implemented probe.  Replace that declaration; do
  not add a second parallel syscall registration mechanism.
- `src/kern/file.c` has no open callback invocation and no public ioctl
  wrapper.  `struct filedesc` cannot remove/close one descriptor or install at
  an exact descriptor number.
- `src/kern/mount.c::mount()` has a generic-looking signature but
  unconditionally interprets `data` as `struct fat_mount_args` and requires a
  disk.  It must be generalized before devfs can use it.
- `src/noct/platform.c` and `src/noct/pc98-beui.c` directly call HAL, VFS,
  port-I/O, and framebuffer ownership functions.  None of that code is safe
  in ring 3 as written.
- With `NOCT_TARGET_PC98BE`, upstream `noct/src/core/jit.c` allocates JIT code
  with `malloc()` and does not call `mprotect()`.  The userspace build must be
  a freestanding POSIX target, not the embedded-PC98BE target.
- The linker places the low loader closure at physical `0x20000..0x80000` and
  the disposable high image at physical `0x100000` and above.  The fixed
  512-KiB `.kernel_heap` remains high even after embedded Noct is removed.

## 4. User/kernel ABI fixed by this plan

### 4.1 System-call register convention

Keep vector `0xc2` and use exactly this i386 ABI:

| Register | Meaning |
|---|---|
| EAX | syscall number on entry; signed result on return |
| EBX | argument 0 |
| ECX | argument 1 |
| EDX | argument 2 |
| ESI | argument 3 |
| EDI | argument 4 |
| EBP | argument 5 |

Successful calls return a nonnegative `intptr_t`.  Failures return `-errno`
in EAX.  The userspace wrapper converts a negative result to `-1` (or
`MAP_FAILED`), stores the positive value in `errno`, and preserves successful
zero.  Never return a kernel pointer.  All userspace addresses remain below
`0x80000000`, so a successful mmap result cannot be confused with a negative
errno.

`int.c` enters through an interrupt gate with IRQs disabled.  Initial syscall
handlers must not casually enable interrupts inside VFS/device critical
sections.  Blocking calls use the existing scheduler explicitly; a switched
task restores its own IF state.  Audit this policy again before adding SMP or
preemptible kernel execution.

### 4.2 Stable syscall numbers

Create `include/uapi/boots/syscall.h` and assign these values once.  Do not
renumber them while implementing later phases.

```c
enum boots_syscall_number {
        BOOTS_SYS_exit = 1,
        BOOTS_SYS_open = 2,
        BOOTS_SYS_close = 3,
        BOOTS_SYS_read = 4,
        BOOTS_SYS_write = 5,
        BOOTS_SYS_lseek = 6,
        BOOTS_SYS_fstat = 7,
        BOOTS_SYS_getdents = 8,
        BOOTS_SYS_chdir = 9,
        BOOTS_SYS_getcwd = 10,
        BOOTS_SYS_mmap = 11,
        BOOTS_SYS_munmap = 12,
        BOOTS_SYS_mprotect = 13,
        BOOTS_SYS_ioctl = 14,
        BOOTS_SYS_clock_gettime = 15,
        BOOTS_SYS_nanosleep = 16,
};
```

Unknown numbers return `-ENOSYS`.  `exit` never returns.  `open` is the
initial ABI; do not add `openat` until directory descriptors are needed by
user programs.  `off_t` remains the repository's current signed 32-bit type
for this milestone.  A future 64-bit offset ABI must use a new syscall rather
than silently changing this one.

### 4.3 Shared UAPI rules

Add `include/uapi/boots/console.h`, `include/uapi/boots/graphics.h`, and
`include/uapi/boots/dirent.h`.  Add `include/uapi` to both kernel and userspace
include paths.  These headers may include only fixed-width C/POSIX public
types.  They must not include `kern/*`, `hal/*`, or Noct headers.

- Use `uint32_t` for user addresses inside ioctl request structures, never
  `void *`, `size_t`, enums with compiler-dependent width, or kernel pointers.
- Use explicit reserved fields and require callers to initialize them to
  zero.
- Define ioctl numbers with `_IO`, `_IOR`, `_IOW`, and `_IOWR` macros in the
  new `libc/include/sys/ioctl.h`.
- Every kernel ioctl implementation must check the exact command, structure
  size, reserved fields, dimensions, multiplication overflow, and user range.
- Colors are target-independent `0x00RRGGBB` as already documented by BeUI.

### 4.4 mmap version-one contract

Expose the POSIX signature and constants in `libc/include/sys/mman.h`, but
support only:

- `MAP_PRIVATE | MAP_ANONYMOUS`, `fd == -1`, `offset == 0`;
- `PROT_NONE`, `PROT_READ`, `PROT_WRITE`, and `PROT_EXEC` combinations;
- `addr == NULL` first-fit allocation, or a nonbinding page-aligned hint;
- full-mapping `munmap()` and full-mapping `mprotect()`.

Return `EOPNOTSUPP` for file mappings, `MAP_SHARED`, and unsupported flags;
return `EINVAL` for `MAP_FIXED`, zero length, non-page-aligned explicit
addresses, partial unmap, or partial protection changes.  These full-region
restrictions are intentional because `struct vm_region` owns one contiguous
`hal_pmem` allocation.  Do not fake partial freeing.  Noct's allocator and JIT
must request and release whole mappings, so this contract is sufficient for
the milestone and can later be extended by changing vm-region backing.

Reject a simultaneous writable+executable request.  The JIT lifecycle is RW,
then `mprotect(..., PROT_READ | PROT_EXEC)`, and back to RW only when Noct
explicitly requests it.  On i386 without NX, execute-disable may not be
hardware-enforced, but write protection must be enforced and the logical
protection state must remain accurate.

Use `0x10000000` as the first-fit mmap search base and stop below a guard gap
under `vmspace->stack_bottom`.  Never map page zero, ELF segments, or the
stack.  Round length upward with checked arithmetic.

## 5. Planned file layout

### 5.1 New shared/public headers

- `include/uapi/boots/syscall.h`: syscall numbers and result convention.
- `include/uapi/boots/dirent.h`: fixed `struct boots_dirent` and `DT_*` values.
- `include/uapi/boots/console.h`: console event and ioctl request structures.
- `include/uapi/boots/graphics.h`: graphics modes, formats, capability flags,
  rectangles, images, glyph result, and ioctl request numbers.
- `libc/include/sys/ioctl.h`: ioctl encoding macros and `ioctl()` declaration.
- `libc/include/sys/mman.h`: mmap constants, `MAP_FAILED`, and declarations.
- `libc/include/dirent.h`: `DIR`, public `struct dirent`, and directory API.

### 5.2 New kernel files

- `include/kern/syscall.h`, `src/kern/syscall.c`: dispatcher and syscall
  implementations.
- `include/kern/uaccess.h`, `src/kern/uaccess.c`: `user_range_check()`,
  `copyin()`, `copyout()`, and `copyinstr()`.
- `include/kern/cdev.h`, `src/kern/cdev.c`: minimal character-device registry.
- `include/kern/devfs.h`, `src/kern/devfs.c`: nodev filesystem mounted at
  `/dev`.
- `include/kern/console-device.h`, `src/kern/console-device.c`:
  `/dev/console` cdev.
- `include/kern/graphics.h`, `src/kern/graphics.c`: platform-independent
  graphics cdev, exclusive-open state, ioctl validation, bounded copy loops,
  and the internal graphics-backend contract.
- `src/kern/pc98/graphics.c`: privileged wrapper around the existing
  GDC/Cirrus auto backend and framebuffer ownership.
- `src/kern/pc98/font.c`: Unicode-to-JIS/CGROM glyph extraction used by
  `BOOTS_GRAPHICS_GET_GLYPH` without exposing CGROM to userspace.
- `include/kern/process-result.h`, `src/kern/process-result.c`: the optional
  fd-3 bounded result sink used to return AUTOEXEC's `BOOT_ACTION` with normal
  `write()` rather than a private syscall.

### 5.3 New userspace files

- `user/crt0.S`: `_start`, stack decoding, libc initialization, `main`, exit.
- `user/libc/syscall.c`: six-argument `int $0xc2` wrappers.
- `user/libc/errno.c`: userspace `errno` storage.
- `user/libc/unistd.c`, `fcntl.c`, `stat.c`, `dirent.c`, `mman.c`, `ioctl.c`,
  and `time.c`: thin POSIX wrappers.
- `user/libc/heap-init.c`: choose an anonymous heap arena and initialize the
  existing `struct boots_heap` allocator before `main()`.
- `user/libc/stdio.c`: fd-backed `FILE`, stdin/stdout/stderr, formatted output,
  and the subset used by Noct.  It must not include or link `libc/stdio-fs.c`.
- `user/libc/stdlib.c`: `exit`, `_exit`, `abort`, assertions, and environment
  ownership (`environ`, `getenv`, `setenv`, `unsetenv`).
- `user/noct/main.c`: command-line parsing, source/bytecode loading, VM
  lifecycle, API registration, result-fd emission, and final exit status.
- `user/noct/napi.c`, `user/noct/napi.h`: userspace version of the Boots
  Console/Screen/Keyboard/System APIs.
- `user/noct/term.c`, `user/noct/term.h`: Noct Term backend using console
  ioctls and POSIX file/directory APIs.
- `user/noct/beui.c`, `user/noct/beui.h`: Noct BeUI HAL using
  `/dev/graphics`, `/dev/console`, and `clock_gettime()`.
- `platform/pc98/noct-user.ld`: page-aligned static user ELF layout.
- `tests/user-syscall.S`, `tests/user-uaccess.S`, `tests/user-mmap.c`,
  `tests/user-console.c`, and `tests/user-graphics.c`: focused ring-3 probes.
- New bounded QEMU scripts described in section 19.

### 5.4 Files modified during integration

- `include/hal/hal.h`, `src/hal/i386/int.c`: real syscall callback and EAX
  return.  `src/hal/i386/int.h` changes only if names/types must be exposed to
  `int.c`; the frame layout remains unchanged.
- `include/kern/vmspace.h`, `src/kern/vmspace.c`: address search, unmap,
  protection, and range validation.
- `include/kern/process.h`, `src/kern/process.c`: parent wait/wakeup, exit
  cleanup, result sink, and process enumeration/quiescence queries.
- `include/kern/thread.h`, `src/kern/thread.c`: blocking wait and safe zombie
  reaping; no CPU-context clone.
- `include/kern/exec.h`, `src/kern/exec.c`: general `process_spawn()` and full
  initial `argv`/`envp` stack.
- `include/kern/file.h`, `src/kern/file.c`: file open callback, refcount-safe
  close, ioctl wrapper, and carefully scoped pseudo-file construction.
- `include/kern/filedesc.h`, `src/kern/filedesc.c`: close/take/install-at and
  descriptor teardown.
- `include/kern/mount.h`, `src/kern/mount.c`: `FILESYSTEM_NODEV` and devfs
  mount handling.
- `include/kern/vfs.h`, `src/kern/vfs.c`: cdev registration, devfs
  registration/mount, then FAT partition mounts and boot cwd selection.
- `include/kern/platform.h`, `src/kern/pc98/platform.c`: connect the PC-98
  graphics backend to `src/kern/graphics.c`; do not expose the backend to
  userspace.
- `src/kern/user-probe.c` and its header/tests: retain user-fault coverage but
  retire the temporary syscall-observation callback after the real dispatcher
  test replaces it.
- `src/kern/entry.c`: install syscall handler; later remove Noct-arena
  reservation.
- `src/kern/main.c`, `src/kern/shell.c`, `src/kern/startup.c`: spawn/wait
  `NOCT.ELF` at the existing user-visible call sites.
- `libc/include/errno.h`, `unistd.h`, `time.h`, `stdlib.h`, `stdio.h`,
  `fcntl.h`, and `sys/stat.h`: declarations/constants needed by the user libc.
- `libc/libc.mk`, `noct.mk`, `Makefile`, and `platform/pc98/platform.mk`:
  separate kernel, host, and user object sets; link/install `NOCT.ELF`; remove
  embedded objects only at the parity gate.
- Image creation/install scripts: add `NOCT.ELF` without changing CHS or
  partition construction.
- `src/kern/pc98/linux-boot.c`, `src/kern/pc98/linux-entry.S`, and the Linux
  call path only in the final point-of-no-return phase.

### 5.5 Files removed only after the parity gate

Move/refactor useful code first, then delete these kernel-only integration
files after all userspace Noct tests pass:

- `include/kern/noct.h`;
- `src/noct/noct.c` and `src/noct/noct-m6-script.h`;
- `src/noct/memory.c`, `src/noct/memory.h`;
- `src/noct/napi.c`, `src/noct/napi.h` after their userspace replacements
  exist;
- `src/noct/platform.c`, `src/noct/platform.h`;
- `src/noct/target.c`, `src/noct/target.h` after `user/noct/term.c` works;
- `src/noct/pc98-beui.c`, `src/noct/pc98-beui.h` after privileged display
  code is in `src/kern/pc98/graphics.c`.

Do not delete `libc/stdio-fs.c` merely because it leaves BOOT.SYS.  Keep it for
existing host compatibility tests until a separate cleanup proves there are
no remaining users.  Exclude it from the final kernel/user link if unused.

## 6. Phase A: replace the trap probe with the syscall ABI

1. In `include/hal/hal.h`, replace the current unimplemented
   `void *(*)(int, void *[])` declaration with a typed callback returning
   `intptr_t` and receiving a number plus six `uintptr_t` arguments.  Set
   `HAL_SYSCALL_ARGS` to 6.  Keep the HAL ignorant of process, errno, VFS, and
   userspace ABI structures.
2. In `src/hal/i386/int.c`, store that handler, extract EAX/EBX/ECX/EDX/ESI/
   EDI/EBP from `struct interrupt_frame`, call it only for DPL3 vector `0xc2`,
   and assign its result to `fp->regs.eax`.  A missing handler returns
   `-ENOSYS` or stops during early boot; choose one behavior and test it before
   enabling user tasks.
3. Do not change `src/hal/i386/trap.S`: `pushal` already saves every required
   register and `popal` restores the EAX value changed in the frame.  Change
   assembly only if a disassembly/frame-offset test proves this assumption
   false.
4. Add `src/kern/syscall.c::syscall_dispatch()` with a fixed table/switch and
   one small handler per number.  `syscall_init()` registers it from
   `kernel_entry()` after process/scheduler initialization and before the
   first user process starts.
5. Initially implement only `exit` and `write` to fd 1/2, enough for
   `tests/user-syscall.S`.  Add the remaining handlers in later phases; all
   unimplemented slots return `-ENOSYS` rather than halting.
6. Replace the `INT 0xc2` probe assertion with an actual return-value test.
   Keep the fault observation path until the normal fault-to-exit/reap test is
   converted.

Gate A: a ring-3 ELF writes a marker through fd 1, verifies EAX return, calls
`exit(37)`, is reaped, and the kernel GUI still accepts a key.

## 7. Phase B: uaccess and syscall safety

Implement `src/kern/uaccess.c` before accepting paths, buffers, or ioctls.

- `user_range_check(vm, address, length, required_prot)` walks across as many
  adjacent `vm_region` objects as necessary.  It rejects wraparound, page
  zero, kernel-half addresses, holes, and insufficient protection.
- `copyin()` requires read permission; `copyout()` requires write permission.
  Copy in bounded chunks and return `EFAULT`, never a page fault or panic.
- `copyinstr()` stops at NUL, returns the observed length if requested, and
  returns `ENAMETOOLONG` when the supplied maximum is exhausted.  Syscall
  paths use `PATH_MAX` for paths and device-specific caps for text/image data.
- Never validate a pointer and later dereference it after switching to another
  process.  In this single-CPU milestone, syscall copy/operation/copyout runs
  against `curthread->proc->vmspace` without switching away except at an
  explicit sleep boundary.
- File reads/writes use a small kernel bounce buffer, not direct user pointers.
  This also bounds time spent in a filesystem operation and prevents a drive
  from retaining a user address.

Add host tests for zero length, exact end, overflow, kernel address, a hole,
read-only copyout, adjacent regions, and strings crossing a page/region edge.
Add a QEMU test where an invalid write buffer returns `EFAULT` and the same
process can make another valid syscall afterward.

## 8. Phase C: process spawn, exit, wait, and result channel

### 8.1 General spawn

Replace the init-only path with:

```c
int process_spawn(const char *path, char *const argv[], char *const envp[],
                  unsigned flags, struct process **result);
```

Keep `process_spawn_init()` temporarily as a thin compatibility/test wrapper;
remove it or make it test-only after `kernel_main()` stops auto-launching the
old looping `INIT.ELF`.

`exec_build_initial_stack()` must accept counted `argv` and `envp`, enforce
caps (recommended: 32 arguments, 64 environment entries, 16 KiB combined
strings/pointers), detect all arithmetic overflow, and build the conventional
i386 stack:

```text
argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL
```

No aux vector is required.  Preserve 16-byte stack alignment at `_start`.
Build the image completely while unpublished; on every failure close the ELF
file and destroy filedesc/cwd/vmspace/process objects without publishing a PID
or runnable thread.

### 8.2 Standard descriptors

After devfs exists, open `/dev/console` separately for fd 0, 1, and 2 using
read-only, write-only, and write-only modes.  Add
`filedesc_install_at(fd, file, number)` and reject replacement unless the
caller explicitly closed/took the old file.  Do not share one `struct file`
until file refcounting is proven.

### 8.3 Blocking wait and reaping

Add `process_wait(struct process *child, int *status, char *result,
size_t result_capacity)` for process0's synchronous launcher.

- A running child records one parent waiter under IRQ exclusion and puts the
  current kernel thread to `sched_sleep(0)`.
- `exit1()` stores status, closes/destroys its filedesc and releases cwd
  references while the address space is still valid, marks the process
  zombie, wakes the waiter, and calls `thread_exit()`.
- The waiter calls `thread_wait()` only after the child thread is zombie.
  `thread_wait()` remains the sole code which clears HAL private data,
  destroys the non-current HAL task/kernel stack, detaches the thread, and
  decrements `thread_count`.
- Only after all threads are detached may `process_free_mem()` destroy the
  vmspace and process object.
- Fix the lost-wakeup window by checking state, registering the waiter, and
  sleeping while IRQs are excluded.  Do not spin in the GUI thread.
- A user fault goes through the same `exit1()` and wake/reap path.

Only one userspace thread and one waiter per child are supported now.  Encode
that invariant and return `EBUSY` for a second waiter.

### 8.4 AUTOEXEC result without a private syscall

Reserve child fd 3 only when `PROCESS_SPAWN_RESULT` is requested.  A bounded
pseudo-file writes into storage owned by the child process object.  Add a
carefully scoped `file_create_pseudo()` helper; pseudo files have `f_inode ==
NULL`, explicit ops, normal refcount/close behavior, and are never visible in
devfs.

Set `BOOTS_RESULT_FD=3` in the child's environment.  At the end of
`user/noct/main.c`, read its own `BOOT_ACTION`; if set, validate the local
length and `write(3, value, length)`.  The parent obtains the bounded bytes
from `process_wait()` before freeing the process and applies the existing
`valid_boot_action()` validation.  This preserves AUTOEXEC semantics using
ordinary POSIX `write()` and avoids a `bootctl` syscall or FAT temporary file.

Gate C: run the syscall test process 100 times, including normal exit and
fault exit, and prove stable kernel heap/pmem counts and graphics-independent
GUI responsiveness.

## 9. Phase D: VM syscalls and userspace heap

Extend `vmspace` with these internal functions:

```c
int vmspace_map_find(struct vmspace *, uintptr_t hint, size_t size,
                     uint32_t prot, uintptr_t *mapped);
int vmspace_unmap(struct vmspace *, uintptr_t start, size_t size);
int vmspace_protect(struct vmspace *, uintptr_t start, size_t size,
                    uint32_t prot);
int vmspace_check(struct vmspace *, uintptr_t start, size_t size,
                  uint32_t required_prot);
```

Keep `vmspace_map_anon()` for the ELF loader and stack builder.  Insert regions
in address order rather than at an arbitrary list head so gap search and
uaccess are deterministic.  Translate POSIX protections to HAL flags only in
`sys_mmap`/`sys_mprotect` or one named conversion helper.

`vmspace_unmap()` and `vmspace_protect()` initially require an exact region.
On unmap: call `hal_page_unmap`, flush if required by the HAL contract, free
pmem, unlink metadata, and free it.  On protection failure leave the old
metadata unchanged.  On success update `region->prot` only after
`hal_page_prot()` succeeds and flush the active space TLB.

The userspace allocator reuses `libc/heap.c` but owns a userspace
`struct boots_heap`.  `_start` calls `boots_user_heap_init()` before `main()`;
it tries whole anonymous arenas in descending sizes appropriate for available
systems (for example 32, 16, 8, 4, then 2 MiB) and initializes the first
successful one.  It must leave enough physical memory for the JIT mapping and
kernel.  Do not call kernel `kern_malloc` or reserve a physical address from
userspace.  A later allocator may grow across arenas; the first version may
use one arena and return `ENOMEM` when exhausted.

Gate D: a C user ELF allocates/frees/reallocates, maps an independent JIT
region, writes code while RW, switches it to RX, executes it, switches back to
RW, unmaps the whole range, and exits.  A write after RX must cause only that
process to fault.  Unsupported/partial mmap operations return the documented
errors.

## 10. Phase E: file and time syscalls

Implement syscall handlers as thin validation/adaptation layers over existing
kernel functions:

- `open(path, flags, mode)`: `copyinstr`, validate known flags, call
  `file_openat(curthread->proc->cwdi, ...)`, then install the lowest fd.
- `close(fd)`: atomically take the descriptor from `filedesc`, then close it.
- `read`/`write`: validate fd/access, loop through a bounded bounce buffer,
  preserve partial-result semantics, and return bytes or the first error.
- `lseek`: call `file_seek`; directories/devices may reject it.
- `fstat`: call `inode_getattr` and `copyout` the existing public `struct stat`.
  Pseudo files synthesize a character-device-like stat if needed.
- `getdents`: return zero at EOF or one/more fixed `struct boots_dirent`
  records that fit the supplied buffer.  libc `readdir()` owns one record and
  hides this private syscall format.
- `chdir`/`getcwd`: use the process's `cwdinfo`, never global
  `kern_cwdinfo`.  `getcwd` copies the string including NUL and returns the
  user buffer address as POSIX requires.
- `ioctl`: resolve the fd and pass the command plus raw user address to the
  device file op; the owning cdev performs command-specific copyin/copyout.
- `clock_gettime`: initially support `CLOCK_MONOTONIC` from scheduler/HAL
  ticks.  Use a fixed public `struct timespec` and checked conversion.
- `nanosleep`: validate the interval, round up to at least one scheduler tick,
  call `sched_sleep(sched_ticks() + ticks)`, and return zero after wake.  No
  signal remainder behavior is needed yet.

Implement libc `stat()` as open/fstat/close for the current no-symlink VFS,
and `access(F_OK)` similarly.  Do not add separate syscalls for them.

Gate E: from ring 3, list `/`, `/dev`, and `/disk1`; chdir to `/disk1`; read a
script; create/write/read a test FAT file on a disposable image; test bad fd,
bad path, read-only write, EOF, and invalid pointers.

## 11. Phase F: cdev and devfs

### 11.1 Character-device contract

Define a small internal interface in `include/kern/cdev.h`:

```c
struct cdev_ops {
        int (*open)(struct file *);
        int (*close)(struct file *);
        ssize_t (*read)(struct file *, void *, size_t);
        ssize_t (*write)(struct file *, const void *, size_t);
        int (*ioctl)(struct file *, unsigned long, uintptr_t user_arg);
};

int cdev_register(const char *name, dev_t rdev,
                  const struct cdev_ops *ops, void *data);
const struct cdev *cdev_find(const char *name);
```

Use a fixed registry sufficient for this milestone.  Reject duplicate names,
invalid names, and registration after devfs mount if the implementation does
not support live additions.  A devfs inode stores its cdev in `i_data`, uses
`INODE_CHAR`, `S_IFCHR`, and `i_rdev`, and routes file operations through one
devfs adapter.  The cdev's per-open state goes in `file->f_data`.

Add `file_ops.open` and call it only after `struct file` is initialized.  On
open failure, release the inode and file slot completely.  Make
`file_close()` honor `f_usecount`, call close only on the last reference, and
support pseudo files with no inode.

### 11.2 nodev mount

Add `FILESYSTEM_NODEV` to `struct filesystem_type`.  Generalize `mount()` so
an exact nodev type may use `data == NULL`, `m_disk == NULL`, and no probe or
`disk_open()`.  FAT retains its current fspec/disk/probe path unchanged.
`unmount()` must skip `disk_close()` when `m_disk == NULL`.

`devfs.c` implements only root lookup, getattr, and readdir.  It creates a
stable inode for each registered cdev and no regular-file create/unlink
operations.  Register cdevs and `devfs_type`, mount rootfs, then mount devfs at
`/dev`, and only then mount FAT partitions.  If devfs cannot mount, fail VFS
initialization: userspace Noct cannot run safely without its devices.

Gate F: host VFS tests show `/dev`, `/dev/console`, and `/dev/graphics` with
correct directory/type/stat data, while all prior `/diskN`, FAT, mount-crossing,
cwd, and partition tests remain unchanged.

## 12. Phase G: `/dev/console`

Register `console` before devfs is mounted.

- `write()` accepts UTF-8 and calls HAL console output.  Copy user data through
  syscall bounce buffers.  Preserve up to three trailing bytes across chunks
  so a UTF-8 sequence is never split into two invalid HAL calls.
- `read()` blocks until an ASCII/control key can be returned as bytes.  Rich
  editing keys are obtained through event ioctls, not lossy byte translation.
- Multiple opens are allowed.  Console cursor/screen state remains global in
  the first version; serialize each operation with IRQ exclusion.

Define at least these ioctls:

- `BOOTS_CONSOLE_GET_SIZE` -> rows and columns;
- `BOOTS_CONSOLE_CLEAR`;
- `BOOTS_CONSOLE_CLEAR_ROW`;
- `BOOTS_CONSOLE_CLEAR_TO_EOL`;
- `BOOTS_CONSOLE_GET_CURSOR` and `BOOTS_CONSOLE_SET_CURSOR`;
- `BOOTS_CONSOLE_SHOW_CURSOR`;
- `BOOTS_CONSOLE_WRITE_AT`: row, column, attribute, user UTF-8 address/length;
- `BOOTS_CONSOLE_POLL_EVENT`: nonblocking normalized event or `EAGAIN`;
- `BOOTS_CONSOLE_READ_EVENT`: blocking normalized event;
- `BOOTS_CONSOLE_KEY_STATE` and `BOOTS_CONSOLE_DRAIN_INPUT` for BeUI.

Add `EAGAIN` and any other actually used errno values to `errno.h`; do not
reuse an unrelated error.  Keep event codes compatible with the current
`HAL_KEY_EVENT_*` representation, but define them independently in the UAPI
header so userspace never includes `hal.h`.

Gate G: a user test writes UTF-8/Japanese, moves and hides the cursor, writes
at a position with attributes, polls and reads arrows/modifiers, then exits
with the kernel terminal state restored.

## 13. Phase H: `/dev/graphics`

### 13.1 Ownership and mode lifecycle

Opening `/dev/graphics` reserves the one graphics owner but does not switch
mode.  A second open returns `EBUSY`.  `BOOTS_GRAPHICS_ENTER` receives
`preferred_bits_per_pixel` (zero means backend default), calls the existing
auto backend, clears VRAM before scanout exactly as the current verified path
does, and returns actual width/height/bpp/stride/capabilities.

`close()` calls backend leave when entered, clears `fb_set_active`, releases
ownership, and restores text visibility.  It must be safe from `exit1()` and
the user-fault path.  Kernel fallback UI must not draw while a user process
owns graphics; process0 waits for the child, so do not add a second compositor.

### 13.2 Required ioctl operations

Define these UAPI operations and map them to the current native display HAL:

- `BOOTS_GRAPHICS_GET_CAPS`;
- `BOOTS_GRAPHICS_ENTER` and `BOOTS_GRAPHICS_GET_MODE`;
- `BOOTS_GRAPHICS_FILL_RECT`;
- `BOOTS_GRAPHICS_DRAW_LINE`;
- `BOOTS_GRAPHICS_PATTERN_FILL` (the requested brush operation, using the
  existing 64-bit pattern semantics);
- `BOOTS_GRAPHICS_BLIT` supporting indexed 8-bit, RGB24, and 1-bit mono with
  caller-supplied foreground/background colors;
- `BOOTS_GRAPHICS_BLIT_PATTERN` if required by the current BeUI image path;
- `BOOTS_GRAPHICS_FLUSH` with zero rectangles meaning whole display;
- `BOOTS_GRAPHICS_GET_GLYPH` for one UTF-32 codepoint.

All coordinates are unsigned logical pixels and clipped/rejected consistently
before reaching a backend.  Cap rectangle count and dimensions.  Do not copy
a 640x480 image into kernel heap.  For blit, copy one bounded source row at a
time into kernel scratch memory, construct a one-row native image, and draw it
at the appropriate destination.  Copy a maximum 256-entry palette once after
validating its count.  Preserve source stride and checked byte calculations.

### 13.3 UTF-32 glyph contract

`BOOTS_GRAPHICS_GET_GLYPH` takes one Unicode scalar value and returns:

- width (8 or 16 for the initial PC-98 font), height (16), advance/bearings;
- 1-bit-most-significant-bit-first bitmap format and stride;
- at most 32 bitmap bytes copied to a fixed user buffer.

Reject surrogates/out-of-range values and use the same fallback glyph policy
as the console.  `src/kern/pc98/font.c` owns Unicode-to-JIS conversion, CGROM
selection/read timing, and a small kernel cache.  Extract/adapt the already
working algorithm; do not call CGROM ports from user code and do not modify
the Noct submodule solely to expose a currently static helper.

The userspace BeUI glyph backend calls GET_GLYPH and then BLITs the 1-bit image
with foreground/background colors.  Thus font retrieval is a real ioctl test,
and text rendering remains in userspace.

Gate H: GDC and Cirrus QEMU tests cover default 8-bpp behavior, explicit
24-bpp hint where Cirrus supports it, GDC fallback, fill/line/pattern/image,
Japanese glyph retrieval/drawing, flush, exclusive open, close restoration,
and crash restoration.

## 14. Phase I: minimal userspace libc and crt0

### 14.1 Sources reused without kernel coupling

Compile these existing pure sources into both kernel/host and user libraries
as needed: `libc/string.c`, `ctype.c`, `int64.c`, `strto.c`, and `format.c`.
Compile `libc/heap.c` into the user library with the userspace heap initializer.
Remove the obsolete `noct_pc98be_*` aliases only after the embedded build is
gone.  Do not link `libc/stdio.c` or `libc/stdio-fs.c` into `NOCT.ELF`; use the
new fd-backed user implementation.

### 14.2 Required libc surface

The first supported headers/functions are:

- process: `_exit`, `exit`, `abort`;
- descriptors: `open`, `close`, `read`, `write`, `lseek`, `fstat`, `isatty`,
  `fileno`;
- paths/directories: `access(F_OK)`, `stat`, `chdir`, `getcwd`, `opendir`,
  `readdir`, `closedir`;
- memory: `mmap`, `munmap`, `mprotect`, `malloc`, `calloc`, `realloc`, `free`,
  `strdup`;
- device: `ioctl`;
- time: `clock_gettime(CLOCK_MONOTONIC)`, `nanosleep`, `time` if Noct still
  references it;
- stdio: stdin/out/err, `fopen`, `fclose`, `fflush`, `fread`, `fwrite`,
  `fseek`, `ftell`, `fgets`, `getc`, `printf`, `fprintf`, `snprintf`,
  `vsnprintf`, `putchar`, `puts`, `ferror`, `clearerr`;
- environment: `environ`, `getenv`, `setenv`, `unsetenv`;
- existing pure string/ctype/numeric/soft-float support required by Noct.

After compiling all selected Noct and userspace glue objects, perform a
relocatable link and `nm -u` audit.  Add a libc function only when the audit or
a runtime path proves it is needed.  Do not implement broad libc stubs that
return success without behavior.

`FILE` becomes fd-backed in userspace.  Preserve the public structure as an
opaque implementation detail where possible; do not store a kernel pointer.
stdio must handle partial read/write and set EOF/error/errno correctly.

### 14.3 crt0 and environment

`user/crt0.S` reads argc/argv/envp from the stack, establishes `environ`, calls
`boots_user_libc_init()`, calls `main(argc, argv, envp)`, and invokes `_exit`
with the returned int.  It must contain no `hlt`, I/O instruction, or direct
HAL call.  `abort()` exits with a documented nonzero code after a diagnostic
to stderr.

Gate I: standalone libc user tests cover every wrapper, errno conversion,
stdio, malloc exhaustion, environment replacement, and clean process exit.

## 15. Phase J: build `NOCT.ELF`

Refactor `noct.mk` so user and embedded transition objects cannot be confused.

- User compilation defines `NOCT_TARGET_POSIX`, `NOCT_TARGET_BOOTS`, and
  `NOCT_USE_JIT`; it does **not** define `NOCT_TARGET_PC98BE`.
- Undefine host `__linux__`/`linux` target-detection macros for the freestanding
  build so Noct does not identify Boots as Linux.  `NOCT_TARGET_POSIX` supplies
  the intended API branches.
- Continue `-m32 -march=i386`, no PIC/PIE, no stack protector, no x87/MMX/SSE,
  and the existing forbidden-opcode audit.
- Link selected Noct core, JIT, API-file, Term, generic BeUI core/image/API,
  userspace Boots adapters, user libc/crt0, and softfloat into
  `build/pc98/NOCT.ELF`.
- Do not link PC-98 GDC/Cirrus/CGROM upstream backends into the user ELF; their
  replacement is the ioctl BeUI HAL.
- `platform/pc98/noct-user.ld` should place page-aligned nonoverlapping
  PT_LOADs starting around `0x00400000`, keep all mappings below mmap space,
  and leave the fixed high stack area free.  Match the current ELF loader's
  no-overlapping-page rule.
- Add `readelf -h -l`, `nm -u`, opcode, no-IO-instruction, and size checks.
  The final undefined set must be empty.

Do not modify files under the `noct/` submodule during this milestone.  The
POSIX target path already uses malloc, stdio/directory APIs, mmap, mprotect,
and munmap.  If a real upstream defect blocks the build, stop and present the
smallest proposed upstream patch separately rather than silently editing the
submodule.

Install `NOCT.ELF` in the boot FAT root using the existing 8.3-compatible
name.  Never modify a source image under `~/images`; tests operate on copies.

## 16. Phase K: userspace Noct adapters

`user/noct/main.c` supports these stable forms:

```text
NOCT.ELF --repl
NOCT.ELF path/to/program.NCT [arguments...]
NOCT.ELF --bytecode path/to/program.NAP [arguments...]
NOCT.ELF --self-test [repeat-count]
```

It opens/reads the complete source or bytecode with stdio, configures Noct
memory sizes conservatively from the successfully allocated heap, registers
the selected APIs, executes `main`, destroys the VM, closes BeUI/Term/files,
emits optional fd-3 result data, and exits.  It must not depend on
`struct boots_filesystem`, `struct cwdinfo`, `boots_environment`, or the
kernel's old arena profile.

Adapt the current `src/noct/napi.c` behavior rather than changing Noct script
APIs:

- Console output uses stdout.
- Screen and Keyboard call `/dev/console` ioctls.
- File/FileUtil uses libc stdio, stat, cwd, and dirent.
- System environment uses the process-local `environ` implementation.
- imports load through normal file APIs.
- BeUI uses the user HAL described above.
- clock uses `CLOCK_MONOTONIC`; remove the one-second BIOS polling workaround.

The BeUI display `enter()` opens `/dev/graphics` once, sends the preferred-bpp
hint, and stores returned mode info.  Every fill/line/pattern/blit/flush is one
validated ioctl (with row-chunking performed in the kernel).  Glyph measure
and draw use GET_GLYPH plus mono blit.  Key-state/drain remain console ioctls.
`leave()` always closes graphics, including error cleanup.

Gate K: launch HELLO/LS/CP, REPL, BMPVIEW, the graphical menu, Holoris, and
Remacs as userspace Noct, first while embedded Noct remains linked as a
fallback.  Compare output/screenshots and file effects with the existing test
suite.

## 17. Phase L: switch kernel call sites and remove embedded Noct

Add one kernel helper, for example
`run_noct_user(path, argc, argv, env, result_buffer, capacity)`, outside the
generic process core.  It constructs `NOCT.ELF` argv/envp, optionally requests
fd 3, spawns, waits, reaps, restores the text console if graphics was used,
and returns child status/result.  Keep Noct policy out of `process.c`.

Update call sites in this order, running their old regression immediately:

1. `noct-test` and explicit `noct file` in `src/kern/shell.c`;
2. unknown `.NCT`/`.NAP` application dispatch;
3. `emacs`/REMACS;
4. `src/kern/startup.c::run_autoexec()` including fd-3 `BOOT_ACTION`;
5. interactive `noct` REPL.

Do not change the user-visible command grammar in the same phase.  The kernel
shell and startup menu remain process0 code for now; only the Noct execution
engine moves.

Once every path passes on GDC and Cirrus:

- remove the files listed in section 5.5 and their BOOT.SYS objects;
- remove `boots_noct_prepare_memory()` and its `pmem_reserve()` call from
  `kernel_entry()`;
- remove embedded `NOCT_OBJECTS`, generic user BeUI objects, and Noct-only
  softfloat objects from `STAGE2_OBJS`;
- retain only PC-98 backend objects genuinely used by `/dev/graphics`, or move
  their logic into the new kernel driver;
- keep host Noct tests but make them link the user adapters/libc where useful;
- run `size`, `readelf -l`, and linker-map comparisons before/after.  Report
  low-segment remaining margin, high file size, high bss, and kernel heap
  separately.

The high `.kernel_heap` is not moved merely to make a size report look small.
It may remain disposable after the Linux commit, provided the post-commit
closure never reads it.

## 18. Phase M: Linux point of no return

This phase begins only after all synchronous Noct children have exited and
been reaped.

1. Split Linux loading into `probe/validate/prepare` and `commit/load/jump`.
   Before commit, validate ELF headers, every source range, destination range,
   entry, boot parameters, command line, disk/file readability, and all
   arithmetic.  Any failure here returns normally to the shell.
2. Add a process-table query that proves no process other than process0 has a
   live/runnable/sleeping/zombie thread.  Do not implement a half-working
   forced kill.  Return `EBUSY` before commit if a user process remains.
3. Restore text, close all process0 files not needed by the already prepared
   loader, and ensure execution is on thread0/system space and the original
   low startup stack.
4. Disable scheduler timer/reschedule requests and then disable IRQs.  After
   this point do not call scheduler, task, process, VFS name lookup, allocator,
   printf/stdio, graphics, or any code/data in the disposable high segment.
5. Set a low-memory `linux_handoff_committed` marker.  Load Linux only through
   a statically audited low closure using prevalidated state and bounded
   low/static buffers.  The current initial HAL task object lives in high
   kernel heap, so the committed path must not call `curthread` or any
   `hal_task_*` function after overwrite begins.
6. Once the first destination byte at/above 1 MiB can be written, any error
   calls a low-only fatal halt/reboot path.  It never returns to shell.
7. Jump through `linux-entry.S`.  The jump path is noreturn and contains no
   hidden cleanup.

Add a linker/map audit that lists every symbol reachable from the commit
entry.  Reject references to `.text.high`, `.rodata.high`, `.data.high`,
`.bss.high`, `.kernel_heap`, Noct/user objects, scheduler/process/task code,
or generic heap/stdio.  Keep the existing low/high assertions and measure
their margins rather than weakening them.

Gate M: the known `~/linux-pc98` release kernel reaches its BusyBox shell from
the copied working HDD image after the full GUI/Emacs/Noct suite has passed.

## 19. Verification plan and required gates

### 19.1 Host/unit tests

Add or extend tests for:

- syscall argument extraction and negative errno conversion;
- uaccess ranges, crossing regions, holes, protection, overflow, strings;
- ordered vm regions, first-fit, full unmap/protect, W^X rejection, rollback;
- filedesc install-at/take/close and file reference behavior;
- process wait lost-wakeup cases, fault exit, repeated reap, result fd bounds;
- nodev mount/unmount and unchanged FAT mount behavior;
- cdev duplicate/lookup/open/close/ioctl and devfs readdir/stat;
- console ioctl validation and UTF-8 chunk boundaries with a fake HAL;
- graphics ioctl clipping, overflow, row chunking, glyph buffers, exclusive
  ownership, and close-on-fault with fake backends;
- userspace libc wrappers through a mock syscall transport;
- Noct user relocatable-link unresolved symbols and JIT mmap lifecycle.

Every new allocation failure path needs a host test or an explicit injected
failure test.  Run the existing full `make ARCH=pc98 check` after each phase.

### 19.2 New QEMU scripts

Add bounded scripts, following the cleanup/QMP/screenshot style of existing
tests:

- `scripts/test-user-syscall.sh`;
- `scripts/test-user-uaccess.sh`;
- `scripts/test-user-mmap.sh`;
- `scripts/test-devfs-console.sh`;
- `scripts/test-devfs-graphics.sh` with GDC and Cirrus modes;
- `scripts/test-user-noct-file.sh`;
- `scripts/test-user-noct-repl.sh`;
- `scripts/test-user-noct-menu.sh`;
- `scripts/test-user-noct-remacs.sh`;
- `scripts/test-user-noct-crash.sh`;
- `scripts/test-linux-handoff.sh`.

Use the current QEMU binary under `~/qemu-pc98/build/qemu-system-i386`, the
compatible BIOS under `~/qemu-pc98/roms/pc98bios`, `-M pc9821 -m 64M`, and
`-snapshot` or a disposable image copy.  Compatible-BIOS POST exceeding five
seconds is a failure signal.  Every loop has a deadline and trap cleanup.

### 19.3 Final regression matrix

At minimum run and report:

```text
make ARCH=pc98 clean
make ARCH=pc98 all -j2
make ARCH=pc98 check -j2
scripts/test-hdd-boot.sh
scripts/test-ide-multidrive.sh
scripts/test-noct-file.sh
scripts/test-noct-repl.sh
scripts/test-beui-gdc.sh
scripts/test-beui-cirrus.sh
scripts/test-beui-menu.sh
scripts/test-beui-input.sh
scripts/test-beui-holoris.sh
scripts/test-autoexec-remacs.sh
scripts/test-user-syscall.sh
scripts/test-user-uaccess.sh
scripts/test-user-mmap.sh
scripts/test-devfs-console.sh
scripts/test-devfs-graphics.sh
scripts/test-user-noct-file.sh
scripts/test-user-noct-repl.sh
scripts/test-user-noct-menu.sh
scripts/test-user-noct-remacs.sh
scripts/test-user-noct-crash.sh
scripts/test-linux-handoff.sh
```

Preserve old tests until their userland replacements pass.  Only then may an
old embedded-only test be retired, and the final review must identify every
retired test and its replacement.

## 20. Error handling and rollback boundaries

- Syscall error: return `-errno`; never fatal for user input.
- Uaccess error: no partial kernel-state mutation before the failing copy,
  except POSIX-permitted partial read/write results.
- Spawn error: candidate process remains unpublished and every owned object is
  destroyed in reverse order.
- mmap error: no region metadata or page mapping remains after failure.
- mprotect error: old mapping and metadata remain effective.
- cdev open error: no file slot, inode reference, or ownership bit remains.
- graphics ioctl error: mode/ownership remains consistent; a failed ENTER
  must restore text and release `fb_set_active`.
- process crash: descriptors close, graphics leaves, parent wakes and reaps.
- Noct user failure before parity: report it and keep embedded fallback
  available; do not delete the old path.
- Linux failure before commit: return to shell.  Linux failure after commit:
  low-only halt/reboot, never return.

Implementation is uncommitted, but rollback must use reviewed phase-local
patches.  Do not use `git reset --hard`, `git checkout --`, or delete the
worktree.  Preserve user edits which appear during implementation.

## 21. Files and repositories that must not be edited

Unless the user separately changes scope, do not edit:

- anything under `/home/awe/linux-pc98/external/linux` (reference only);
- anything under `/home/awe/qemu-pc98` (QEMU executable/ROMs are test inputs);
- the `noct/` submodule (compile and inspect it, but keep its commit/diff
  unchanged as described in phase J);
- `bootsectors/pc98/stage1.S`, `platform/pc98/boot-header.S`,
  `platform/pc98/fat-loader.S`, or Stage 1/Stage 2 handoff formats;
- `drivers/pc98-ide.c`, `drivers/pc98-ide.h`, partition parsers, FAT sector
  layout, CHS logic, or image geometry scripts, except adding `NOCT.ELF` to
  an existing image-copy/install list;
- `src/hal/i386/page.c`, `space.c`, `task.c`, `dispatch.S`, or `locore.S`
  unless a focused test proves a HAL defect that cannot be fixed through the
  public HAL.  Present such a defect for review before broadening scope;
- the working images under `~/images`;
- unrelated backup files, editor temporaries, or user changes;
- git history, index, branches, submodule commit, or remotes.

`src/hal/i386/int.c` is the only intended CPU-dependent syscall change.
`src/hal/i386/trap.S` is expected to remain unchanged.

## 22. Review checkpoints for a lower-capability implementation agent

Stop and request review rather than inventing behavior if any of these occurs:

- the current source tree no longer matches the baseline function/file names;
- a required Noct unresolved symbol falls outside the libc surface in section
  14 and would require threads, sockets, dynamic loading, or a broad POSIX
  subsystem;
- Noct cannot use its existing POSIX mmap path without editing the submodule;
- partial mmap operations become necessary for actual Noct behavior;
- a graphics operation cannot be expressed with the fixed ioctl UAPI without
  exposing VRAM or unbounded user buffers;
- process0 is not the current process when synchronous Noct launch returns;
- any active user process remains at Linux commit;
- the post-commit Linux closure references high memory or heap/task state;
- a test requires modifying a source image or external reference repository;
- existing user changes overlap a planned edit and cannot be preserved.

Do not treat a build failure as authorization to change the syscall ABI,
ioctl structures, filesystem paths, or fixed non-goals.

## 23. Final review evidence

The final uncommitted review must include:

1. `git status --short`, categorized diffstat, and confirmation of no staged
   files/no commit/no submodule change;
2. the exact syscall-number table and a disassembly showing `INT 0xc2` plus
   return in EAX;
3. NOCT.ELF `readelf -h -l`, size, unresolved-symbol result, opcode/I/O audit;
4. `/`, `/dev`, `/dev/console`, `/dev/graphics`, and `/diskN` listing/stat
   evidence;
5. normal/fault exit and 100-run heap/pmem leak evidence;
6. mmap/JIT RW-to-RX-to-RW/unmap evidence and invalid-pointer `EFAULT` tests;
7. GDC and Cirrus screenshots, preferred-24-bpp result, glyph test, and
   graphics crash restoration;
8. Noct file/REPL/menu/Holoris/Remacs results and preserved AUTOEXEC action;
9. BOOT.SYS before/after file size, low segment margin, high segment layout,
   and `.kernel_heap` location;
10. Linux precommit rejection tests, postcommit low-closure audit, and BusyBox
    boot evidence;
11. exact QEMU/BIOS/image-copy commands and all timeout values;
12. known limitations: one user thread, no fork/signals/dynamic ELF, anonymous
    whole-region mmap only, no VRAM mapping, and synchronous Noct launch.

The reviewer decides whether and how to commit.
