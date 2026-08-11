# Process, thread, scheduler, ELF init, and ring-3 trap plan

## 0. Document status

- Status: **implemented; unstaged working tree ready for review**
- Written: 2026-08-11
- Authoritative repository: `/home/awe/boots`
- Baseline branch: `kernel`
- Baseline commit observed while writing this plan:
  `1e3753f Add block device, bio, inode, and file system layers`
- Implementation policy: do not create commits, do not stage changes, and leave
  the complete working-tree diff for final review.

Implementation completed on 2026-08-11 without creating a commit or staging
files.  The normal ring-3 INT probe, contained user-fault case, missing-init
case, malformed-init case, live GUI checks, full build, and host test suite all
pass with the QEMU and compatible BIOS paths documented in the final review.

This document defines the next Boots milestone.  It is intentionally detailed
enough for an implementation agent to follow without inventing interfaces or
silently broadening scope.  If the source tree changes after the baseline
above, re-run the baseline checks and update only the file paths that moved;
do not reinterpret the fixed decisions below.

## 1. End state and acceptance criteria

The milestone is complete only when all of the following are true at the same
time.

1. The existing boot context is represented as `process0` and `thread0`.
   It continues to execute the existing GUI menu, shell, embedded Noct, BeUI,
   startup scripts, and Linux-loading commands.
2. A priority round-robin kernel scheduler uses HAL tasks as opaque CPU
   contexts.  Timer IRQs can preempt the foreground kernel thread and run a
   background thread.
3. HAL spaces provide a protected user address space below `0x80000000`.
   Kernel mappings at and above `0x80000000` are supervisor-only in every user
   page table.
4. The VFS opens `INIT.ELF` relative to the boot partition's initial current
   directory.  A missing or invalid image reports an error and leaves the GUI
   and shell usable.
5. `INIT.ELF` is validated as a static ELF32/i386 `ET_EXEC`, its `PT_LOAD`
   segments and user stack are installed in a new `struct vmspace`, and PID 1
   with one ring-3 thread is made runnable.
6. The test ELF executes `int $0xc2` from CPL 3.  The kernel records a trap
   marker containing at least vector, CS, EIP, EAX, PID, TID, and count, then
   returns to CPL 3.  The ELF remains alive in a user-mode loop.
7. A QEMU test proves both sides concurrently: the trap marker appears, and
   the existing graphical menu remains visible and responds to a key after
   the marker appears.
8. An unexpected user fault terminates or quarantines only that user thread
   and returns execution to the foreground kernel thread.  A user error must
   not halt the entire machine.
9. Existing host and QEMU regressions continue to pass, including Noct/BeUI
   tests.  Noct remains embedded in this milestone.
10. `git diff --check` passes and the final review receives the complete
    unstaged diff, test commands, results, image paths, and any known
    limitation.  No commit is created.

The first test ELF is not a userland shell and does not call a real system
call.  Reaching and returning from the ring-3 `INT 0xc2` gate is the final code
point for this milestone.

## 2. Decisions fixed by review

The following are requirements, not open questions.

- Kernel process object: `struct process`.
- Kernel scheduling/thread object: `struct thread`.
- Kernel VM object: `struct vmspace`.
- CPU-dependent context: an opaque HAL `hal_task_t`; the kernel must not read
  the i386 resume frame directly.
- A HAL task contains one kernel-private `void *` which points to its owning
  `struct thread`.
- Per-thread scheduler fields live in `struct sched`, embedded in
  `struct thread`.
- Runnable lists use `struct sched_queue`.
- Current thread is `curthread`, derived from the current HAL task.  Current
  process is always `curthread->proc`; do not introduce an independently
  mutable `curproc` global.
- Descriptor table: `struct filedesc`.
- Root/current-directory state: `struct cwdinfo`.
- Process-wide signal state belongs to `struct process`; thread-specific
  signal state belongs to `struct thread`.  Signal delivery itself is out of
  scope.
- Process/thread names reserved by the design are:
  `process_init()`, `process_find()`, `process_find_by_tid()`, `fork1()`,
  `thread_create()`, `kthread_create()`, `thread_fork()`, `thread_start()`,
  `thread_exit()`, `exit1()`, `process_free_mem()`, and `thread_wait()`.
- VM names are `vmspace_create()`, `vmspace_fork()`, and `vmspace_free()`.
- Scheduler names are `sched_add()`, `sched_unlink()`, `sched_wakeup()`,
  `sched_switch()`, `sched_yield()`, `sched_clock()`, `sched_sleep()`, and
  `sched_awake_from_sleep()`.
- A terminating thread leaves the CPU by setting its terminal state,
  `sched_unlink(curthread)`, and `sched_yield()`.  It does not rely on a HAL
  entry-function return and it never destroys its own HAL task or stack.
- The system-call interrupt vector is `0xc2`.
- This milestone implements only a temporary `INT 0xc2` observation path.
  System-call numbers, argument registers, copyin/copyout, errno return ABI,
  and actual services are a later milestone.
- The default init path is the relative path `INIT.ELF`; it therefore resolves
  from the boot partition's `/diskN` initial cwd.  Do not add `/sbin` to the
  synthetic root.
- The initial test image is ELF32, little-endian, `EM_386`, static `ET_EXEC`.
- The test ELF executes `int $0xc2` once and then loops in user mode.  PID 1
  remains alive while the GUI is tested.
- A persistent kernel heap and the temporary embedded-Noct arena must be
  separated.  Keep the Noct side isolated and removable because Noct will
  shortly move into a user ELF; do not redesign upstream Noct around this
  temporary arrangement.

## 3. Explicit non-goals

Do not add any of the following during this milestone.

- A real syscall dispatcher or any `read`, `write`, `open`, `exit`, `fork`,
  `execve`, signal, time, or device syscall ABI.
- libc `_start`, a general user libc, dynamic linking, `PT_INTERP`, shared
  libraries, ELF relocations, PIE/`ET_DYN`, demand paging, copy-on-write,
  `mmap`, `brk`, stack growth, ASLR, or an ELF core dumper.
- Userland Noct.  Embedded Noct and its arena remain operational until the
  separate Noct-to-ELF milestone.
- SMP.  Data structures should not prevent future SMP, but synchronization in
  this milestone is the existing single-CPU interrupt lock.
- Full POSIX signals, sessions, process groups, credentials, permissions, or
  resource limits.  Only storage locations/placeholders required by the
  agreed object ownership may be added.
- A shell command that launches arbitrary ELF files.  Only automatic
  `INIT.ELF` launch is required.
- Changing disk enumeration, partition parsing, FAT semantics, `/diskN`
  numbering, boot-origin detection, or cwd selection.
- Changing the Linux boot protocol or converting the legacy Linux loader to
  the new process VM.
- Changing PC-98 BIOS, CHS rules, boot sector formats, Stage 1 handoff ABI,
  IDE behavior, or QEMU emulation.
- Changing BeUI rendering, color selection, Cirrus/GDC initialization, input
  behavior, or Noct language/runtime semantics.
- Modifying `/home/awe/qemu-pc98` or
  `/home/awe/linux-pc98/external/linux`.
- Creating a commit, staging the diff, rebasing, resetting, or using a
  destructive git command.

`fork1()`, `thread_fork()`, and `vmspace_fork()` are fixed names for the later
fork path, but a correct user `fork()` requires the active syscall/trap frame.
Because the syscall framework is explicitly deferred, do **not** invent a
half-working CPU-context clone in this milestone.  The structures must be
designed so those functions can be added, but no `ENOSYS` stub or misleading
public implementation is required.  `vmspace_create()`, `vmspace_free()`,
normal `thread_create()`, and init process creation are in scope.

