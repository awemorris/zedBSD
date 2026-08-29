# WS003 shared test cases

Parent: [WS003](../ws.md)

| Case ID | Environment | Required observation |
| --- | --- | --- |
| BR-T00 | Physical inventory | Exact BIOS/UEFI/Secure Boot state and PCI/USB IDs are captured without modifying devices |
| BR-T10 | Host image | USB image generation is reproducible and partitions/files verify before writing media |
| BR-T20 | QEMU UEFI/BIOS | Firmware discovers USB and transfers control to the intended loader/kernel |
| BR-T21 | QEMU xHCI | Kernel enumerates xHCI and USB mass storage, selects the intended root despite added disks |
| BR-T22 | QEMU root I/O | Sustained read/write, sync, reboot, missing-root, and timeout behavior pass on disposable images |
| BR-T23 | UEFI memory-map normalization | Valid sorted/unsorted maps canonicalize; malformed size, overflow, overlap, and capacity cases fail distinctly; OVMF still boots |
| BR-T24 | QEMU high-RSDP USB boot | The same production image boots OVMF/q35/xHCI at 4, 8, and 16 GiB; RSDP is above 1 GiB and ACPI/IRQ, four CPUs, USB root, and `login:` pass without fatal/storage errors |
| BR-T25 | Host xHCI capability/MMIO fixture | Valid 1.0/1.1/1.2 capability layouts pass; zero/all-one and malformed extents fail distinctly; all-ones runtime MMIO is invalid; Memory Space enable precedes MMIO; 32/64-bit BAR assignment is transactional and fail-closed |
| BR-T26 | Host USB HCD teardown fixture | Failed checked quiesce retains the registered bus and blocks new URBs; a later successful quiesce/stop removes the same bus |
| BR-T27 | Host xHCI control/EP0/reset fixture | Setup, Data, and Status TRBs have exact xHCI 1.2 reserved-zero and TD boundaries; EP0 packet size/Average TRB Length and bounded port-reset outcomes are correct |
| BR-T28 | Host xHCI cancellation fixture | Running, Halted, Error, and Stopped endpoint recovery retains request/DMA ownership until Stop/Reset/Set-Dequeue or controller quiesce succeeds |
| BR-T29 | xHCI command/hotplug lifecycle | Command Completion events match the submitted TRB across IRQ/poll interleavings; QEMU auxiliary USB remove/re-add completes without stale commands, live DMA, or leaked slots |
| BR-T30 | Final Latitude repeatability | One frozen integrated image reaches init/login/shell from USB on five consecutive cold boots; a failure breaks the sequence and is analyzed before retry |
| BR-T31 | Latitude root I/O | USB-root filesystem smoke and sustained I/O complete without reset/corruption |
| BR-T32 | Latitude UEFI entry | Three cold boots pass memory-map normalization and `ExitBootServices`, then show an unambiguous kernel-entry marker |
| BR-T33 | Latitude xHCI boundary confirmation | One boot identifies both xHCI functions and the boot-media controller attaches and enumerates USB mass storage without `capabilities (13)`, `boot-storage wait expired`, or `physical disks=0`; final repeatability belongs to BR-T30 |
| BR-T34 | Latitude xHCI device enumeration | One boot of the frozen q013 p004--p009 image reaches `usb-storage: sda` without an EP0/command/recovery failure or boot-storage timeout; later U3 errors are recorded separately |
| BR-T35 | Host xHCI halted-endpoint recovery | Running submits directly; Halted uses Reset Endpoint then Set TR Dequeue; Stopped/Error use Set TR Dequeue; disabled/unknown states reject the TD |
| BR-T36 | Host shared-DMA lifecycle | Concurrent IRQ/SMP-style allocation, lookup/map, unlink/free, and destruction cannot corrupt or traverse a freed shared allocation-list node |
| BR-T37 | xHCI multi-device association lifetime | URB/endpoint operations resolve only their USB lifecycle-owned xHCI object; hotplug never traverses an unrelated freed controller-private device node |
| BR-T38 | xHCI SuperSpeed context | Slot Context preserves raw PORTSC Speed ID and SuperSpeed bulk Endpoint Context preserves companion `bMaxBurst` |
| BR-T39 | Host USB-storage flush capability | Bounded MODE/SENSE parsing plus BOT status/residue accounting select immutable sync-cache, write-through, FUA-from-first-write, or read-only policy before publication; unsafe state and runtime sync loss fail closed with the original error |
| BR-T40 | CDC selection | Selected ACM or ECM/NCM profile interoperates and reconnects; device-role capability is proven first |
| BR-T41 | Latitude USB-storage U3 confirmation | One boot of the frozen q014 image resolves `/dev/sda1`, mounts the read-write data loop and root overlay, and starts init without opcode-35 `05/20/00` or a false-success cache policy |
| BR-T42 | Common boot-parameter parser | The production parser enforces the 3071-byte grammar, known-key uniqueness, indexed boot/swap names, unknown-key policy, and architecture-independent absolute `init=` selection |
| BR-T43 | Four-path x86 parameter handoff | i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI validate and copy the same bounded NUL-terminated parameter string without retaining loader memory; opaque UEFI binary options fall back to image defaults |
| BR-T44 | Boot-slot and root-mode selection | Four sparse FAT boot slots, selector ambiguity/aliasing, safe `bootN:PATH`, native `rootpart`, explicit lower/upper overlay, and complete failure unwind follow the public contract |
| BR-T45 | Multi-source swap | Zero to four sparse FAT-file or signed raw-partition sources activate atomically, allocate in numeric order, route I/O to the owning source, aggregate stats, and shut down without leaks |
| BR-T46 | Four-platform parameter acceptance | Every declared i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI runtime cell proves normal/default init, explicit shell init, native root, overlay root, file swap, raw swap, and visible invalid-configuration failure |
| BR-T47 | Static image parameters and scripting closure | One maintained source default feeds all four x86 loader paths and the kernel fallback; no generated input/header, Python generator, build-time file selector, stale state, or Python invocation remains, while affected non-default parameter/swap fixtures remain usable |
| BR-T48 | UEFI whole-load-option compatibility | A test wrapper places one valid complete `EFI_LOAD_OPTION` descriptor with empty `OptionalData` in the real `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` fields, then the production loader boots through OVMF/q35/xHCI USB to `login:` without a LoadOptions fatal |
| BR-T49 | Latitude NVMe installation | The ordinary USB system explicitly initializes the internal SN740 as GPT/ESP, installs and verifies zedBSD, and the frozen result boots both native UFS root and NVMe-backed overlay root through installed `BOOTX64.EFI` |
| BR-T50 | Physical network | Static or DHCP setup, peer reachability, and a bounded data transfer pass |

