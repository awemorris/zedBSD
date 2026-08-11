# Source layout, HAL console, and Stage 2 refactoring plan

This document is the implementation plan to be reviewed before the refactoring
starts.  It supersedes the source-layout and console-placement portions of the
older HAL integration notes where they disagree with the decisions below.

## 1. Goals

The refactoring has four goals.

1. Put 32-bit HAL, kernel, and embedded-Noct glue under a conventional
   `include/` and `src/` layout.
2. Make the kernel entry point an ordinary C function.  Assembly remains only
   where CPU state actually has to be changed.
3. Make every 32-bit text-console user go through the HAL console.  Remove the
   independent PC-98 console implementation and its duplicate cursor state.
4. Split the current `platform/pc98/stage2.c` into a small PC-98 platform
   initializer and platform-independent kernel services.

The work is a refactoring.  Existing boot, menu, shell, Noct, BeUI, disk, and
Linux-loading behavior must continue to work after the move.

## 2. Decisions fixed by review

The following decisions are requirements, not open design questions.

- `platform/pc98/stage2.c` is 32-bit code and must use the HAL console.
- `platform/pc98/noct-platform.c` moves under `src/noct/` and uses the HAL
  console.  `src/noct/` is kernel-resident glue temporarily; it is not the
  Noct language implementation.  The Noct submodule remains at `noct/`.
- `platform/pc98/console.c` is removed after its missing functionality has
  been merged into `src/hal/i386/bsp-pc98/cons.c`.
- The HAL console accepts UTF-8.  No new `hal_cons_put_sjis_at()` or
  `hal_cons_puts_sjis()` API is introduced, and all old Shift-JIS-only console
  entry points are removed.
- The UTF-8 decoder and Unicode-to-PC-98 JIS kuten conversion are copied into
  the PC-98 HAL console.  The HAL console must not depend on a Noct symbol or
  on `noct/src/api/jisx0208.c`, because Noct will later become a user process.
- PC-98 keyboard hardware handling and scan-code translation move into
  `src/hal/i386/bsp-pc98/cons.c`.  `cons_getc()` uses the existing native
  PC-98 keyboard driver's event queue and normalized-key behavior.
- A platform-independent `drivers/keyboard.c` may provide kernel-facing
  wrappers, but it only calls the HAL console/input API and contains no PC-98
  port I/O or scan-code table.
- The PC-98 IDE driver remains under `drivers/`, renamed from
  `drivers/ide-pc98.c` to `drivers/pc98-ide.c` (and likewise for its header).
- The C conversion of `kernel_entry()` does not include the later task model.
  Moving normal work to a task and leaving the initial CPU context in an HLT
  loop is a separate future change.
- The kernel boot-stack switch is moved from `kern/entry.S` to HAL assembly
  before the first ordinary C kernel entry is called.  Validation found that
  Noct/JIT already uses about 49 KiB: the old nominal 32 KiB stack survived by
  spilling into link-layout padding.  The explicit stack is therefore 64 KiB;
  task creation and the eventual HLT-only initial context remain deferred.

## 3. Explicitly deferred work

The following already-agreed features are not mixed into this structural
refactoring.  They start only after the refactored baseline passes all tests.

- Replace `fs.h` with the minimal inode/VFS layer.
- Mount every supported filesystem partition as a dense `/disk1`, `/disk2`,
  ... sequence, rather than mounting one filesystem per physical disk.
- Extend the Stage 1 handoff with the boot partition LBA and set the initial
  current directory to the boot partition's `/diskN` mount.
- Add shell `cd` and `pwd`, make shell file commands path-oriented, change the
  prompt, and rename `devalias` to `device`.
- Create the first scheduled task and turn the initial CPU context into an HLT
  loop.

Keeping these changes separate makes a console/layout regression distinguishable
from a VFS or scheduler regression.

## 4. Resulting source tree

The intended tree after this refactoring is:

```text
include/
  hal/
    hal.h
    clock.h
    console.h
    framebuffer.h
    irq.h
    memory.h
    runtime.h
    task.h
  kern/
    block.h
    boot.h
    device.h
    env.h
    fs.h
    image.h
    namespace.h
    noct.h
    partition.h
    platform.h
    sched.h
    shell.h

src/
  hal/
    i386/
      asm.h
      defs.h
      i386.h
      locore.S
      trap.S
      dispatch.S
      cmain.c
      fb.c
      int.c
      irq.c
      lib.c
      page.c
      task.c
      univ.c
      bsp-pc98/
        clock.c
        cons.c
        dma.h
        pic.c
        display.c
  kern/
    entry.c
    main.c
    clock.c
    shell.c
    startup.c
    device.c
    loader.c
    sched-stub.c
    block.c
    env.c
    fs.c
    fat.c
    fat16.c
    image.c
    namespace.c
    partition.c
    pc98/
      platform.c
      partition.c
      linux-boot.c
      linux-entry.S
  noct/
    noct.c
    memory.c
    napi.c
    platform.c
    target.c
    pc98-beui.c

drivers/
  pc98-ide.c
  pc98-ide.h
  keyboard.c
  keyboard.h
```

Private i386 and BSP headers stay beside their implementation under `src/hal`.
Only interfaces used outside the HAL are installed under `include/hal`.

`platform/pc98/` retains build and boot-format material rather than the 32-bit
runtime implementation:

```text
platform/pc98/
  boot-header.S
  stage2.ld
  applet.ld
  fat-loader.S
  platform.mk
  dos/
```

The 16-bit IPL and Stage 1 sources remain under `bootsectors/pc98/`.

## 5. File migration table

### 5.1 HAL and kernel headers

| Current path | New path or action |
| --- | --- |
| `hal/include/hal/hal.h` | `include/hal/hal.h`, retained as an umbrella |
| `hal/include/sys/hal/cons.h` | `include/hal/console.h` |
| `hal/include/sys/hal/clock.h` | `include/hal/clock.h` |
| `hal/include/sys/hal/fb.h` | `include/hal/framebuffer.h` |
| `hal/include/sys/hal/irq.h` | `include/hal/irq.h` |
| `hal/include/sys/hal/pmem.h` | `include/hal/memory.h` |
| `hal/include/sys/hal/task.h` | `include/hal/task.h` |
| `hal/include/sys/hal/univ.h` | fold into the appropriate HAL public header |
| `hal/include/sys/kcrt/kcrt.h` | `include/hal/runtime.h` |
| `hal/include/sys/kern/sched.h` | `include/kern/sched.h` |
| `hal/include/sys/types.h` | remove; use freestanding `stdint.h`/`stddef.h` |

The existing duplicate HAL integer type world is removed.  Kernel files no
longer need local declarations merely to avoid including HAL headers.

### 5.2 HAL implementation

| Current path | New path or action |
| --- | --- |
| `hal/i386/*` | `src/hal/i386/*` |
| `hal/i386/bsp-pc98/clock.c` | `src/hal/i386/bsp-pc98/clock.c` |
| `hal/i386/bsp-pc98/pic.c` | `src/hal/i386/bsp-pc98/pic.c` |
| `hal/i386/bsp-pc98/cons.c` | `src/hal/i386/bsp-pc98/cons.c`, expanded as specified below |
| `platform/pc98/display-pc98.c` | `src/hal/i386/bsp-pc98/display.c` |
| `platform/pc98/display-pc98.h` | public parts under `include/hal/framebuffer.h` |

### 5.3 Kernel and filesystem implementation

| Current path | New path |
| --- | --- |
| `core/blkdev.*` | `src/kern/block.c`, `include/kern/block.h` |
| `core/env.*` | `src/kern/env.c`, `include/kern/env.h` |
| `core/fs.*` | `src/kern/fs.c`, `include/kern/fs.h` for this phase |
| `core/fat.*` | `src/kern/fat.c`, kernel headers as required |
| `core/fat16.*` | `src/kern/fat16.c`, kernel headers as required |
| `core/image.*` | `src/kern/image.c`, `include/kern/image.h` |
| `core/namespace.*` | `src/kern/namespace.c`, `include/kern/namespace.h` |
| `core/partition.*` | `src/kern/partition.c`, `include/kern/partition.h` |
| `core/messages.txt` | `src/kern/messages.txt` |
| `kern/sched-stub.c` | `src/kern/sched-stub.c` |
| `platform/pc98/partition-pc98.c` | `src/kern/pc98/partition.c` |
| `platform/pc98/partition-pc98.h` | internal PC-98 kernel header or `include/kern/partition.h` |
| `drivers/ide-pc98.c` | `drivers/pc98-ide.c` |
| `drivers/ide-pc98.h` | `drivers/pc98-ide.h` |

