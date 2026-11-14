# zedBSD x64 UEFI loader

`BOOTX64.EFI` is a freestanding fallback-path application.  It searches the
FAT16/FAT32 filesystems on the physical disk from which firmware loaded it for
root `/zedbsd.cfg`; the loaded filesystem is considered first and remaining
handles retain firmware order.  No match is fatal.  Multiple matches produce
a warning and use the first match.  Filesystems on other disks are ignored.

The selected file uses the common bounded configuration grammar.  Its single
`kernel=` value is a safe relative path on that same FAT; remaining lines form
the kernel-parameter record.  The loader synthesizes a missing `boot0=` from
the selected FAT UUID and normalizes relative overlay/data/swap files.  It
does not consume UEFI LoadOptions and has no fixed-kernel or embedded-parameter
fallback.

Two tokens of the parameter lines are also the loader's.
`logo=PATH` names a binary PPM (P6, maximum value 255, at most 4096 pixels a
side, at most 16 MiB) on the same FAT; the loader fills the screen with the
colour of its top-left pixel, draws it in the middle, and from then on writes
its progress text only to the debug port (a failure is still shown).
`kmsg=quiet` makes the loader draw no progress blocks and enter the kernel
through the transition's quiet entry, which draws none either, so the logo
stays until the kernel takes the screen.  Both stay in the parameter record;
the kernel ignores `logo=` and reads `kmsg=`.  The images put
`tools/build/make-boot-logo.py`'s logo on the ESP as `/logo.ppm`.

Two keys change one boot without editing the file.  The loader reads
the firmware's extended console input (`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`)
when it starts, asking for exposed modifier keys, and again right after it
parses the configuration: each time it drains the key queue and takes the
modifier keys held at that moment from the key state the input reports with
an empty queue.  It never waits for a key.  Ctrl held removes every
`kmsg=` and `logo=` token from the record and appends `kmsg=console`, and the
loader wishes for a 640x480 GOP mode when `video=` names none, so the early
console's 80 columns fill the screen.  Shift held removes every `login=` token
and appends `login=console`.  Tokens are matched by whole name, removed before
appending (the kernel refuses a repeated known name), and an appended token
that would pass 3071 bytes is left out.  The rewrite
(`bootloader/common/boot-override.c`) runs before the logo and the video mode
are chosen.  The loader then prints `Boot: kernel messages (Ctrl)`,
`Boot: console login (Shift)` and `A64 PARAMS OVERRIDE <record>`; on screen
they are best effort.  The way to press the keys is to hold the modifier and
tap Space repeatedly after power-on: a modifier with Space is reported by
every firmware with the extended input, a modifier alone only by firmware
that reports the held state or honours exposed modifiers.  See
`docs/reference/kernel-boot-parameters.md` Section 7c.

The GOP mode is chosen once, after the boot keys, in this order.  A
`video=WIDTHxHEIGHT` token names a mode that must exist (a missing one stops
the boot).  Without it, a boot with Ctrl held wishes for 640x480.  Any other
boot asks for the monitor's size: the loader reads the monitor's EDID from
`EFI_EDID_ACTIVE_PROTOCOL`, or `EFI_EDID_DISCOVERED_PROTOCOL` when there is no
active one, on the GOP's handle (or the only EDID in the system, when the GOP
is the console's and carries none), and takes the active size of its preferred
timing (the first detailed timing descriptor of an EDID 1.x base block with a
valid checksum; an interlaced one is refused).  The mode of that size is set
when the firmware has it; otherwise the largest mode by area that fits inside
it, one of the monitor's aspect ratio winning a tie, so nothing is cut off.
When there is no EDID or no mode fits, a boot with a logo wishes for
1920x1080 (1080x1920 on a portrait screen), and otherwise the firmware's mode
stays.  Only modes in a 32-bit RGB or BGR linear layout are taken.

After validating and loading the configured restricted ELF64 kernel, the
loader captures a ZBL6 v2 memory map, exits boot services, installs private
four-level bootstrap page tables, and enters the kernel using the System V
AMD64 ABI.  Firmware disk services are not used after `ExitBootServices()`;
the kernel resolves the selected FAT and configured root through its native
storage drivers.

# The riscv64 UEFI loader

`BOOTRISCV64.EFI` (`bootriscv64.c`, `riscv64/`) is the same kind of
fallback-path application for 64-bit RISC-V firmware (U-Boot's UEFI, edk2).
It reads `/zedbsd.cfg` from the volume it was loaded from only, with the same
grammar and the same synthesized `boot0=`, then the configured kernel and,
when the volume has one, `/root.img`, which the kernel takes as its initial
ramdisk.  It asks the firmware for the boot hart through
`RISCV_EFI_BOOT_PROTOCOL` (a firmware without it is refused), copies the
flattened device tree from the configuration table, fills the riscv64
handoff (`bootloader/include/riscv64-handoff.h`), exits boot services and
enters the kernel at its physical entry with translation off.  It builds no
page tables: the kernel builds its own.

LLVM writes no PE/COFF object for RISC-V, so the loader is linked as a
position-independent ELF for address 0 (`riscv64/loader.ld`) that carries
only relative relocations.  `platform/riscv64/tools/make-bootriscv64.py`
puts a PE32+ header with two sections in front of its flat image, and the
entry (`riscv64/start.S`) applies the relocations for the address the
firmware loaded the image at.  `platform/riscv64/tools/check-bootriscv64.py`
checks the result.  The kernel placement contract is
`bootloader/include/riscv64-kernel-image.h`; `elf64.c` serves both machines.