For the q011 diagnostic BR-T32 image, the top-right GOP marker is unary: one
large white block means boot services exited, two means the final map passed,
three means the bootstrap CR3 executed, and four means the kernel entry
instruction executed. A successful console initialization erases the panel.
With one large block, 1--7 small blocks underneath encode the final map error
enum. A final-map or `ExitBootServices()` error returned while boot services
remain live is printed as text instead.

The sparse ACPI physical-window allocator is exercised independently of
privileged page-table operations, including the Latitude RSDP address, mapping
reuse, a cross-page table, arithmetic overflow, exact exhaustion, and capacity
failure:

```sh
cc -std=c11 -I. -Iinclude -Wall -Wextra -Werror \
  src/hal/amd64/acpi-window.c \
  plan/ws003-bringup/tests/acpi-window-test.c \
  -o /tmp/ws003-acpi-window-test
/tmp/ws003-acpi-window-test
```

Each extracted Phase converts the applicable row into an executable script or
a hardware runbook and records its artifact path here.

BR-T23 is a focused host fixture:

```sh
cc -std=c11 -I. -Wall -Wextra -Werror \
  bootloader/uefi/memory-map.c \
  plan/ws003-bringup/tests/uefi-memory-map-test.c \
  -o /tmp/zedbsd-uefi-memory-map-test
/tmp/zedbsd-uefi-memory-map-test
```

BR-T25 directly shares the production xHCI capability arithmetic and PCI
enable-state implementation. It covers valid xHCI 1.0--1.2 snapshots,
reason-coded malformed layouts, scratchpad and relative xECP arithmetic,
32/64-bit BAR readback and transactional assignment, command-state rollback,
all-ones runtime-MMIO rejection, and fail-closed partial-write/readback-error
paths:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  src/drivers/pci.c \
  plan/ws003-bringup/tests/xhci-capability-mmio-test.c \
  -o /tmp/ws003-xhci-capability-mmio-test