## 4. Current baseline hazards that must be resolved

These are observed facts in the baseline, not optional cleanup.

### 4.1 HAL API split

`include/hal/hal.h` declares `hal_task_*`, `hal_mem_create_space()`, and
`hal_page_*`, while the i386 implementation still exports obsolete task and
address-space APIs from compatibility headers.  Several modern
functions are declarations without implementations.  The implementation must
converge on the `hal_*` API before process code is added.

### 4.2 Task initialization is not wired on PC-98

`i386_task_init()` exists but PC-98 does not call it.  `cmain()` enables IRQs
before entering the kernel because task initialization currently needs the
kernel allocator.  The new boot sequence must install the persistent kernel
allocator, initialize the current HAL task, wrap it as `thread0`, initialize
the scheduler, and only then enable timer preemption.

### 4.3 Page-table implementation is incomplete

The legacy space creator embeds an unaligned page directory in a heap object,
user page mapping is missing, and its handle check does not validate real user
spaces.  `hal_page_map()`, protection, unmap, TLB, and space destruction need
real i386 implementations.

### 4.4 Kernel mappings are user-accessible

The baseline copies kernel direct-map PDEs with `PTE_USER`.  Ring 3 can address
above `0x80000000` regardless of the kernel's software convention, so these
PDEs must become supervisor-only in both initial and newly-created spaces.

### 4.5 Heap ownership is unsafe for persistent objects

The HAL allocator points to `boots_malloc()`, but embedded Noct calls
`boots_heap_init()` and `boots_heap_reset()` on its temporary arena.  A
process, thread, task, or page-table bookkeeping object allocated through that
global state would be lost or freed against the wrong arena.  Persistent
kernel allocation and active Noct allocation must be separate instances.

### 4.6 Physical allocator does not know all owners

The page bitmap reserves fixed low memory and PC-98 device holes, but it must
also reserve the complete loaded BOOT.SYS low/high ranges, the persistent
kernel heap, and the selected embedded-Noct arena before allocating ELF pages.

### 4.7 `INT 0xc2` currently halts

The IDT gate is correctly installed with DPL 3, but `int_handler()` has no
`INT_SYSCALL` branch and treats it as fatal.  The milestone adds only a probe
branch which records and returns.

### 4.8 User faults currently halt the machine

`handle_fault()` prints registers and loops in HLT for both kernel and user
faults.  Kernel faults remain fatal; user faults must exit/unlink the user
thread and schedule the GUI thread.

## 5. Dependency direction and ownership

The dependency direction after this work is:

```text
VFS/file
   |
   v
exec/ELF ----> process ----> vmspace ----> HAL space/page/pmem
                  |
                  v
                thread ----> scheduler ----> HAL task/context switch
                                             |
                                             v
                                       i386 CPU context
```

Rules:

- HAL may store and return an opaque kernel-private pointer, but may not
  dereference `struct thread`, `struct process`, or `struct vmspace`.
- Kernel scheduler code may call HAL task operations but may not include
  `src/hal/i386/task.h` or inspect `struct task_info`.
- ELF code may call VFS/file and vmspace APIs.  It may not call i386 page-table
  helpers or physical allocator internals directly.
- `struct process` owns exactly one `struct vmspace`, one `struct filedesc`,
  one `struct cwdinfo`, shared signal state, and a list of threads.
- Each `struct thread` owns one HAL task and one embedded `struct sched`.
- A HAL task's private pointer points back to that thread.  It is set before
  the thread becomes runnable and cleared before deferred task destruction.
- `struct vmspace` owns every physical allocation mapped into the process.
  `vmspace_free()` is the only normal release path for those regions.
- A running or current thread must never free its own `struct thread`, HAL
  task, kernel stack, process, or vmspace.

## 6. Persistent kernel heap and temporary Noct heap

### 6.1 Heap instance API

Refactor `libc/heap.c` state into an explicit object without changing the
allocator algorithm.

```c
struct boots_heap {
    void *original_base;
    size_t original_size;
    uint8_t *begin;
    uint8_t *end;
    struct heap_block *first;
    struct heap_block *free_list;
    size_t current_bytes;
    size_t peak_bytes;
    size_t errors;
    size_t fail_after;
    size_t successful_allocations;
    boots_heap_observer_fn observer;
    void *observer_context;
};
```

Provide explicit operations, with final exact spelling chosen once and used
consistently:

```c
void boots_heap_init_instance(struct boots_heap *, void *, size_t);
void *boots_heap_alloc(struct boots_heap *, size_t);
void *boots_heap_calloc(struct boots_heap *, size_t, size_t);
void *boots_heap_realloc(struct boots_heap *, void *, size_t);
void boots_heap_free(struct boots_heap *, void *);
void boots_heap_reset_instance(struct boots_heap *);
struct boots_heap *boots_heap_set_active(struct boots_heap *);
```

Compatibility wrappers such as `boots_heap_init()`, `boots_malloc()`,
`boots_free()`, observer/stat APIs, and freestanding `malloc()` must continue
to operate on the active heap so existing host and Noct tests continue to
build.  Do not duplicate allocator algorithms between the instance and
wrapper paths.

### 6.2 Persistent heap placement

Add a 512 KiB, page-aligned, NOLOAD kernel heap buffer to the resident high
segment.  Use a named `.kernel_heap` section and explicit linker symbols so it
is visible in the BOOT.SYS high-end accounting.

```c
#define KERNEL_HEAP_SIZE (512U * 1024U)
```

The linker must keep the existing assertions.  If the new `__high_end`
violates the PC-98 15 MiB-hole assertion, stop and report; do not weaken the
assertion or move data blindly across the hole.

`kernel_entry()` creates a `kernel_heap` instance over that buffer before any
HAL task allocation.  `hal_set_allocator()` is wired to functions that always
allocate/free from `kernel_heap`, never from the active Noct heap.

Kernel process/thread/vmspace/filedesc/cwdinfo bookkeeping also uses explicit
kernel-heap wrappers.  Do not call the active generic `malloc()` for objects
that survive a Noct invocation.

### 6.3 Temporary Noct activation

`src/noct/noct.c` owns a temporary heap instance for each Noct VM/repl run.
It activates that instance before upstream Noct code uses `malloc()` and
restores the previous active heap on every success and failure path.

Requirements:

- Restoring the previous heap is mandatory even if VM creation, registration,
  compilation, execution, or cleanup fails.
- `boots_heap_reset()` during Noct execution resets only the Noct instance.
- HAL and kernel persistent allocations always bypass the active pointer and
  remain valid while Noct is active.
- Do not modify files under the `noct/` submodule for this separation.
- Mark the activation adapter as temporary and local to `src/noct/`; it is
  deleted when Noct becomes a user process.
- During this milestone, the background test ELF performs no syscalls, so it
  cannot allocate in the kernel while Noct owns the active libc heap.  The
  future syscall milestone must revisit general preemption-safe allocation.

### 6.4 Physical reservations

Before the first `hal_pmem_alloc()` used by a user vmspace:

1. Reserve physical ranges corresponding to `__low_start..__low_end` and
   `__high_start..__high_end`, including `.kernel_heap`.
2. Run the existing Noct memory-profile selection once and reserve the entire
   selected Noct arena physical range.  Keep this bridge isolated in
   `src/noct/memory.c` or one small kernel-facing adapter so it can be removed
   with embedded Noct.
3. Preserve all BSP reservations for VRAM, ROM, and the 15--16 MiB hole.
4. Reject overlapping/double reservations only if they indicate a real owner
   conflict; idempotent reservation of an already-reserved kernel page is
   acceptable.