### 5.4 Entry, console, Noct, and obsolete files

| Current path | New path or action |
| --- | --- |
| `kern/kmain.c` | split into `src/kern/entry.c`, `main.c`, and `clock.c` |
| `kern/entry.S` | remove after its stack switch moves to `locore.S` |
| `platform/pc98/stage2.c` | split according to section 8, then remove |
| `platform/pc98/stage2-entry.S` | remove; it is not linked by current targets |
| `platform/pc98/console.c` | merge into HAL cons, then remove |
| `core/console.h` | replace with `include/hal/console.h` |
| `platform/pc98/timer.c/.h` | remove; current HAL PIT/tick path supersedes it |
| `platform/pc98/exit-trampoline.S` | `src/kern/pc98/linux-entry.S` |
| `core/noct.c` | `src/noct/noct.c` |
| `core/noct-memory.c` | `src/noct/memory.c` |
| `core/noct-napi.c` | `src/noct/napi.c` |
| `platform/pc98/noct-platform.c` | `src/noct/platform.c` |
| `platform/pc98/noct-target.c` | `src/noct/target.c` |
| PC-98 BeUI construction currently in `stage2.c` | `src/noct/pc98-beui.c` |

The Noct submodule itself is not moved or modified by this refactoring.

## 6. C kernel entry and stack ownership

The current `kern/entry.S` does two things: it changes to a boot stack and
calls C.  The second operation does not require assembly.

The replacement sequence is:

1. `locore.S` continues using its 4 KiB initial setup stack while it creates
   the GDT, paging, TSS, and IDT.
2. Immediately before entering ordinary HAL C initialization, `locore.S`
   switches to the 64 KiB boot stack, now owned by the HAL BSS.  This size
   makes explicit the space that the old layout supplied accidentally below
   its nominal 32 KiB allocation.
3. `locore.S` reloads the saved Stage 1 handoff pointer and calls
   `cmain(handoff)` using the i386 C ABI.
4. `cmain()` performs HAL initialization and calls the ordinary C function
   `kernel_entry(handoff)`.
5. `kernel_entry()` installs allocator hooks, calls
   `kern_platform_init(handoff, &boot_context)`, and then calls
   `kernel_main(&boot_context)`.

`kern_platform_init()` is implemented by `src/kern/pc98/platform.c`.  It alone
interprets `struct boots_handoff`, copies the PC-98 firmware device table,
selects the PC-98 partition scheme, initializes the native IDE driver, and
registers the PC-98 Linux image loader.  `kernel_main()` does not include the
PC-98 ABI header.

This phase does not create or switch to a scheduled task.  `kernel_main()`
continues to run on the boot stack until the later scheduler refactoring.

## 7. HAL console and PC-98 keyboard consolidation

### 7.1 One console state

`src/hal/i386/bsp-pc98/cons.c` becomes the single owner of:

- PC-98 text and attribute VRAM access;
- terminal row and column;
- fixed-menu and scrolling-terminal mode;
- row clearing and scrolling;
- GDC hardware cursor position and visibility;
- framebuffer-active suppression;
- UTF-8 decoding and conversion to PC-98 character cells;
- PC-98 keyboard receive, mapping, pressed-key state, modifiers, and event
  queue.

There must be no second cursor or terminal state in the kernel or Noct glue.
HAL diagnostics, libc output, the startup menu, the shell, and Noct all use
the same state.

### 7.2 Public output API

`include/hal/console.h` provides at least:

```c
enum hal_cons_mode {
        HAL_CONS_FIXED_MENU,
        HAL_CONS_TERMINAL,
};

struct hal_cons_state;

void hal_cons_init(void);
void hal_cons_reset(void);
void hal_cons_clear(void);
void hal_cons_clear_row(unsigned row);
void hal_cons_set_mode(enum hal_cons_mode mode);
void hal_cons_putc(int ascii_or_control);
size_t hal_cons_write_utf8(const char *text, size_t length);
int hal_cons_put_utf8_at(unsigned row, unsigned column,
                         const char *text, size_t length,
                         uint8_t attribute);
int hal_cons_clear_to_eol_at(unsigned row, unsigned column);
int hal_cons_move_cursor(unsigned row, unsigned column);
void hal_cons_show_cursor(int visible);
void hal_cons_update_cursor(void);
void hal_cons_save_state(struct hal_cons_state *state);
void hal_cons_restore_state(const struct hal_cons_state *state);
```

`hal_cons_putc()` is for ASCII and control characters used by HAL diagnostics.
All strings and positioned non-ASCII text use the length-aware UTF-8 API.

There is deliberately no Shift-JIS public API.  `src/kern/messages.txt` stays
UTF-8, and `scripts/generate-messages.py` changes from CP932 byte generation to
escaped UTF-8 byte generation.  Startup menu callers then use the UTF-8 HAL
entry points.

### 7.3 UTF-8 to PC-98 conversion

The following behavior is copied from `platform/pc98/console.c` into the HAL
PC-98 cons implementation:

- strict UTF-8 decoding with invalid input replaced by `?`;
- ASCII and half-width katakana handling;
- Unicode lookup in the JIS X 0208 table;
- JIS ku/ten to PC-98 text-VRAM character-code conversion;
- one-cell and two-cell width handling.

The lookup table is copied into the HAL/BSP source with an HAL-owned symbol.
The HAL must remain linkable after all `src/noct/` objects and the Noct
submodule are removed.  The temporary duplicate table size is included in the
low-segment size audit.

### 7.4 Keyboard implementation

The code currently in `drivers/kbd-pc98.c` and `drivers/kbd-pc98-map.c` moves
into `src/hal/i386/bsp-pc98/cons.c`:

- ports `0x41` and `0x43`;
- make/break handling;
- base and shifted JIS keyboard maps;
- normalized special keys;
- Shift, Ctrl, and Graph state;
- the 32-entry event queue;
- non-consuming poll, consuming read, state query, and drain behavior.

The BSP-private functions are:

```c
int cons_getc(void);              /* blocking, normalized key */
int cons_poll(void);              /* non-consuming, -1 if empty */
int cons_key_state(int key);
void cons_drain(void);
unsigned cons_modifiers(void);
int cons_read_event(void);        /* normalized key + modifier snapshot */
int cons_poll_event(void);
```

The public HAL wrappers use `hal_cons_*` names.  Their event contract keeps
the existing key and modifier bit values until Noct becomes a user process.

`drivers/keyboard.c` and `drivers/keyboard.h`, if retained, are thin
platform-independent adapters.  They call only these HAL functions.  They do
not include PC-98 constants, issue port I/O, or carry scan-code tables.

The old `drivers/kbd-pc98.c`, `drivers/kbd-pc98.h`,
`drivers/kbd-pc98-map.c`, and `drivers/kbd-pc98-map.h` are removed after their
host tests have been transferred to an HAL PC-98 cons/keyboard test.

## 8. Splitting `stage2.c`

The large file is removed only after all functions have a tested destination.

