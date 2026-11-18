# HAL and BSP Architecture

Status: current

zedBSD enforces a clean separation between machine-independent kernel services,
architecture-dependent execution mechanisms, and board-specific peripheral/boot
bindings. This document describes the structure of the Hardware Abstraction
Layer (HAL), the two-tier Board Support Package (BSP) model, and the procedures
for porting zedBSD to new CPU architectures and new boards.

For higher-level layer boundaries and memory ownership, see
[Kernel, HAL and driver boundaries](kernel-and-hal.md).

---

## 1. Architectural Overview and Boundaries

```
+--------------------------------------------------------------------------+
|                     Userland Applications & Services                     |
+--------------------------------------------------------------------------+
                                    | System Calls (syscall / ecall / svc)
+--------------------------------------------------------------------------+
|                        Generic Kernel (src/kern/)                        |
|   VFS, Process/Thread Lifecycle, Scheduler, POSIX Semantics, Networking   |
+--------------------------------------------------------------------------+
            |                                           |
            | kern_platform_*                           | hal_*
            v                                           v
+-------------------------------+   +--------------------------------------+
|  Kernel Platform Unit         |   |  Hardware Abstraction Layer (HAL)   |
|  src/kern/platform/<board>.c  |   |  src/hal/<arch>/                     |
|  - Boot disk discovery        |   |  - MMU & Page tables (space.c)       |
|  - Bus probing (PCI, ACPI, DT)|   |  - Context switch & Tasks (task.c)   |
|  - Console input & devfs hook |   |  - Traps & Interrupt dispatch (int.c)|
|  - Board power & reset hooks  |   |  - SMP & IPI routing (smp.c)         |
+-------------------------------+   |  - Early C runtime & console (cons.c)|
            |                       +--------------------------------------+
            |                                           |
            +--------------------+                      | Low-level HW
                                 v                      v
                    +--------------------------------------+
                    |  HAL Board Support Package (BSP)     |
                    |  src/hal/<arch>/bsp-<board>/         |
                    |  - Early UART / GOP text console     |
                    |  - PIC / LAPIC / GIC / PLIC setup    |
                    |  - Firmware handoff (FDT / ACPI)     |
                    +--------------------------------------+
```

The system organizes hardware support across three decoupled layers:

1. **CPU Architecture Layer (`src/hal/<arch>/`)**:
   Implements the ISA-level hardware mechanisms: processor execution modes, MMU
   page table formats, context switching, CPU register save/restore, interrupt/trap
   trampolines, atomic operations, cache control, and memory barriers.
   Public interfaces are declared in [`include/hal/hal.h`](../../include/hal/hal.h) and
   [`include/hal/arch.h`](../../include/hal/arch.h).

2. **HAL Board Support Package (`src/hal/<arch>/bsp-<board>/`)**:
   Implements pre-kernel board bring-up under the CPU architecture directory:
   locating and parsing firmware tables (ACPI, Flattened Device Tree), bringing up
   the early UART or GOP framebuffer console, initializing the primary interrupt
   controller (x86 APIC, ARM GIC, RISC-V PLIC), and calibrating the system timer.

3. **Kernel Platform Unit (`src/kern/platform/<board>.c`)**:
   Implements the kernel's platform contract declared in
   [`include/kern/platform.h`](../../include/kern/platform.h). Each supported
   board has exactly one translation unit in `src/kern/platform/`. It bridges the
   generic kernel to board devices: locating the boot storage medium, probing
   platform buses, wiring up console input devices, and orchestrating system halt,
   poweroff, and reboot.

---

## 2. HAL Architecture & Subsystems

The HAL interface in [`include/hal/hal.h`](../../include/hal/hal.h) exposes
primitive mechanisms to the generic kernel without imposing kernel policy.

### 2.1 Early Runtime & Logging

- **HAL C Runtime**: Low-dependency string and memory routines (`hal_strlen`,
  `hal_memset`, `hal_memcpy`). Used only in the HAL and early kernel setup.
- **Early Console**: `hal_putchar()`, `hal_putc()`, `hal_puts()`, and `hal_printf()`.
  Before the kernel console is ready, writes go to the early hardware UART or
  firmware framebuffer. Once `kernel_putc` is registered, early logging delegates
  to the kernel log buffer.