/tmp/ws003-xhci-capability-mmio-test
```

The p003 teardown regression links the production USB core and verifies that a
failed checked HCD quiesce retains its registered bus, blocks new URBs, and can
be retried safely, while a later successful quiesce/stop removes that same bus:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  src/drivers/usb.c \
  plan/ws003-bringup/tests/usb-hcd-unregister-test.c \
  -o /tmp/ws003-usb-hcd-unregister-test
/tmp/ws003-usb-hcd-unregister-test
```

BR-T27, BR-T28/BR-T35, BR-T36, and BR-T38 share the production helper
contracts used by q013.  BR-T37's direct xHCI-device association lifetime is
covered by the extended HCD unregister fixture above and BR-T29 below.

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  plan/ws003-bringup/tests/xhci-control-ep0-reset-test.c \
  -o /tmp/ws003-xhci-control-ep0-reset-test
/tmp/ws003-xhci-control-ep0-reset-test

cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  plan/ws003-bringup/tests/xhci-cancel-command-test.c \
  -o /tmp/ws003-xhci-cancel-command-test
/tmp/ws003-xhci-cancel-command-test

cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror -pthread \
  src/drivers/dma.c plan/ws003-bringup/tests/dma-allocation-lock-test.c \
  -o /tmp/ws003-dma-allocation-lock-test
/tmp/ws003-dma-allocation-lock-test
```

BR-T29 boots from IDE while an auxiliary xHCI mass-storage device is removed
and re-added.  It proves that command, slot, request, device-association, and
DMA ownership survive the hotplug boundary without disrupting the live root:

```sh
plan/ws003-bringup/tests/xhci-hotplug-lifecycle.sh \
  build/amd64/hdd-image.img build/data.img /tmp/ws003-br-t29
```

The q013 legacy-BIOS USB-only gate always copies the supplied production image
before booting it.  It requires xHCI attach, device configuration,
`usb-storage: sda`, `/dev/sda1` root resolution, and `login:` and rejects the
bounded xHCI/storage failure signatures:

```sh
plan/ws003-bringup/tests/legacy-xhci-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-legacy-xhci
```

q014 also runs that disposable-image gate with QEMU's cache disabled. This
requires the guest marker `cache=disabled ... flush=write-through`, enables
QEMU SCSI command logging, and rejects any redundant SYNCHRONIZE CACHE command:

```sh
USB_STORAGE_WRITE_CACHE=off USB_STORAGE_COMMANDLOG=on \
  plan/ws003-bringup/tests/legacy-xhci-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-q014-write-through
```

BR-T24 is the production OVMF USB matrix. It defaults to the required 4, 8,
and 16-GiB cases and preserves per-capacity logs under the supplied output
directory.  Reusing an output directory explicitly truncates each run's guest
and QEMU logs before launch so stale markers cannot produce a false result:

```sh
plan/ws003-bringup/tests/uefi-high-memory-usb-boot.sh \
  build/amd64/hdd-image.img /tmp/ws003-br-t24
```

BR-T39 is the q014 USB-storage durability-policy fixture. It shares the
production BOT, MODE SENSE, sense-classification, and policy helpers and covers
valid and malformed response lengths, CSW status/residue accounting,
WP/WCE/DPOFUA combinations, exact current-error `05/20/00` classification,
pre-publication policy selection, FUA on the first write, read-only unsafe
media, and sticky runtime sync failure with the original error:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  plan/ws003-bringup/tests/usb-storage-flush-test.c \
  -o /tmp/ws003-usb-storage-flush-test
/tmp/ws003-usb-storage-flush-test
```

BR-T42 links the production common parser directly. It covers the complete
known-name set, sparse boot/swap indices, owned storage, bounded unknown-name
diagnostics, duplicate and malformed input, ASCII/NUL transport boundaries,
the exact 3071-byte limit, and absolute `init=` selection:

```sh
cc -std=c11 -Iinclude -Iinclude/uapi -I. -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections \
  src/kern/boot.c src/kern/init.c \
  plan/ws003-bringup/tests/boot-parameters-test.c \
  -Wl,--gc-sections -o /tmp/ws003-boot-parameters-test
/tmp/ws003-boot-parameters-test
```