| Current responsibility/functions | Destination |
| --- | --- |
| `boots_main()` outer loop | `src/kern/main.c:kernel_main()` |
| handoff validation and device descriptor copy | `src/kern/pc98/platform.c:kern_platform_init()` |
| `blk_for_dev`, sector read/write adapters | `src/kern/device.c` plus PC-98 binding in `platform.c` |
| `scanparts`, mount helpers, device/partition selection | `src/kern/device.c` |
| `prompt`, `line`, `split`, `number`, `command` | `src/kern/shell.c` |
| `ls`, `catfile`, applet loading and CRC | `src/kern/shell.c` or `loader.c` |
| IDE/SCSI reported-device maps and probe helpers | `src/kern/device.c` |
| startup state machine and automatic-selection policy | `src/kern/startup.c` |
| startup menu drawing and progress display | `src/kern/startup.c` using HAL cons |
| `vmlinux_probe`, `vmlinux_load`, PC-98 setup nodes | `src/kern/pc98/linux-boot.c` |
| Linux paging-off jump | `src/kern/pc98/linux-entry.S` |
| generic image-load progress | `src/kern/loader.c` |
| BeUI display proxy and PC-98 BeUI construction | `src/noct/pc98-beui.c` |
| Noct keyboard and clock adapters | `src/noct/platform.c` using HAL APIs |
| local string/memory primitives | remove; use the freestanding libc |
| local console wrappers | remove; call HAL cons directly |
| M9 disk-write diagnostics | a test-only source under `tests/` |

`src/kern/pc98/linux-boot.c` is not Linux kernel code and not an IDE driver.
It is the PC-98 Linux boot-protocol adapter currently embedded in `stage2.c`.
`kern_platform_init()` registers it with the platform-independent image-loader
registry, after which the shell calls only the generic image boot interface.

## 9. Embedded Noct isolation

`src/noct/` contains only the temporary in-kernel adapter around the Noct
submodule.  Its exported kernel interface is declared in `include/kern/noct.h`.

The split is:

- `noct.c`: VM creation, execution, reset, and result handling;
- `memory.c`: arena and JIT memory profile selection;
- `napi.c`: the current native API registrations;
- `platform.c`: file, directory, console, clock, keyboard, REPL, and libc
  adapters using kernel/HAL interfaces only;
- `target.c`: Noct Term and target registrations without PC-98 port I/O;
- `pc98-beui.c`: construction of the current PC-98 BeUI HAL until BeUI also
  moves behind a process/device interface.

The following code is removed from Noct glue or moved elsewhere:

- direct PC-98 `inb`/`outb`: use HAL I/O or a supplied HAL callback;
- direct BIOS work-area memory reads: use the HAL memory map/platform boot
  context;
- high-memory enable port sequence: move to PC-98 HAL initialization;
- `boots_libc_panic()`: move to `src/kern/panic.c` and use HAL cons;
- the strong libc `boots_console_write_bytes()` hook: place in kernel glue and
  forward to `hal_cons_write_utf8()`;
- direct references to `drivers/kbd-pc98.h`: use HAL cons/input wrappers.

## 10. Build-system changes

`Makefile`, `noct.mk`, and `platform/pc98/platform.mk` are updated together.

- Add `-Iinclude`; remove `-Ihal/include` and obsolete `core/` include paths.
- Preserve source hierarchy in every object path under `build/pc98/`.
- Keep `.d` dependency generation for C and preprocessed assembly.
- Replace `HAL_PC98_SOURCES`, `HAL_PC98_ASM`, kernel object lists, and Noct glue
  object lists with the new paths.
- Update host-test source paths rather than weakening or deleting tests.
- Update `stage2.ld` low-segment selectors for `src/hal`, `src/kern`, drivers,
  and the PC-98 Linux entry object.
- Remove all references to `stage2-entry.o`, `kern/entry.o`, the old
  `platform/pc98/console.o`, old keyboard-driver objects, and old source paths.
- Update `scripts/generate-messages.py` and its generated-header target for the
  UTF-8 console contract.
- Keep the Noct submodule's own build layout unchanged.

## 11. Implementation checkpoints

The implementation may be applied as one reviewed worktree rewrite, but it is
validated internally at these checkpoints.  A checkpoint is not passed by
disabling a test or relaxing a binary-layout assertion.

### Checkpoint 0: baseline and rollback data

- Record branch, HEAD, submodule HEADs, `git status --short`, and QEMU HEAD.
- Save a binary-capable patch of any pre-existing user changes.
- Build the unmodified baseline and run the currently passing smoke tests.

### Checkpoint 1: mechanical include/source move

- Move headers, HAL sources, kernel/core sources, Noct glue, and rename the IDE
  driver.