- **Diagnostics**: `HAL_ASSERT(expr)` and `HAL_FATAL(msg)` halt the CPU with file
  and line diagnostics.

### 2.2 CPU and SMP Management

- **Topology & ID**: `hal_cpu_count()` reports online CPUs; `hal_cpu_id()` returns
  the caller's logical CPU ID (`0` to `HAL_CPU_MAX - 1`).
- **Inter-Processor Interrupts (IPI)**: `hal_cpu_notify(cpu)` and `hal_cpu_notify_mask(mask)`
  send an IPI to target cores to process rescheduled threads or TLB shootdowns.
  Target cores invoke `kernel_ipi_entry()`.
- **Power Management & Idle**: `hal_cpu_idle_suspend()` enters the deepest safe
  low-power sleep state until an interrupt arrives.
- **Secondary CPU Bootstrapping**: Secondary cores are initialized by the HAL and
  wait in an architectural park/trampoline until kicked by the primary core. When
  released, secondary cores enter `kernel_secondary_entry(cpu_id)`.

### 2.3 Interrupts and Exception Routing

- **State Flags**: `hal_irq_disable()` and `hal_irq_enable()` control interrupt
  delivery on the calling core.
- **Vector Registration**:
  - `hal_irq_register(irq, handler, ctx)`: Registers a line-based interrupt.
  - `hal_irq_mask(irq)` / `hal_irq_unmask(irq)`: Masks/unmasks line interrupts at
    the controller.
  - `hal_irq_set_trigger(irq, trigger, polarity)`: Configures level/edge and
    active-high/low polarity.
  - `hal_irq_eoi(ack)`: Sends End of Interrupt acknowledgment to the hardware.
- **Message-Signaled Interrupts (MSI / MSI-X)**:
  `hal_irq_alloc_msi()`, `hal_irq_attach_msi()`, and `hal_irq_free_msi()` allocate
  interrupt vectors, target addresses, and data payloads for PCI MSI devices.
- **System Traps**: Hardware exceptions (page faults, division by zero, invalid
  opcodes) and system calls enter through assembly entrypoints (`trap.S`) and
  dispatch to `kernel_syscall()`, `kernel_user_fault()`, or `kernel_system_fault()`.

### 2.4 Virtual Memory and Address Spaces (`hal_space_*`)

The HAL manages page tables and hardware address translations:
- **Address Space Lifecycle**:
  - `hal_space_create()`: Creates a new user address space sharing the global
    kernel mappings.
  - `hal_space_destroy()`: Releases page table structures.
  - `hal_space_switch(space)`: Loads the page directory root (e.g. CR3 on x86_64,
    TTBR0 on ARM64, satp on RISC-V) and performs necessary TLB sync.
- **Page Operations**:
  - `hal_space_map(space, va, pa, size, attr)`: Maps virtual to physical pages
    with attributes (`HAL_PAGE_ATTR_READ`, `HAL_PAGE_ATTR_WRITE`,
    `HAL_PAGE_ATTR_EXEC`, `HAL_PAGE_ATTR_USER`).
  - `hal_space_unmap(space, va, size)`: Unmaps a virtual range.
  - `hal_space_map_device(pa, size, attr)`: Creates an unmanaged kernel MMIO mapping.
  - `hal_space_protect(space, va, size, attr)`: Updates page protections.
  - `hal_space_flush_tlb()` and `hal_space_flush_tlb_range()`: Invalidates TLB entries.

### 2.5 Physical Memory Management (`hal_pmem_*`)

- **Size and Allocator**:
  - `hal_pmem_size()`: Reports total detected physical memory in bytes.
  - `hal_pmem_alloc(size, align)`: Allocates contiguous physical memory blocks during
    boot or early bootstrap.
  - `hal_pmem_alloc_device(size, align, boundary, max_addr)`: Allocates DMA-safe
    physical memory satisfying hardware constraints (e.g., 32-bit DMA boundary).
  - `hal_pmem_free(pa, size)`: Frees allocated physical memory.
  - `hal_pmem_p2v(pa)`: Translates physical addresses within the direct-mapped
    kernel physical memory window.

### 2.6 Tasks and Context Switching (`hal_task_*`)

- **Task Lifecycle**:
  - `hal_task_create(space, entry, arg, stack, stack_size)`: Initializes a task
    context and sets up the execution frame.
  - `hal_task_destroy(task)`: Frees task-specific HAL state.
  - `hal_task_switch(prev, next)`: Saves current CPU registers and switches to the
    next task context.