BR-T43 links the production shared record, four x86 layout classifiers, and
UEFI `LoadOptions` conversion helpers directly. It also verifies optional-NUL
length-delimited text, unaligned input, Dell-style complete
`EFI_LOAD_OPTION` descriptor extraction, and that unrecognized, odd-sized, or
embedded-NUL binary options cannot prevent boot and select the image default
instead:

```sh
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  src/hal/x86/boot-parameters.c \
  src/hal/amd64/bsp-pcat/handoff-validation.c \
  bootloader/uefi/load-options.c \
  plan/ws003-bringup/tests/x86-parameter-handoff-test.c \
  -o /tmp/ws003-x86-parameter-handoff-test
/tmp/ws003-x86-parameter-handoff-test
```

BR-T44 links the production selector, boot-reference, root-mode, and private
slot ownership code. It covers strict selectors, bounded normalized paths,
sparse slots, loader-origin fallback, duplicate aliases, FAT12 rejection,
undefined references, same-FAT root promotion, and reverse rollback after
every slot acquisition stage:

```sh
cc -std=c11 -Iinclude -Iinclude/uapi -I. -Wall -Wextra -Werror \
  src/kern/boot.c plan/ws003-bringup/tests/boot-source-test.c \
  -o /tmp/ws003-boot-source-test
/tmp/ws003-boot-source-test
```

BR-T45 links the production swap header, VM backend, and aggregate source
mapping. It covers legacy `ZEDSWAP1`, 64-bit `ZEDSWAP2` capacity/UUID/label,
malformed and length-mismatched headers, sparse numeric source order,
duplicate/overflow rejection, boundary routing, concurrent allocation/free,
first-error flush, and complete shutdown. Generate both on-disk formats before
running the fixture:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  tools/build/make-swapfile.noct --format v1 --size-mib 32 \
  --output /tmp/ws003-swap-v1
build/NoctLang/build-static/noct --path=tools/build \
  tools/build/make-swapfile.noct --format v2 --size-mib 1 \
  --uuid 0123456789ABCDEF --label TESTSWAP \
  --output /tmp/ws003-swap-v2
cc -std=c11 -Iinclude -I. -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -pthread \
  src/kern/swap.c src/kern/swap-source.c \
  plan/ws003-bringup/tests/swap-source-test.c \
  -Wl,--gc-sections -o /tmp/ws003-swap-source-test
/tmp/ws003-swap-source-test /tmp/ws003-swap-v1 /tmp/ws003-swap-v2
```

BR-T46 is the production-loader QEMU acceptance harness.  Its complete matrix
contains 31 cells: six common behaviors on each of i386 PC/AT, i386 PC-98,
amd64 BIOS, and amd64 UEFI (24 cells), plus mixed file/raw swap, UUID disk
reordering, and PARTUUID disk reordering on both amd64 firmware paths (six
cells), plus one PC/AT native-root/raw-swap alias rejection cell.  `--list` and
`--dry-run` expose the selected cells without building or booting them:

```sh
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh --list
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh --dry-run \
  --platform amd64-uefi --case partuuid-reorder
```

A full run requires a new or empty persistent output directory.  It saves the
generated configurations, build logs, exact QEMU commands and versions,
parameter/image hashes, guest logs, controller results, and result table there.
It hashes the user's `config.mk` before and after the run, uses the normal
independent `build/pcat`, `build/pc98`, and `build/amd64` directories with
`make -j16`, and builds each group's production loaders once. Each non-default
cell patches only disposable BPR1-bearing loader copies, rebuilds and validates
the corresponding BIOS/MZ or EFI wrapper, and verifies that every production
loader hash remains unchanged. QEMU receives only per-cell disposable image
copies:

```sh
plan/ws003-bringup/tests/boot-parameter-qemu-acceptance.sh \
  plan/ws003-bringup/temp/q015-br-t46