- Update build paths without changing runtime behavior.
- Require `make ARCH=pc98 all`, HAL compile checks, kernel compile checks, and
  host tests to pass.

### Checkpoint 2: C `kernel_entry`

- Move the boot-stack switch into `locore.S`; retain the model but make the
  validated 64 KiB requirement explicit.
- Pass the handoff through `cmain(handoff)` to C `kernel_entry(handoff)`.
- Introduce `kern_platform_init()` and `kernel_main()`.
- Remove `kern/entry.S` and confirm the ELF entry still resolves to
  `locore.S:text_start`.
- Run a normal HDD boot before continuing.

### Checkpoint 3: HAL cons and keyboard merge

- Merge console.c behavior and native keyboard code into BSP cons.
- Convert generated startup messages and callers to UTF-8.
- Route HAL diagnostics, libc, startup, shell, and Noct output through one HAL
  state.
- Remove old console and keyboard sources only after host and QEMU checks pass.

### Checkpoint 4: `stage2.c` and Noct split

- Move each function group to the destination in sections 8 and 9.
- Remove `stage2.c` only when no function remains.
- Verify that generic kernel sources do not include the PC-98 ABI, use port
  I/O, or refer to PC-98 keyboard symbols.

### Checkpoint 5: final cleanup

- Remove unused `stage2-entry.S` and polling timer sources.
- Remove stale build rules, old include names, and obsolete comments/docs.
- Run the complete verification matrix below.

## 12. Verification matrix

### Static and host verification

```text
make ARCH=pc98 all
make ARCH=pc98 check
make ARCH=pc98 hal-pc98-compile kern-compile
git diff --check
readelf -h/-l build/pc98/stage2.elf
nm/objdump entry and i386-opcode audits
```

Additional host tests must cover:

- UTF-8 ASCII, half-width katakana, Japanese JIS X 0208, invalid UTF-8, and
  one-/two-cell cursor advancement;
- row clearing, scrolling, save/restore, and cursor visibility;
- PC-98 keyboard make/break, Shift/Ctrl/Graph, special keys, event ordering,
  non-consuming poll, drain, and key-state queries;
- the platform-independent keyboard wrapper calling only HAL interfaces.

### QEMU verification

Use the current `/home/awe/qemu-pc98/build/qemu-system-i386` and compatible
BIOS.  Treat POST longer than five seconds as failure.

Run at least:

- ordinary single-HDD boot and AUTOEXEC;
- two-IDE-drive regression test;
- startup menu and shell input;
- Japanese menu text, cursor movement, scrolling, and ESC return;
- Noct REPL and Noct file execution;
- Emacs startup, editing input, and filesystem access;
- BeUI GDC mode and return to text;
- BeUI Cirrus 8bpp and 24bpp and return to text;
- BeUI menu, keyboard-input, and Holoris tests;
- PC-98 Linux image load and execution.

For display tests, retain screenshots/logs in the build test directory for
review.  The test succeeds only if text output remains usable after BeUI closes
and HAL diagnostics do not use a second cursor state.

## 13. Completion criteria

The refactoring is complete only when all of the following are true.

- `kernel_entry()` is C and `kern/entry.S` no longer exists.
- `locore.S:text_start` remains the BOOT.SYS ELF entry.
- `kernel_main()` and generic shell/startup sources contain no PC-98 ABI or
  direct I/O.
- `platform/pc98/console.c`, `core/console.h`, and all old
  `boots_console_*` state APIs are gone.
- All 32-bit text output, including Noct and libc, reaches HAL cons.
- The HAL PC-98 cons has no dependency on the Noct submodule.
- PC-98 keyboard hardware and mapping live in BSP cons; any generic keyboard
  driver is a HAL-only adapter.
- PC-98 IDE is built from `drivers/pc98-ide.c`.
- `platform/pc98/stage2.c` and `stage2-entry.S` are gone.
- `src/noct/` can be identified and later removed as one isolated subsystem.
- The static, host, and QEMU verification matrix passes without weakening an
  existing assertion.

Only after this baseline is reviewed and accepted should the inode, all-
partition `/diskN`, initial-directory, shell `cd`, and `device` command work
begin.