- **Thread Local Storage (TLS)**:
  - `hal_task_set_tls(task, tls_ptr)` and `hal_task_get_tls(task)`.
- **Registers & Debugging**:
  - `hal_task_user_regs()`: Accesses user-mode register state during syscalls or traps.
  - `hal_task_set_instruction_trap()`: Enables single-step execution.
  - `hal_task_debug_set()`: Programs hardware breakpoints/watchpoints.

### 2.7 Hardware Timers and Clocks

- **Wall Clock (RTC)**: `hal_rtc_read(seconds)` reads Unix epoch time.
- **Monotonic Timecounter**: `hal_timecounter_read()` returns a continuous,
  high-resolution tick count for the kernel scheduler and high-precision timers.

---

## 3. BSP Architecture and the Kernel Contract

zedBSD divides board support into two decoupled tiers:

### Tier 1: HAL-Level Machine Bring-up (`src/hal/<arch>/bsp-<board>/`)

Provides low-level initialization routines invoked directly by `src/hal/<arch>/cmain.c`:
- **Early Console**: Serial UART driver (`cons.c`) that outputs bytes before the
  kernel console or memory management is up.
- **Interrupt Controller**: Setup for hardware interrupt distributors and local
  interrupt controllers (e.g., IOAPIC + LAPIC, GICv2/v3, PLIC).
- **Firmware Envelope**: Ingestion and validation of firmware-provided boot
  structures (ACPI tables, FDT blob, or UEFI GOP framebuffer metadata).

### Tier 2: Kernel Platform Unit (`src/kern/platform/<board>.c`)

Each platform defines a single translation unit in `src/kern/platform/` implementing
the hooks in [`include/kern/platform.h`](../../include/kern/platform.h):

```c
size_t
kern_platform_init(
    const struct kern_boot_handoff *handoff,
    struct kern_boot_device *devices,
    size_t capacity);

struct disk *
kern_platform_block_device(
    const struct kern_boot_device *device);

int
kern_platform_input_init(void);

void
kern_platform_debug_write(
    const char *text);

void
kern_platform_halt(void) __attribute__((noreturn));

void
kern_platform_reboot(void) __attribute__((noreturn));

int
kern_platform_poweroff(void);

void *
kern_boot_handoff(
    const char *name);
```

#### Responsibilities of the Platform Unit:
1. **Boot Device Registration**: Discovers and populates `devices[]` with the
   available storage devices (e.g., NVMe, SD card, RAM disk, USB drive) so the
   generic kernel can locate the root filesystem.
2. **Device Bridge**: Resolves `kern_platform_block_device()` to an instantiated
   `struct disk *` corresponding to the chosen boot device.
3. **Subsystem Probing**: Triggers discovery on board buses (probes PCI host
   controllers, scans ACPI namespaces, or parses FDT device nodes) and registers
   platform-specific drivers.
4. **Input Initialization**: Calls `kern_platform_input_init()` to bind PS/2,
   serial, or HID keyboard drivers to the kernel console.
5. **System Reset / Power**: Handles ACPI sleep transitions, PMIC cutoffs, PSCI
   calls, or watchdog resets for halt, reboot, and poweroff.

---

## 4. How to Add a New CPU Architecture

Porting zedBSD to a new CPU architecture (e.g. `riscv32`, `mips64`, `loongarch64`)
requires implementing the HAL interface and architecture headers.

### Step 1: Create Architecture Headers

1. Create `include/hal/arch/<newarch>.h`:
   - Define architectural register structures: `struct hal_user_regs`,
     `struct hal_trap_frame`, `struct hal_fpu_state`.
   - Define stack layout and alignment rules (`HAL_STACK_ALIGN`).
   - Define trap numbers and fault codes.
2. Update [`include/hal/arch.h`](../../include/hal/arch.h) to conditionally include
   `<hal/arch/<newarch>.h>` based on `__<newarch>__` or compiler macros.
3. Provide atomic primitives in `include/hal/arch/atomic/<newarch>.h` or map them to
   compiler built-in atomics (`__atomic_*`).

### Step 2: Implement the Architecture HAL (`src/hal/<newarch>/`)

Create the directory `src/hal/<newarch>/` and implement the core HAL modules:

| Source File | Function & Responsibility |
| --- | --- |
| `locore.S` | Assembly entry point. Sets up the initial bootstrap stack, switches CPU to supervisor privilege mode, invalidates caches, and jumps to `hal_cmain()`. |
| `cmain.c` | First C function executed. Validates boot handoff parameters, initializes early console, calls `hal_space_init()`, `hal_pmem_init()`, and transfers control to `kernel_entry(0)`. |
| `space.c` & `page.c` | Implements `hal_space_*`: MMU page table creation, hardware page directory traversal, TLB flushing, and direct physical mapping. |
| `task.c` & `dispatch.S` | Implements `hal_task_*`: task context structures, `hal_task_switch()` assembly context switch, kernel/user stack pointers, and TLS registers. |
| `trap.S` & `int.c` | Low-level exception vector table. Saves scratch registers onto the trap frame, calls `kernel_syscall()`, `kernel_user_fault()`, or interrupt handlers, and restores registers on return. |
| `smp.c` & `percpu.c` | Secondary core trampoline (`ap-trampoline.S`), IPI send/receive handling, and per-CPU state structures. |
| `asm.c` & `asm.h` | Low-level inline assembly helpers: memory barriers (`hal_mb`), cache invalidation (`hal_cache_flush`), and CPU control register access. |

### Step 3: Implement Early Platform Support

Implement early console output (`cons.c`) and clock counter (`clock.c`) so that
`hal_printf()` works before the memory allocator is active.

### Step 4: Linker Scripts and Toolchain Integration

1. Create `mk/platform-<target>/vmunix.ld`:
   - Set virtual load address (e.g. `0xFFFFFFFF80000000` or `0x80000000`).
   - Lay out text, rodata, data, and BSS sections with required page alignment.
2. Create userland linker script `mk/platform-<target>/user.ld`.
3. Add libc system call stubs and setjmp/longjmp in `src/libc/arch/<newarch>/`.

---

## 5. How to Add a New BSP / Platform

Adding support for a new board using an existing architecture (e.g., adding an
Allwinner, Rockchip, or NXP board on `arm64`, or a new RISC-V SoC) requires three
main components: the HAL-level board bring-up, the kernel platform translation unit,
and the build system configuration.

### Step 1: Create the HAL BSP Module (If hardware initialization differs)

If the new board requires unique early hardware bring-up (e.g. a different early
UART address or custom interrupt controller routing):

1. Create `src/hal/<arch>/bsp-<board>/`.
2. Implement:
   - `cons.c`: Early UART output driver matching the board's MMIO UART base address
     and register stride.
   - `boot.c`: Parsing device tree (FDT) or ACPI handoff tables passed by the
     bootloader.
   - `clock.c` / `int.c`: Board-specific interrupt controller or timer hooks.

*(Note: If the board uses a standard firmware interface like UEFI + ACPI on x86_64,
the existing `bsp-pcat` can be reused directly.)*

### Step 2: Implement the Kernel Platform Unit (`src/kern/platform/<board>.c`)

Create a single self-contained C translation unit in `src/kern/platform/<board>.c`:

```c
#include <hal/hal.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/platform.h>

/* Track discovered boot device */
static struct disk *board_boot_disk = NULL;

size_t
kern_platform_init(
    const struct kern_boot_handoff *handoff,
    struct kern_boot_device *devices,
    size_t capacity)
{
    /* 1. Parse bootloader handoff (framebuffer, ramdisk, boot arguments) */
    /* 2. Probe platform buses (PCI host controllers, SoC buses, ACPI/DT) */
    /* 3. Initialize storage devices and populate devices[0..count-1] */
    /* 4. Return number of devices populated */
    return count;
}

struct disk *
kern_platform_block_device(const struct kern_boot_device *device)
{
    return board_boot_disk;
}

int
kern_platform_input_init(void)
{
    /* Attach keyboard/input drivers to console */
    return 0;
}

void
kern_platform_debug_write(const char *text)
{
    while (*text) {
        hal_putc((unsigned char)*text);
        text++;
    }
}

void
kern_platform_halt(void)
{
    (void)hal_irq_disable();
    for (;;)
        hal_halt();
}

void
kern_platform_reboot(void)
{
    /* Trigger SoC watchdog, PSCI, or ACPI reboot */
    hal_reset();
    for (;;)
        hal_halt();
}

int
kern_platform_poweroff(void)
{
    /* Trigger PMIC power off or ACPI S5 */
    hal_poweroff();
    return 0;
}

void *
kern_boot_handoff(const char *name)
{
    /* Expose named handoff properties (e.g. "fdt.blob", "framebuffer") */
    return NULL;
}
```