Do not change Noct's selected capacities or memory-class policy in this work.
On a machine too small to leave enough pages after the reservations, report
`INIT.ELF: insufficient memory` and continue the existing GUI rather than
stealing from the Noct arena.

## 7. HAL API convergence

### 7.1 Public task API

Use the declarations in `include/hal/hal.h` as the canonical interface:

```c
typedef void *hal_task_t;

void hal_task_init(void);
hal_task_t hal_task_create(hal_space_t space,
    void (*start)(void *), void *arg, void *user_stack_pointer);
void hal_task_destroy(hal_task_t);
void hal_task_context_switch(hal_task_t);
hal_task_t hal_task_get_current(void);
void hal_task_set_private(hal_task_t, void *);
void *hal_task_get_private(hal_task_t);
void hal_task_set_tls(hal_task_t, uintptr_t);
uintptr_t hal_task_get_tls(hal_task_t);
```

Rules:

- `hal_task_init()` registers the already-running boot context.  It does not
  allocate a replacement stack or switch context.
- The initial task is not destroyable.
- A created kernel task has `space == HAL_SPACE_SYS`, `user_stack_pointer ==
  NULL`, and starts through a kernel trampoline with `arg`.
- A created user task has a non-NULL space.  `start` is the ring-3 EIP,
  `user_stack_pointer` is the exact initial ESP, and `arg` must be NULL.
- HAL must not write arguments or a return address into user memory.
- User CS/SS/DS/ES have RPL 3; EFLAGS has IF set and IOPL 0.
- `hal_task_destroy()` asserts that the target is neither current nor the
  initial task.
- The private pointer is opaque to HAL and is not confused with user TLS.

Migrate all PC-98 consumers from `task_t/task_*` to `hal_task_t/hal_task_*`.
Compatibility typedefs may exist during a phase, but remove them once all
objects build.  Do not keep two independently implemented APIs.

### 7.2 Public space and pmem API

Implement the existing `hal_space_t`, `hal_page_*`, and `hal_pmem_*`
declarations.  Fold or remove the old space handle, `PAGE_*`, and `pmem_desc`
public surface after all callers are migrated.

All public page operations validate:

- non-NULL, known user-space handle;
- page-aligned virtual/physical addresses and size;
- nonzero size with no addition overflow;
- the complete virtual range below `0x80000000`;
- no mapping of page zero unless a later explicit policy changes it;
- no remapping of an already-present PTE;
- valid permission/cache flags.

`HAL_SPACE_EXEC` is retained in software metadata.  i386 without NX cannot
enforce non-executable pages, which must be documented rather than simulated
incorrectly.

### 7.3 i386 page-directory ownership

Change the i386 space object so page directories and
page tables are physical page allocations with guaranteed 4 KiB alignment.
Store both the physical address used by CR3/PDE and the kernel direct-map alias
used to edit entries.

For every new user space:

- user PDEs 0--511 start empty;
- copy the required kernel-half PDEs from the known system page directory;
- clear `PTE_USER` on every kernel PDE/PTE;
- preserve supervisor read/write access required by the kernel;
- retain the PC-98 device/direct mappings the current kernel needs;
- keep a list of allocated page-table pages for destruction.

Replace the legacy handle check with validation in the canonical space
implementation.  `HAL_SPACE_SYS` is valid where documented,
but user map/unmap calls must not modify the system page table.

### 7.4 Initial page table protection

Remove `PTE_USER` from kernel direct-map entries created in `locore.S`.  Verify
that ring-0 boot, console, IDE, graphics, Noct, and Linux loading still work.
Do not solve a regression by re-enabling ring-3 access to the kernel half.

### 7.5 Context switching

When switching threads:

- select the destination space from `destination_thread->proc->p_vmspace`;
- kernel threads use `HAL_SPACE_SYS`;
- update TSS `esp0` before a user task is entered;
- switch CR3 when required, including returning from a user space to the
  canonical system space;
- save/restore the existing floating-point state as before;
- never call the scheduler from inside the assembly dispatcher;
- preserve the property that a task suspended in an IRQ returns through the
  same IRQ frame and `iret` when later selected.

## 8. Boot and interrupt initialization order

Change the sequence deliberately; do not enable interrupts in `cmain()` before
the scheduler is ready.

Required sequence:

```text
i386 locore (IRQs disabled)
  -> physical page bitmap and BSP reservations
  -> GDT/IDT/PIC/PIT construction, still masked/disabled
  -> kernel_entry
       -> initialize persistent kernel heap
       -> install HAL persistent allocator
       -> reserve BOOT.SYS and temporary Noct physical ranges
       -> hal_task_init() for boot context
       -> process_init() creates process0/thread0
       -> associate current HAL task private pointer with thread0
       -> sched_init() marks thread0 running and initializes queues
       -> create and start a dedicated idle kernel thread
       -> enable IRQ delivery through a public HAL IRQ operation
       -> existing platform/VFS/graphics initialization
       -> attempt INIT.ELF spawn
       -> existing GUI/startup/shell loop in thread0
```

Do not expose `asm_sti()` to ordinary kernel code.  Add or use a small public
HAL operation for the final interrupt enable.  Its implementation remains in
HAL/i386.

The idle thread belongs to `process0`, has the lowest priority, and loops with
HLT only when no non-idle thread is runnable.  The existing boot/GUI thread is
not converted to idle in this milestone.

## 9. Kernel scalar types and errors

Add missing process types to `libc/include/sys/types.h`:

```c
typedef int32_t pid_t;
typedef int32_t tid_t;
```

Add only errors needed by this work, with unique existing-style positive
values, including at least:

```text
ENOEXEC  invalid/unsupported ELF
ESRCH    PID/TID not found
ECHILD   no waitable child/thread
EFAULT   invalid user address/faulted image operation
EAGAIN   process/thread/resource limit reached
```

Kernel internal functions return positive errno values as the current VFS
does.  Do not introduce Linux-style negative errno returns.

## 10. Process, thread, and scheduler structures

### 10.1 Process state

```c
enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_ZOMBIE,
    PROCESS_DEAD
};
```

`PROCESS_ZOMBIE` retains PID, parent linkage, and exit/fault status until a
safe reaper path calls `process_free_mem()`.  Heavy resources may be released
before final object removal only through a documented one-time path.

### 10.2 Thread state

```c
enum thread_state {
    THREAD_NEW = 0,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_ZOMBIE,
    THREAD_DEAD
};
```

Lifecycle state belongs directly to `struct thread`, not inside
`struct sched`, because zombie/dead states are not scheduler-policy data.

### 10.3 Scheduler data

```c
struct sched {
    int priority;
    uint32_t quantum;
    uint64_t wakeup_tick;
    unsigned queue_kind;
    struct thread *next;
    struct thread *prev;
};

struct sched_queue {
    struct thread *head;
    struct thread *tail;
    unsigned count;
};
```

Use 16 priority levels retained from the baseline, with 0 highest and 15
lowest.  Use a fixed initial quantum documented in ticks.  The idle thread is
special and is never inserted ahead of ordinary priority-15 work.

### 10.4 Thread object

The initial object must contain at least:

```c
struct thread {
    tid_t tid;
    struct process *proc;
    hal_task_t task;
    enum thread_state state;
    unsigned flags;
    int exit_status;
    struct sched sched;
    struct thread *proc_next;
    void (*kernel_entry)(void *);
    void *kernel_arg;
    uint32_t signal_mask;
    uint32_t signal_pending;
};
```

Names may use the repository's established prefix style, but do not rename the
public structures or adopted functions.  Thread-specific signal fields are
storage only in this milestone.