```

Swap cells add the test-only production-ABI `/bin/brt46-swap` helper to a
private acceptance root image.  A cell passes only after at least 1024 pages
(4 MiB) have actually paged out, all touched anonymous pages have been read
back with their contents intact, and the VM counters report a positive
page-in.  PC-98 observations use QEMU monitor text-VRAM snapshots rather than
OCR.  The amd64 UUID and PARTUUID regressions enumerate a distinct auxiliary
MBR/FAT disk before the production boot disk while `bootindex` still selects
the production image; this independently verifies loader-origin `boot0` and
explicit `boot1` resolution.  The PC/AT alias regression stamps a valid
`ZEDSWAP2` header into the reserved boot block of a disposable native UFS root
partition, selects that same partition as both `rootpart` and `swap0`, and
requires the pre-mount/pre-activation `EEXIST` diagnostic.

The PC-98 extended-image fixture also makes its BOOT and added UFS/SWAP
partitions disjoint. The ordinary single-partition PC-98 image describes the
BOOT entry through the medium end while its BPB bounds the FAT filesystem; the
fixture shortens that entry to the actual 128-MiB FAT boundary before adding a
second partition. This preserves the q020 writable-backing overlap safety
check instead of exempting a historical test-only overlap.

The q015 USB-backed swap forward-progress correction reserves one embedded
xHCI transfer request plus an 8-KiB-aligned, 8-KiB coherent bounce buffer per
controller before it runs. The existing controller-wide single-flight rule
lets page-sized storage I/O and its BOT envelope use that reserve without a
heap or DMA allocation; generic transfers above 8 KiB retain the dynamic path.
USB storage similarly allocates persistent control, bulk-in, and bulk-out URBs
at attach. `drv_usb_urb_wait_reusable()` prevents reuse until terminal status
and the HCD ownership hold are both clear. xHCI's atomic `completion_busy`
counter prevents detach/stop quiescence from seeing a false zero across
overlapping or reentrant completion publication, while failed cancellation
retains request, URB, and DMA ownership.

Focused post-correction BR-T46 evidence is:

| Cell | Evidence directory | Final page-in / page-out |
| --- | --- | --- |
| amd64 UEFI file swap, repeat 002 | `plan/ws003-bringup/temp/q015-br-t46-reserve-file-focused-002-uefi` | 1767 / 3533 |
| amd64 UEFI file swap, repeat 003 | `plan/ws003-bringup/temp/q015-br-t46-reserve-file-focused-003-uefi` | 1767 / 3533 |
| amd64 BIOS raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-raw-focused-001-bios` | 2755 / 5509 |
| amd64 UEFI raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-raw-focused-001-uefi` | 1748 / 3495 |
| amd64 BIOS mixed file/raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-mixed-focused-001-bios` | 2799 / 5597 |
| amd64 UEFI mixed file/raw swap | `plan/ws003-bringup/temp/q015-br-t46-reserve-mixed-focused-001-uefi` | 1792 / 3583 |

Every listed `results.tsv` records `pass`; each cell's logical guest log ends
with a successful full anonymous-page readback and the nonzero counters shown
above. The eight existing affected host regressions also pass:
`xhci-capability-mmio-test`, `usb-hcd-unregister-test`,
`xhci-control-ep0-reset-test`, `xhci-cancel-command-test`,
`dma-allocation-lock-test`, `xhci-model-test`, `usb-storage-scsi-test`, and
`usb-urb-publication-test`. BR-T39 `usb-storage-flush-test` also passes against
the corrected USB-storage path.

The authoritative BR-T46 result is **PASS 31/31** at
`plan/ws003-bringup/temp/q015-br-t46-final-007`: PC/AT 7/7, PC-98 6/6, amd64
BIOS 9/9, and amd64 UEFI 9/9. It ran from 2026-08-27 11:04:12Z through
11:20:24Z with system QEMU 10.0.11 and PC-98 QEMU 11.0.93. All 10 positive
swap cells report nonzero page-in/page-out, full anonymous-page readback, and
`OBJECT-SHARED PASS`; the fatal BOT/storage scan found zero matches. The
native-root/raw-swap alias rejection and both UUID and PARTUUID reordered-disk
cases pass on their declared production paths.

The run preserved `config.mk` SHA-256
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
Its authoritative SHA-256 values are
`d290ceb43b1f4b3c076c53b3b41d571dbfdf61b4526bd7821e03da5355efeb3c`
for `cells.tsv`,
`d64dfbcc76d3c0f8e86f27ba86a7280391ca7a4e86fe0666c404c5d1f8a7d8cf`
for `results.tsv`, and
`e38233677fbebf399d89cd2688c9a98d65f6ec2bea1b81baedcb3f26a4fffd1b`
for `metadata.txt`.