Register the new platform in [`src/kern/platform/README.md`](../../src/kern/platform/README.md).

### Step 3: Create Platform Build Configuration (`mk/platform-<board>/`)

Create the directory `mk/platform-<board>/` containing the platform build rules:

1. **`defaults.mk`**:
   Sets target architecture, board name, and default configuration options:
   ```makefile
   override ARCHITECTURE := arm64
   override BOARD        := myboard

   CONFIG_ROOTFS_DEVELOPMENT := y
   CONFIG_BOOT_SPLASH        := y
   CONFIG_DRIVER_PCI         := y
   CONFIG_DRIVER_USB         := y
   CONFIG_DRIVER_FS_UFS      := y
   CONFIG_DRIVER_FS_FAT      := y
   ```
2. **`vmunix.mk`**:
   Configures compiler flags, include paths, and platform sources:
   ```makefile
   ARCH_CFLAGS  += -mcpu=cortex-a72 -march=armv8-a
   ARCH_LDFLAGS += -T mk/platform-myboard/vmunix.ld

   # HAL sources
   HAL_SRCS += src/hal/arm64/locore.S \
               src/hal/arm64/cmain.c \
               src/hal/arm64/space.c \
               src/hal/arm64/task.c \
               src/hal/arm64/bsp-myboard/cons.c

   # Platform unit
   PLATFORM_SRCS += src/kern/platform/myboard.c
   ```
3. **`vmunix.ld`**:
   Linker script specifying entry point (`hal_start` or `_start`), memory sections,
   and page alignment.
4. **`bootloader.mk` & `image.mk`**:
   Targets for creating the bootloader artifact (e.g. UEFI `.efi`, U-Boot `fitImage`,
   or Raspberry Pi `kernel8.img`) and constructing the bootable disk image
   (FAT boot partition + UFS root partition).
5. **`Makefile`**:
   Standard target entrypoint including `defaults.mk`, `vmunix.mk`, and `image.mk`.

### Step 4: Verification Checklist

1. **Kernel Compilation**:
   ```bash
   make PLATFORM=<board> vmunix -j16
   ```
   Ensure linking succeeds with no undefined `hal_*` or `kern_platform_*` symbols.
2. **Early UART Console**:
   Boot under an emulator (e.g. QEMU) or physical board and verify that `hal_printf`
   banner messages appear on the serial console.
3. **Memory Translation**:
   Verify that `hal_space_create()` and initial page directory switching succeed
   without faults.
4. **Platform Hand-off**:
   Confirm that `kern_platform_init()` detects storage devices and that the root
   filesystem is mounted.
5. **Disk Image Build**:
   ```bash
   make PLATFORM=<board> disk -j16
   ```
   Ensure the disk image formats cleanly and contains both boot configuration and
   rootfs.

---

## 6. Execution Flow: From Firmware to Userland

```
[Firmware / Bootloader] (UEFI / U-Boot / Raspberry Pi VideoCore)
        |
        v
[src/hal/<arch>/locore.S]
        - Setup bootstrap stack
        - CPU mode setup (Supervisor / EL1)
        - Basic MMU/cache invalidation
        |
        v
[src/hal/<arch>/cmain.c]
        - Initialize early console (cons.c)
        - Validate boot handoff parameters
        - Setup early physical memory allocator (hal_pmem)
        - Initialize kernel page tables (hal_space)
        |
        v
[src/kern/main.c: kernel_entry()]
        - Core subsystem initialization (VM, scheduler, vfs, klog)
        - Initialize interrupts (hal_irq)
        |
        v
[src/kern/platform/<board>.c: kern_platform_init()]
        - Probe platform buses (PCI, ACPI, FDT)
        - Initialize storage controllers (NVMe, AHCI, SDHCI)
        - Register boot block devices
        |
        v
[Root Filesystem Mount & Init Process Spawning]
        - Mount root disk (UFS)
        - Create user address space (hal_space_create)
        - Load ELF binary (/sbin/init)
        - Dispatch to user mode (hal_task_switch)
```