### 10.5 Process object

The initial object must contain at least:

```c
struct process {
    pid_t pid;
    enum process_state state;
    unsigned flags;
    unsigned thread_count;
    int exit_status;
    struct process *parent;
    struct process *children;
    struct process *sibling;
    struct process *all_next;
    struct thread *threads;
    struct vmspace *vmspace;
    struct filedesc *fd;
    struct cwdinfo *cwdi;
    uint32_t signal_pending;
    uint32_t signal_ignored;
};
```

`process0` has PID 0, uses the system vmspace, owns `thread0` and the idle
thread, and receives the existing VFS cwd after mount initialization.  PID 1
is a child of `process0`.

### 10.6 Current object

Define `curthread` from HAL state, not from a separately maintained pointer:

```c
#define curthread \
    ((struct thread *)hal_task_get_private(hal_task_get_current()))
```

If a function form is needed for type checking, provide `thread_current()` and
define `curthread` in terms of it.  Assert that the private pointer is non-NULL
after `process_init()`.

## 11. Process and thread API semantics

### 11.1 Initialization and lookup

```c
void process_init(void);
struct process *process_find(pid_t);
struct thread *process_find_by_tid(tid_t);
```

- Initialize PID/TID allocation, the all-process list, `process0`, and
  `thread0` exactly once.
- PID 0 and TID 0 are reserved for the boot context.  PID 1 is explicitly
  assigned to init.  Subsequent IDs increase monotonically and are not reused
  in this milestone.
- Lookup returns a borrowed pointer valid until the next scheduling point on
  the single CPU.  Callers that modify lists disable interrupts.  Do not cache
  a lookup result across `sched_yield()`.
- Lookup ignores `PROCESS_DEAD`/`THREAD_DEAD` objects.

### 11.2 Empty process creation

Add a small internal process creation operation (for example
`process_create()`) because init is not the result of `fork1()`.

It must:

1. allocate and zero a process from the persistent kernel heap;
2. set parent/child relationships;
3. create an empty `filedesc`;
4. clone/reference the parent's `cwdinfo` as specified below;
5. leave `vmspace` and thread list empty until the caller installs them;
6. keep state `PROCESS_NEW` until the first thread is successfully built;
7. publish the process in global/PID lists only after all mandatory creation
   steps succeed, or remove it completely during unwind.

### 11.3 Thread creation

Use two public creation paths:

```c
int thread_create(struct process *, uintptr_t entry, uintptr_t user_sp,
    struct thread **result);
int kthread_create(void (*entry)(void *), void *arg, int priority,
    struct thread **result);
```

- `thread_create()` requires a non-system vmspace, validates entry/stack below
  `0x80000000`, creates a user HAL task, sets its private pointer, and leaves
  the thread `THREAD_NEW` and not queued.
- `kthread_create()` attaches the thread to `process0`, creates a system HAL
  task through a kernel trampoline, and also leaves it `THREAD_NEW`.
- The kernel trampoline calls the supplied function and then calls
  `thread_exit()` if the function returns.
- No creation function makes a partially initialized task runnable.

### 11.4 Start, exit, wait, and deferred destruction

```c
void thread_start(struct thread *);
void thread_exit(int status) __attribute__((noreturn));
void exit1(int status) __attribute__((noreturn));
int thread_wait(struct thread *, int *status);
void process_free_mem(struct process *);
```

- `thread_start()` is the only `THREAD_NEW -> THREAD_RUNNABLE` path and calls
  `sched_add()` with interrupts disabled.
- `thread_exit()` marks the current thread zombie, stores status, removes it
  from scheduler queues with `sched_unlink()`, wakes any waiter, and calls
  `sched_yield()`.  Reaching code after that call is fatal.
- `exit1()` marks the whole process zombie, records status, prevents sibling
  threads from returning to user mode, notifies the parent, and terminates the
  current thread through `thread_exit()`.
- `thread_wait()` may sleep until the target is zombie, but it must reject
  waiting on self.  Once the target is not current/runnable, the waiter clears
  the HAL private pointer, calls `hal_task_destroy()`, removes the thread from
  the process, and frees the thread object.
- `process_free_mem()` is called only after all process threads have been
  reaped.  It frees vmspace, filedesc, cwdinfo references, list links, and the
  process object.  It must reject `process0`, the current process, or any
  process with a live thread.

For the background init loop, normal exit/wait is primarily exercised by
kernel-thread and user-fault tests.  Do not add a user `_exit` syscall.

## 12. Scheduler behavior

All scheduler queue operations run with local interrupts disabled.  Add a
debug assertion for that condition if HAL can expose it without expanding
scope.

### 12.1 Queue invariants

- A thread is in at most one scheduler queue.
- `THREAD_RUNNING` is not on a run queue.
- `THREAD_RUNNABLE` is on exactly one priority queue unless it is the selected
  next thread during a switch.
- `THREAD_SLEEPING` is not on a run queue; timed sleep may be on the timewait
  queue.
- `THREAD_ZOMBIE` and `THREAD_DEAD` are on no scheduler queue and can never be
  selected.
- Queue count/head/tail and embedded links always agree.

### 12.2 Exact operations

```c
void sched_init(void);
void sched_add(struct thread *);
void sched_unlink(struct thread *);
void sched_wakeup(struct thread *);
void sched_switch(void);
void sched_yield(void);
void sched_clock(void);
void sched_sleep(uint64_t timeout_tick);
void sched_awake_from_sleep(struct thread *);
```

- `sched_add()` inserts a NEW or explicitly awakened thread at the tail of its
  priority queue and sets RUNNABLE.  It rejects duplicate insertion.
- `sched_unlink()` removes a queued thread if present and leaves a running
  thread unqueued; terminal/sleeping state is set by the caller.
- `sched_wakeup()` handles the public sleeping-to-runnable transition.
- `sched_awake_from_sleep()` is the internal helper that removes a timed
  sleeper from the timewait structure and then calls the same transition as
  `sched_wakeup()`.  It must not implement a second, divergent wake policy.
- `sched_switch()` selects the highest-priority head, round-robin within the
  priority, or idle if none.  It sets the destination RUNNING before invoking
  `hal_task_context_switch()`.
- `sched_yield()` requeues a normal RUNNING current thread at its priority
  tail.  It does not requeue a SLEEPING or ZOMBIE current thread.  It then
  calls `sched_switch()`.
- `sched_clock()` increments scheduler time, expires timed sleepers, decrements
  the current quantum, and requests rescheduling when the quantum expires.  It
  performs no allocation and no direct context switch in the middle of the
  timer device handler.
- `sched_sleep()` marks current sleeping, installs an optional deadline, and
  yields atomically so a wakeup cannot be lost.

Replace `sched-stub.c` only after the real scheduler links and passes its host
test.  Remove the old `sched_link()` and `sched_clock_handler()` names after
HAL IRQ consumers are migrated.  Do not leave both stub and real definitions
in different build variants accidentally.

### 12.3 Timer IRQ contract

The timer flow remains:

```text
IRQ 0 with interrupts disabled
  -> clock_handler()
  -> kernel_timer_handler()
  -> sched_clock()
  -> set reschedule flag if required
  -> send EOI
  -> at common IRQ exit, sched_yield()
  -> resume selected task through its saved frame
```

Use the existing IRQ-exit reschedule window.  Do not context-switch before EOI
or from arbitrary device code.

### 12.4 Existing IRQ service-task API

`irq_enter_isr()`/`irq_handler()` currently store a HAL task and call
`sched_link(task_t, ...)`.  Migrate this narrowly:

- store `hal_task_t` in HAL as before;
- obtain its private owner with `hal_task_get_private()` only at the scheduler
  boundary;