q023 reran the complete matrix after replacing the generated parameter header
with the static BR-T47 source contract. The newest authoritative result is
**PASS 31/31** at
`plan/ws003-bringup/temp/q023-p016-br-t46-authoritative.9rIQrm`: PC/AT 7/7,
PC-98 6/6, amd64 BIOS 9/9, and amd64 UEFI 9/9. The default cell for each path
boots the normal production `hdd-image.img`; only non-default cases patch a
validated BPR1 record in a disposable loader copy. Production-loader hashes
and `config.mk` stayed unchanged. The run's `cells.tsv`, `results.tsv`, and
`metadata.txt` SHA-256 values are
`d290ceb43b1f4b3c076c53b3b41d571dbfdf61b4526bd7821e03da5355efeb3c`,
`afa1c1cb043f042a9efc6188a699d1fcfd9267964adc70cc8c82a876fa932e90`,
and `d522c4340d1fb89544753ac2d70a1352d8dfa294456dbff4af7b94aa5ffd5d11`.

The corresponding no-Python source/build audit passed for amd64, PC/AT, and
PC-98 at `plan/ws003-bringup/temp/q023-p016-audit-final.8oHUQf`. The affected
WS016 runtime-swap runner also passed file, mixed, and native cells 3/3 at
`plan/ws016-swap-control/temp/q023-p016-runtime-swap.UGNqkG`.

BR-T47 guards the maintained-source boot-parameter contract. Run the project
toolchain first, then use the two Phase-owned Noct helpers:

```sh
make -j16 toolchain
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws003-bringup/tests/patch-boot-parameter-record.noct --self-test
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws003-bringup/tests/boot-parameter-source-audit.noct \
  "$PWD" plan/ws003-bringup/temp/br-t47-source-audit
```

`boot-parameter-source-audit.noct` accepts optional
`[REPOSITORY [OUTPUT]]` arguments. It verifies the sole maintained source
default, derived C/assembly lengths, absence of the removed generator, selector,
and stale generated output, and forced amd64, PC/AT, and PC-98 disk-image
dependency traces with no Python invocation.

`patch-boot-parameter-record.noct` accepts either `--self-test` or
`--text TEXT INPUT OUTPUT`. It is only for distinct disposable artifacts. It
requires exactly one complete fixed-size BPR1 record, rejects zero, duplicate,
malformed, oversized, non-ASCII, unterminated, and unsupported records, and
atomically writes the patched output without changing its input.

BR-T48 builds a test-only EFI entry wrapper around the production UEFI loader.
The wrapper obtains the real loaded-image protocol, installs a complete packed
`EFI_LOAD_OPTION` whose `OptionalData` length is zero, and then calls the
production `efi_main` (renamed only in this test build). The bounded one-boot
OVMF run uses a disposable copy of the supplied image and requires the
injection marker, loader exit, kernel entry, and `login:` while rejecting the
old `missing-nul`/LoadOptions fatal path:

```sh
plan/ws003-bringup/tests/uefi-load-option-qemu.sh \
  build/amd64/hdd-image.img /tmp/ws003-br-t48
```

This run postdates the final xHCI stop/IRQ ownership review. In addition to
the eight existing affected regressions, `usb-hcd-unregister-test` now proves
that a timeout cannot return a reusable synchronous URB while the HCD still
owns it, and `xhci-capability-mmio-test` proves that checked PCI IRQ removal
retains its cookie across a first-attempt `EBUSY` and succeeds on retry. The
complete final host set passes 13/13.

BR-T41 was performed once after q014's host/build/QEMU gates passed and its
production image was frozen with a recorded path, size, and SHA-256. The
2026-08-27 Latitude result passes U3 and additionally reaches root login/shell
and X/`zterm`; it is recorded in
[latitude-xhci-evidence.md](latitude-xhci-evidence.md). BR-T30 retains the later
five-consecutive-boot acceptance, and BR-T31 retains sustained root I/O.

Current physical/QEMU evidence is recorded in
[latitude-uefi-evidence.md](latitude-uefi-evidence.md).

The downstream physical xHCI boundary and required diagnostic facts are in
[latitude-xhci-evidence.md](latitude-xhci-evidence.md).