- call `sched_unlink(thread)` when blocking and `sched_wakeup(thread)` on IRQ;
- HAL must not inspect any thread field;
- preserve IRQ mask/EOI behavior exactly.

No new interrupt-thread model is introduced here.

## 13. `struct vmspace`

### 13.1 Structure

```c
struct vm_region {
    uintptr_t start;
    size_t size;
    uint32_t prot;
    struct hal_pmem pmem;
    struct vm_region *next;
};

struct vmspace {
    hal_space_t space;
    unsigned usecount;
    struct vm_region *regions;
    uintptr_t entry;
    uintptr_t stack_bottom;
    uintptr_t stack_top;
};
```

`process0` uses a distinguished system vmspace object whose HAL handle is
`HAL_SPACE_SYS` and which cannot be freed.

### 13.2 Operations

```c
struct vmspace *vmspace_create(void);
int vmspace_map_anon(struct vmspace *, uintptr_t, size_t, uint32_t,
    struct vm_region **);
void vmspace_free(struct vmspace *);
```

`vmspace_map_anon()`:

1. validates aligned user range and overlap against existing regions;
2. allocates page-aligned physical memory through `hal_pmem_alloc()`;
3. zeros the full allocation through its kernel alias;
4. maps it through `hal_page_map()`;
5. publishes the region only after mapping succeeds;
6. unmaps/frees fully on any failure.

`vmspace_free()` walks every region, unmaps it, frees its physical pages,
destroys the HAL space, and frees bookkeeping.  It is idempotent only for a
NULL pointer; a double free of a real object is a bug.

Do not implement `vmspace_fork()` until the future fork/syscall phase can
define the active user-register-frame contract.  Reserve the name and keep all
region ownership sufficient for an eventual eager-copy implementation.

## 14. `struct filedesc` and `struct cwdinfo`

### 14.1 Filedesc

Add a small fixed table, initially 32 entries:

```c
#define KERN_OPEN_MAX 32

struct filedesc {
    unsigned usecount;
    struct file *files[KERN_OPEN_MAX];
};
```

Provide internal create/clone/destroy/get/install helpers.  A future fork must
duplicate the table while retaining shared open-file descriptions, but fork is
out of scope.  PID 1 may start with all entries NULL because the test ELF makes
no I/O syscalls.  Do not invent console device files solely for this probe.

### 14.2 Cwdinfo migration

Rename/evolve the current `struct fs_context` into `struct cwdinfo`:

```c
struct cwdinfo {
    unsigned usecount;
    struct inode *root;
    struct inode *cwd;
    char cwd_path[BOOTS_PATH_MAX];
};
```

Keep pathname behavior identical.  Update `namei_at()`, `file_openat()`,
`fs_chdir()`, libc stdio adapter, startup, shell, and embedded Noct types
mechanically.  Do not change normalization, namecache, mount crossing, FAT
lookup, or `/diskN` policy.

After VFS initialization, attach the boot cwd to `process0->cwdi`.  PID 1 gets
a retained/copy cwdinfo whose initial cwd is the same boot `/diskN`.  Replace
the independent `kern_fs_context` storage with an accessor/compatibility alias
to `process0` during migration; do not keep two cwd objects that can diverge.

Because only thread0 performs kernel VFS/Noct operations in this milestone,
the existing libc stdio active-context mechanism may remain a temporary
thread0 adapter.  General per-process syscall file I/O is deferred.

## 15. ELF32 loader contract

### 15.1 New files

Add kernel-owned ELF definitions and loader code; do not depend on a host
system `<elf.h>`.

```text
include/kern/elf.h       fixed-width ELF32 structures/constants
include/kern/exec.h      exec_image/result and loader interface
src/kern/elf.c           validation and PT_LOAD installation
src/kern/exec.c          VFS open, vmspace, stack, and process orchestration
```

Do not reuse `boots_image_loader`, which is the legacy Linux boot-image path
over `boots_file`, not a process/VFS executable loader.

### 15.2 Accepted image

Validate all of the following before making a thread runnable:

- ELF magic;
- class `ELFCLASS32`;
- little-endian data encoding;
- current ELF version;
- `ET_EXEC`;
- `EM_386`;
- expected ELF/program-header structure sizes;
- bounded, nonzero program-header count;
- program-header table contained within inode file size with no overflow;
- at least one `PT_LOAD`;
- reject `PT_INTERP`, dynamic linking requirements, and unsupported required
  semantics with `ENOEXEC`;
- entry point in a mapped executable load segment, at or above `0x1000` and
  below `0x80000000`.

For every `PT_LOAD`:

- `p_filesz <= p_memsz`;
- `p_offset + p_filesz` is within file size without overflow;
- `p_vaddr + p_memsz` is below `0x80000000` without overflow;
- page-offset congruence between `p_offset` and `p_vaddr`;
- `p_align` is 0, 1, or a valid power-of-two compatible alignment;
- page-rounded load ranges do not overlap another accepted segment;
- translate `PF_R/PF_W/PF_X` to HAL attributes;
- reject a segment with no usable access permission;
- read exactly `p_filesz` bytes and reject short reads;
- zero all leading page padding, BSS, and trailing allocation padding.

The first implementation may require non-overlapping page-rounded segments;
the test linker script must obey that contract.  Do not silently merge
permissions for overlapping text/data pages.

Use `file_seek()` plus `file_read()` on a loader-owned file, or add one small
`file_pread()` helper.  Do not expose FAT-private LBAs or bypass VFS.

### 15.3 Transactional load

Load into a fresh, unpublished vmspace.  On any error:

- close the file;
- free every mapped segment and page table;
- free the candidate vmspace;
- remove/free the unpublished process and cwd/filedesc references;
- leave no PID/TID visible and no runnable thread;
- keep GUI execution unchanged.

Only after all ELF segments and the initial stack are complete may exec code
create the HAL user task and publish/start PID 1.

## 16. Initial user stack

Use a fixed 64 KiB RW, non-executable stack immediately below a guard page at
the user boundary:

```text
0x80000000  user/kernel split
0x7ffff000  guard page, not mapped
0x7ffef000  highest mapped stack page
    ...
0x7fff0000  initial 64 KiB stack bottom
```

If exact arithmetic gives a different bottom, define named constants and test
them; do not map the guard or cross `0x80000000`.

Build an i386 SysV-style initial stack through the physical page's kernel
alias:

```text
ESP -> argc
       argv[0]
       NULL
       envp[0] or NULL
       NULL
       strings
```

For the test:

```text
argc = 1
argv[0] = "INIT.ELF"
envp is empty
```

Align final ESP to at least 16 bytes before installing the words.  Bounds-check
every subtraction/copy.  HAL receives the completed ESP and must not modify
user stack memory.

## 17. Init process orchestration

Add a narrowly-scoped operation such as:

```c
int process_spawn_init(const char *path, struct process **result);
```

Required order:

1. Resolve/open `path` with `process0->cwdi` using `file_openat()`.
2. Allocate a candidate child process with requested PID 1 but do not publish
   it as running.
3. Create its filedesc and retained boot cwdinfo.
4. Create a fresh vmspace.
5. Validate/load ELF segments.
6. Allocate/build user stack.
7. Create the first user thread and HAL task.
8. Publish process/thread lists and set process RUNNING.
9. `thread_start()` the thread.
10. Return to the existing `kernel_main()` flow; do not wait for init.

Call this once after VFS has selected the boot cwd and before or during the
existing GUI/startup loop.  The caller must not replace the loop.  On failure,
print one concise diagnostic in text mode/log state and continue normally.

Do not launch a second PID 1 if initialization is retried or the menu loops.
Track attempted/running state explicitly.

## 18. Ring-3 `INT 0xc2` probe

### 18.1 Gate behavior

Keep IDT vector `0xc2` as an interrupt gate with DPL 3.  In the common
interrupt handler:

- distinguish hardware IRQ, CPU fault, undefined vector, and `INT_SYSCALL`;
- accept vector `0xc2` only when the saved CS has RPL 3 for this probe;
- record the trap and mark it handled;
- do not decode syscall numbers or arguments;
- do not allocate, sleep, open files, or call Noct/GUI;
- leave general registers unchanged and return through `iret`;
- a ring-0 accidental `int $0xc2` is not considered a successful user probe.

### 18.2 Observable state

Add a fixed, volatile probe record in a named kernel symbol:

```c
#define USER_INT_PROBE_MAGIC 0x42544332U

struct user_int_probe {
    volatile uint32_t magic;
    volatile uint32_t count;
    volatile uint32_t vector;
    volatile uint32_t cs;
    volatile uint32_t eip;
    volatile uint32_t eax;
    volatile int32_t pid;
    volatile int32_t tid;
};
```

The kernel-side observer obtains PID/TID from `curthread` after confirming the
HAL task has a private owner.  Write payload fields first and `magic` last with
a compiler barrier so QMP never treats a partial record as success.

Do not draw the marker onto the graphics console.  The QEMU test locates the
symbol in `build/pc98/stage2.elf` with `nm`, converts the higher-half VMA to
the physical direct-map address, and reads it with QMP/HMP `xp`.  This preserves
the GUI while giving deterministic evidence.

This probe is explicitly temporary.  Keep it in one kernel source/header and
one small HAL dispatch branch so the later syscall implementation can replace
it cleanly.

## 19. User-fault containment

Split the existing fatal fault path by saved privilege level.

- CPL 0 fault: retain register dump and fatal halt/panic behavior.
- CPL 3 page fault, illegal instruction, GP, divide error, or other processor
  fault: save vector/EIP/error/CR2 as available in thread/process diagnostic
  state; set a deterministic failure status; invoke `exit1()` or the minimal
  equivalent terminal transition; `sched_unlink()` and yield without
  returning to the bad user EIP.
- Fault handling must not free the current stack/task/vmspace.
- A process0 reaper/test cleanup path performs deferred destruction later.
- If fault containment itself detects corrupt scheduler/current-thread state,
  fail as a kernel fault rather than looping unpredictably.

Add a special negative-test ELF with an invalid memory access or `ud2` only in
the test build.  It must leave the GUI responsive and produce a fault marker;
it is not installed as the normal `INIT.ELF`.

## 20. Test ELF and build artifacts

Add:

```text
tests/user-init.S
platform/pc98/user-init.ld
```

The normal probe code is freestanding and contains no libc call:

```asm
_start:
    movl $0x42545331, %eax
    int  $0xc2
1:
    jmp  1b
```

Link at `0x00400000` as ELF32/i386 `ET_EXEC`, with explicit page-aligned
`PT_LOAD` program headers and no interpreter, relocations, dynamic section,
SSE, or newer CPU instruction.  Build with i386-compatible tools/flags.

Add `build/pc98/INIT.ELF` as an explicit artifact, not as a raw binary.  Add a
static verification target using `readelf`/`objdump` which checks class,
machine, type, entry, program headers, absence of `PT_INTERP`, and absence of
forbidden instructions.

Do not add INIT.ELF to all release images unconditionally during early phases.
The dedicated test script installs it through the existing image tooling.
Once the integrated test passes, adding it to the development HDD image may be
considered, but existing Linux release images must not be rewritten in place.

## 21. File-by-file implementation map

### 21.1 New kernel files

| File | Required content |
| --- | --- |
| `include/kern/process.h` | process state/structure, process0, init/lookup/create/exit/free declarations |
| `include/kern/thread.h` | thread state/structure, curthread, create/start/exit/wait declarations |
| `include/kern/vmspace.h` | vmspace/region structures and create/map/free declarations |
| `include/kern/filedesc.h` | 32-entry descriptor table and helpers |
| `include/kern/elf.h` | ELF32 structures and constants only |
| `include/kern/exec.h` | exec image result, ELF loader, init spawn declarations |
| `include/kern/user-probe.h` | temporary `INT 0xc2` probe structure/observer |
| `src/kern/process.c` | process0, PID/TID lists, creation, lookup, exit/free |
| `src/kern/thread.c` | thread0, kernel/user creation, start, exit, wait/deferred destroy |
| `src/kern/vmspace.c` | vmspace region ownership and HAL map/free wrapper |
| `src/kern/filedesc.c` | fixed descriptor table lifecycle |
| `src/kern/sched.c` | real priority round-robin scheduler |
| `src/kern/elf.c` | ELF validation and segment load |
| `src/kern/exec.c` | VFS open, stack construction, init transaction |
| `src/kern/user-probe.c` | temporary marker and CPL-3 interrupt observation |

### 21.2 Existing kernel files

| File | Allowed change |
| --- | --- |
| `src/kern/entry.c` | static persistent heap setup, HAL allocator/task/process/scheduler init, controlled IRQ enable |
| `src/kern/main.c` | attach process0 cwd and launch INIT.ELF without replacing GUI/shell loop |
| `include/kern/sched.h` | replace old task-cast scheduler contract with agreed structures/functions |
| `src/kern/sched-stub.c` | remove from final object list after real scheduler passes; delete only when unused |
| `include/kern/namei.h`, `src/kern/namei.c` | mechanical `fs_context` to `cwdinfo` ownership migration |
| `include/kern/file.h`, `src/kern/file.c` | cwdinfo type and optional pread helper only |
| `include/kern/vfs.h`, `src/kern/vfs.c` | initialize/attach process0 cwd; preserve mount policy |
| `src/kern/shell.c`, `src/kern/startup.c` | fetch process0/current cwd mechanically; no command behavior changes |
| `src/kern/clock.c` | preserve kernel tick; scheduler uses it or its own tick consistently |
| `src/kern/panic.c` | only if needed to distinguish kernel fatal from contained user fault |

### 21.3 HAL files

| File | Allowed change |
| --- | --- |
| `include/hal/hal.h` | canonical task/space/pmem/private-pointer and IRQ-enable API |
| obsolete HAL compatibility headers, `include/hal/hal.h` | compatibility fold/removal after migration; no second implementation |
| `src/hal/i386/task.c`, `task.h` | canonical API, initial task, private pointer, safe user frame, IOPL 0, deferred destroy rules |
| `src/hal/i386/space.c`, `space.h` | aligned page directory/table ownership and canonical space implementation |
| `src/hal/i386/page.c` | canonical pmem wrappers/reservation validation; preserve bitmap/BSP holes |
| `src/hal/i386/int.c`, `int.h` | vector `0xc2` handled probe, user-fault callback, reschedule exit |
| `src/hal/i386/trap.S` | only frame correctness needed for CPL-3 INT/fault; preserve layout contract |
| `src/hal/i386/dispatch.S` | only changes required by corrected task frame; no scheduler policy |
| `src/hal/i386/locore.S` | supervisor-only kernel mappings; preserve boot addresses/GDT/TSS layout |
| `src/hal/i386/irq.c`, `irq.h` | canonical task names and private-owner scheduler bridge; preserve PIC behavior |
| `src/hal/i386/cmain.c` | delay final STI until kernel scheduler readiness |
| `src/hal/i386/defs.h` | selectors/flags only as required; keep vector `0xc2` |

### 21.4 Heap and Noct adapter files

| File | Allowed change |
| --- | --- |
| `libc/heap.h`, `libc/heap.c` | instance allocator plus compatibility active-heap wrappers |
| `src/noct/noct.c` | temporary Noct heap activation/restoration only |
| `src/noct/memory.c`, `memory.h` | expose selected arena reservation to kernel in one removable adapter |
| `src/noct/platform.c` | cwdinfo type migration only; no Noct/BeUI behavior change |
| files under `noct/` | **no changes** |

### 21.5 Build and tests

| File | Required change |
| --- | --- |
| `platform/pc98/platform.mk` | add kernel/HAL objects, INIT.ELF rules, verification/QEMU targets |
| `platform/pc98/stage2.ld` | named page-aligned kernel heap section/symbols; keep size/hole assertions |
| `Makefile` | host-test dependencies/targets only as necessary |
| `tests/heap-context-host-test.c` | persistent and Noct heap isolation |
| `tests/sched-host-test.c` | queues, quantum, sleep/wake, unlink/zombie invariants with fake HAL |
| `tests/process-host-test.c` | process0/PID1/list/thread lifecycle/failure unwind |
| `tests/vmspace-host-test.c` | fake HAL map ownership, overlap, cleanup |
| `tests/elf-host-test.c` | valid image and exhaustive malformed/overflow/short-read cases |
| `tests/user-init.S` | normal ring-3 INT probe image |
| `tests/user-fault.S` | special negative user-fault image |
| `platform/pc98/user-init.ld` | static ELF32 load layout |
| `scripts/test-user-init.sh` | combined trap-marker and live-GUI QEMU test |

## 22. Files and behavior that must not change

Implementation agents must not edit the following unless this plan is revised
by the reviewer:

- `bootsectors/pc98/stage1.S` or any boot-sector/CHS code;
- `platform/pc98/boot-header.S`, `fat-loader.S`, or the Stage 1 handoff layout;
- `drivers/pc98-ide.c` and partition parsing;
- FAT on-disk algorithms and inode numbering;
- mount enumeration and `/diskN` naming;
- PC-98 console, keyboard mapping, GDC/Cirrus display algorithms;
- Linux loader code or Linux image scripts;
- QEMU source, BIOS images, or the read-only Linux reference tree;
- upstream `noct/` sources;
- release HDD images under `~/images` in place.

Permitted mechanical cwd type changes must not alter VFS behavior.  If a test
fails in one of these areas, diagnose the scheduler/memory regression first;
do not patch the unrelated subsystem to hide it.

## 23. Implementation phases and mandatory gates

Never proceed past a failed gate.  Diagnose and repair the current phase while
the diff is small.  Do not accumulate multiple unverified architectural
changes.

### Phase 0: Baseline capture

1. Record `git status --short`, branch, and `git rev-parse HEAD`.
2. Confirm the tree is clean except this reviewed plan.  Preserve any later
   user changes and stop if they overlap target files unexpectedly.
3. Run clean build and host tests.
4. Run existing HDD boot and one GDC/Cirrus graphical menu test using the
   current `/home/awe/qemu-pc98/build/qemu-system-i386` and compatible BIOS.
5. Save commands/results, not binary artifacts in git.

Gate: baseline BOOT.SYS, VFS, Noct, and GUI are known good before edits.

### Phase 1: Heap instances only

1. Convert heap implementation to explicit instances.
2. Add persistent kernel-heap wrappers without changing boot wiring yet.
3. Convert embedded Noct to temporary heap activation/restoration.
4. Add heap isolation host test and run all libc/Noct host tests.
5. Run existing Noct and BeUI QEMU tests.

Gate: a persistent allocation survives a complete Noct create/run/reset/destroy
cycle byte-for-byte, all previous heap statistics tests pass, and GUI/Noct
behavior is unchanged.

### Phase 2: Physical reservations and persistent boot heap

1. Add named resident kernel heap/linker symbols.
2. Reserve complete BOOT.SYS and selected Noct arena ranges in pmem.
3. Initialize persistent heap and inject it into HAL in `kernel_entry()`.
4. Add allocator diagnostics for overlaps/out-of-memory.
5. Build and inspect BOOT.SYS section/map sizes.
6. Run HDD/Noct/GUI regressions before task changes.

Gate: no pmem allocation overlaps kernel or Noct ranges, linker assertions
remain unchanged and pass, and existing single-context behavior remains good.

### Phase 3: HAL API convergence and protected spaces

1. Implement canonical pmem wrappers.
2. Implement aligned user-space directory/table create/map/prot/unmap/free.
3. Make kernel-half entries supervisor-only in initial and new spaces.
4. Add fake/host tests where practical and an i386 compile/static inspection.
5. Boot existing kernel without creating a user task; run GUI/Noct/Linux-menu
   regressions.

Gate: kernel continues to boot, all user mappings are below the split, page
tables are aligned, and static inspection proves kernel PDEs lack `PTE_USER`.

### Phase 4: HAL tasks and boot-context wrapping

1. Implement `hal_task_init()` and private pointer accessors.
2. Canonicalize create/destroy/switch APIs.
3. Correct user initial frame and IOPL but do not enter user mode yet.
4. Create `process0/thread0` around the current context with IRQs disabled.
5. Keep the real scheduler disabled until Phase 5 gate is ready.

Gate: existing kernel boots through the wrapped current task and GUI/Noct
regressions pass without any context switch.

### Phase 5: Scheduler and kernel-thread smoke test

1. Implement queues and scheduler host tests.
2. Link the real scheduler and remove stub from the target object list.
3. Create idle kernel thread.
4. Add a test-only second kernel thread which increments a volatile marker,
   yields/exits, and is reaped; do not leave it enabled in normal build.
5. Enable timer preemption only after all structures are ready.
6. QEMU-test that the marker advances while the GUI thread is live and that
   the GUI still responds.

Gate: timer-driven kernel-to-kernel context switches, yield, unlink, exit, and
deferred destroy work before any user page or ELF code is introduced.

### Phase 6: Process/filedesc/cwdinfo lifecycle

1. Implement process/thread lifecycle and lookup host tests.
2. Implement filedesc.
3. Migrate `fs_context` to process0-owned cwdinfo mechanically.
4. Re-run VFS/FAT/stdio/Noct host tests.
5. Run shell `pwd/cd/cat/source`, multi-drive, startup, and GUI QEMU tests.

Gate: process0 owns the only existing kernel cwd and every old pathname
consumer produces identical behavior.

### Phase 7: Vmspace region layer

1. Implement vmspace create/map/free over HAL.
2. Add fake HAL host tests for success and every allocation/map failure point.
3. Add a test-only user space with one code page and one stack page, but do not
   load from disk yet.
4. Enter CPL 3 using a tiny embedded test instruction sequence and return only
   through the temporary probe/fault paths.

Gate: ring-3 execution, timer preemption back to GUI, and supervisor-only
kernel mapping are proven independently of VFS and ELF parsing.

### Phase 8: ELF and stack host implementation

1. Implement ELF structures/parser and transactional segment map.
2. Implement standard initial stack.
3. Build `INIT.ELF` and static format verification.
4. Run malformed-image, overflow, overlap, short-read, permissions, BSS, and
   cleanup host tests under ASan/UBSan where host build supports them.

Gate: no QEMU integration until host tests prove all untrusted ELF arithmetic
and failure unwind.

### Phase 9: VFS-to-PID1 integration

1. Implement `process_spawn_init("INIT.ELF", ...)`.
2. Install INIT.ELF only into a copied test image.
3. Start PID 1 after boot cwd is ready and immediately return to GUI flow.
4. Verify QMP trap record: magic, count exactly at least 1, vector `0xc2`, RPL
   3 CS, EAX test magic, PID 1, expected TID.
5. Wait additional timer quanta and verify count/marker stability and GUI
   responsiveness through screenshot plus keyboard action.

Gate: the primary acceptance test passes without a syscall implementation.

### Phase 10: User-fault containment

1. Install special fault ELF into a separate copied image.
2. Verify fault metadata, terminal thread state, scheduler unlink, and GUI
   responsiveness.
3. Reap the failed process from process0/test path and verify pmem/heap usage
   returns to the pre-launch level.

Gate: a broken user process cannot halt or corrupt the foreground kernel.

### Phase 11: Cleanup and full regression

1. Remove all temporary compile-time smoke threads except dedicated test
   variants/scripts.
2. Remove old task/space/scheduler symbols and dead compatibility includes.
3. Confirm no production path depends on test marker values except the small
   temporary `INT 0xc2` observer.
4. Run clean all/check, static checks, required QEMU suite, and diff checks.
5. Do not commit or stage.

Gate: final review evidence is complete.

## 24. Host test requirements

### 24.1 Heap isolation

Test:

- allocate sentinels from persistent heap;
- activate a separate Noct heap;
- allocate/free/reset/failure-inject/observe within it;
- restore persistent heap;
- verify original bytes, allocation list, current/peak/error counters;
- verify explicit kernel allocator routes to persistent heap even while Noct
  heap is active;
- verify a pointer cannot be silently freed through the wrong instance.

### 24.2 Scheduler

Use fake HAL tasks/private pointers and deterministic ticks.  Cover:

- highest priority first;
- FIFO and round-robin at equal priority;
- quantum expiry;
- yield requeue;
- sleeping current not requeued;
- wakeup once, duplicate wake rejected;
- timed wake order and wrap-safe comparisons;
- unlink head/middle/tail/only entry;
- zombie/dead never selected;
- idle only when no runnable normal thread;
- exiting current never returns;
- queue invariants after every operation.

### 24.3 Process/thread

Cover process0/thread0, explicit PID1, monotonic IDs, lookup, parent/child and
thread lists, partial-creation unwind, duplicate PID rejection, current-thread
protection, kernel trampoline return to exit, wait/reap, and process free
preconditions.

### 24.4 Vmspace

With a fake HAL, cover user-bound validation, page-zero rejection, split
boundary, addition overflow, overlap, permissions, short pmem allocation,
map failure, multi-region destruction order, no leaked pages, and system
vmspace non-destruction.

### 24.5 ELF

Construct images in memory rather than relying only on toolchain output.  Test
every header field, table bounds, zero/huge counts, integer overflow,
filesz/memsz, bad alignment, no load segment, overlapping rounded segments,
entry outside executable segment, PT_INTERP, short file reads, BSS zeroing,
permission translation, stack bounds/alignment, argv layout, and full cleanup
at each injected failure.

Run relevant host tests with `-Wall -Wextra -Werror`; additionally run new
heap/process/vmspace/ELF tests with ASan/UBSan when the host toolchain permits.

## 25. QEMU test requirements

Use the current reviewed emulator, normally:

```text
/home/awe/qemu-pc98/build/qemu-system-i386
```

with BIOS directory:

```text
/home/awe/qemu-pc98/roms/pc98bios
```

and `-M pc9821 -m 64M -snapshot` or a copied raw image.  Never mutate a source
image under `~/images`.

### 25.1 Primary combined test

`scripts/test-user-init.sh` must:

1. build BOOT.SYS and INIT.ELF;
2. copy a known working HDD image;
3. install BOOT.SYS, INIT.ELF, and a GUI/menu test configuration;
4. start QEMU with no network and a QMP socket;
5. treat compatible-BIOS POST longer than five seconds as a failure signal;
6. locate the probe symbol from `stage2.elf` rather than hardcoding an address;
7. poll physical guest memory for the completed probe magic;
8. validate vector, ring, EAX magic, PID/TID, and nonzero count;
9. wait across additional scheduler ticks;
10. capture a graphical screenshot and validate dimensions/colors;
11. send a menu key and verify the existing menu action/marker;
12. terminate QEMU cleanly and print one PASS line.

The script must have bounded deadlines and kill QEMU in its trap cleanup.

### 25.2 Negative tests

- Missing `INIT.ELF`: error observed, GUI/menu works.
- Malformed `INIT.ELF`: `ENOEXEC` path, no published PID1/thread/page leak,
  GUI works.
- User-fault ELF: fault marker and reaping, GUI works.
- Noct run while PID1 loops: Noct result and heap reset pass, PID1 remains
  schedulable, GUI returns.

### 25.3 Existing regressions

At minimum run after final integration:

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
scripts/test-autoexec-remacs.sh
scripts/test-user-init.sh
```

If runtime makes the entire GUI suite impractical during intermediate phases,
run the most relevant one at each gate, but the final review must include the
primary GDC/Cirrus/menu paths and report exactly which existing scripts were
not run.

## 26. Static and security checks

Before review:

- `git diff --check`;
- no `PTE_USER` on kernel-half PDE creation paths;
- no user initial EFLAGS IOPL 3;
- no direct include of i386 task/space private headers from kernel code;
- no `struct task_info` cast in scheduler/process code;
- no ELF address arithmetic without checked overflow;
- no load mapping at/above `0x80000000` or page zero;
- no current-task call to `hal_task_destroy()`;
- no current-process call to `vmspace_free()`/`process_free_mem()`;
- no generic persistent allocation routed through the active Noct heap;
- no unbounded QEMU polling loop;
- no forbidden newer x86 instructions in INIT.ELF or BOOT.SYS according to
  existing opcode policy;
- BOOT.SYS low/high linker assertions unchanged and passing;
- no edits in explicitly forbidden trees/files;
- no staged changes and no new commit.

## 27. Failure handling and rollback boundaries

Each phase must keep a single clear failure boundary.

- Heap failure: restore previous active heap and return NULL/error; never reset
  the persistent instance as a recovery shortcut.
- HAL space failure: free all page tables and directory pages allocated so far.
- Vmspace region failure: unmap if mapped, free pmem, then free bookkeeping.
- ELF failure: destroy the unpublished candidate vmspace/process completely.
- Thread creation failure: clear HAL private pointer before destroying task.
- Scheduler invariant failure: stop with a diagnostic; do not continue with a
  corrupted queue.
- Init launch failure: report and return to GUI; do not retry every menu loop.
- User fault: unlink/yield, then defer resource destruction.

Because implementation is intentionally uncommitted, do not use `git reset
--hard`, `git checkout --`, or deletion of the entire worktree as rollback.
Reverse only the current phase's known edits with reviewed patches.  Preserve
all user changes even if they appear during implementation.

## 28. Final review evidence

The implementation handoff must contain:

1. concise statement that GUI/thread0 and background ring-3 PID1 run together;
2. `git status --short` and a categorized diff summary;
3. exact build/host/QEMU commands and PASS/FAIL results;
4. QEMU path, BIOS path, base image, copied test image, and timeout values;
5. the decoded `INT 0xc2` probe values;
6. evidence that the GUI responded after the probe;
7. malformed/missing ELF and user-fault results;
8. BOOT.SYS size/linker margins and INIT.ELF `readelf` summary;
9. heap/pmem leak-accounting result before/after failed process cleanup;
10. known limitations, especially no syscall ABI, no fork, no COW, no dynamic
    ELF, no general user libc, and temporary embedded-Noct heap activation;
11. confirmation that no commit or staging was performed.

The reviewer, not the implementation agent, decides whether and how to commit.
