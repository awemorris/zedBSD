# Queue: common x86 boot parameters and four-platform acceptance

Last updated: 2026-08-27

QID: `q015`

Queue status: finished

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-27

Timebox: no fixed duration; execute through the complete automatic QEMU matrix
unless a Phase reconsideration boundary is reached

Parent: [master plan](master.md)

Previous Queue: [q014](queue-q014.md)

## Purpose

Implement the complete documented x86 kernel-parameter contract and prove it
on four production paths without physical-machine work. Execute
`ws003-p011`--`ws003-p015` in dependency order: introduce the common parser and
architecture-independent `init=`, transport one bounded string through every
x86 loader/HAL, implement boot slots and explicit native/overlay roots,
activate up to four file or raw-partition swap sources, then run the complete
i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI QEMU acceptance matrix.

The normative public contract is
[kernel boot parameters](../docs/reference/kernel-boot-parameters.md). The
Queue may repair defects inside these five Phases until the matrix passes. It
must not invent a different parameter grammar or expand into CPAR menus,
physical-hardware acceptance, or unrelated platform repair.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p011` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase011-boot-parameter-core/phase.md), [contract](../docs/reference/kernel-boot-parameters.md) | completed | BR-T42 passes and one architecture-independent parser provides the complete name set plus exact `init=` selection/failure semantics |
| 2 | `ws003-p012` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase012-x86-parameter-handoff/phase.md), [tests](ws003-bringup/tests/README.md) | completed | BR-T43, all four production build/link paths, and amd64 BIOS/UEFI marker boots pass; PC/AT and PC-98 production runtime remains in p015 |
| 3 | `ws003-p013` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase013-root-source-selection/phase.md), [contract](../docs/reference/kernel-boot-parameters.md) | completed | BR-T44, build, and representative BIOS/UEFI overlay boots pass; the native-root and full four-platform runtime matrix remains in p015 |
| 4 | `ws003-p014` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase014-multi-swap/phase.md), [tests](ws003-bringup/tests/README.md) | completed | BR-T45 passes; sparse `swap0`--`swap3` file/raw sources activate atomically as one ordered pool and retain existing `ZEDSWAP1` compatibility |
| 5 | `ws003-p015` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase015-x86-parameter-acceptance/phase.md), [tests](ws003-bringup/tests/README.md) | completed | BR-T46 passes every required production-loader cell on i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI and promotes the public contract from planned to implemented status |

## Entry evidence and dependency readiness

- q014 is finished, so no active Queue or physical-run request competes with
  this cycle.
- The public parameter names, grammar, defaults, error rules, root-mode
  exclusion, swap ordering, and `init=` semantics are already fixed. No human
  product decision remains inside p011--p015.
- The current implementation still exposes the exact intended boundaries:
  i386 PC/AT retains a borrowed Multiboot command-line pointer; amd64 ZBL6 and
  UEFI do not transport the full string; VFS still parses old `boot=`/`root=`
  and fixed root/data image names; and FAT swap still assumes `/swapfile`.
- The required emulators are available: system `qemu-system-i386` and
  `qemu-system-x86_64`, plus the ignored PC-98-capable binary at
  `build/qemu-pc98/build/qemu-system-i386`.
- p011 is independent. p012 depends on p011; p013 depends on p011/p012; p014
  depends on p011--p013; and integrated p015 depends on all four. This Queue
  therefore has one unambiguous serial implementation order.

## Ordered execution

1. Start p011 and add BR-T42 beside the WS003 shared tests. Implement one
   bounded parser, the complete indexed name set, exact invalid/unknown-key
   policy, and architecture-independent PID 1 path selection. Retain default
   `/sbin/init`; remove implicit fallback after an explicit init failure.
2. Complete p012 with BR-T43 production-shared handoff fixtures. Copy and
   validate parameter text for i386 Multiboot, PC-98, amd64 BIOS ZBL6, and
   amd64 UEFI `LoadOptions`; preserve supported old handoffs and synthesize the
   complete current-layout default only for absent/empty input.
3. Complete p013 with BR-T44. Resolve four private FAT boot slots, normalize
   bounded `bootN:PATH`, enforce selector ambiguity/alias rules, and implement
   atomic `rootpart` versus explicit overlay-root/data selection without fixed
   filenames.
4. Complete p014 with BR-T45. Generalize the FAT swap source, add signed raw
   block sources and the versioned 64-bit-capacity header, aggregate up to four
   sparse sources in numeric order, and prove activation/unwind/concurrency,
   flush, and shutdown behavior.
5. During each Phase run its focused fixtures, affected storage/VM/init
   regressions, `make -j16`, and `git diff --check`. Do not defer an early
   focused failure to the final matrix.
6. Complete p015 by adding a reusable BR-T46 harness under
   `plan/ws003-bringup/tests/`. It must preserve the user's `config.mk`, build
   each production platform into its normal separate build directory, create
   deterministic native-root/raw-swap layouts, and boot only disposable image
   copies.
7. Run BR-T46 on these four environments and record exact QEMU commands,
   versions, artifacts, hashes, logs, and observed guest markers:
   - i386 PC/AT: system `qemu-system-i386`, production PC/AT loader;
   - i386 PC-98: `build/qemu-pc98/build/qemu-system-i386`, `pc9821` machine,
     production PC-98 loader;
   - amd64 BIOS: system `qemu-system-x86_64`, production ZBL6 BIOS loader; and
   - amd64 UEFI: system `qemu-system-x86_64` with OVMF, production
     `BOOTX64.EFI` path.
8. For every environment observe, rather than infer, the generated overlay
   default and normal init/login, explicit `init=/bin/sh`, native `rootpart`,
   FAT-file swap, signed raw-partition swap, and visible rejection of an
   invalid root/overlay combination. Also run the p015 mixed swap and
   cross-boot-filesystem cases on both amd64 firmware paths.
9. Rerun the complete focused suite, all four production builds, BR-T46,
   relative-document link validation, and `git diff --check`. Promote the
   reference document only after runtime evidence matches it.
10. Synchronize actual status and evidence into p011--p015, WS003, WS013,
    BR-T42--BR-T46, and the master. Finish q015 after every item is either
    `completed` or honestly `uncleared`.

## Four-platform acceptance boundary

The required integrated matrix is 31 production-loader cells: six common
behaviors across four environments (24), three supplemental behaviors on both
amd64 firmware paths (six), and one PC/AT-only native-root/raw-swap alias
rejection. Parser-only or handoff-only fixtures do not replace any runtime
cell.

| Runtime behavior | PC/AT i386 | PC-98 i386 | amd64 BIOS | amd64 UEFI |
| --- | --- | --- | --- | --- |
| Generated overlay default reaches normal init/login | required | required | required | required |
| `init=/bin/sh` reaches an interactive shell | required | required | required | required |
| `rootpart=` reaches native non-overlay root | required | required | required | required |
| `swap0=boot0:swapfile` reports and exercises swap | required | required | required | required |
| Signed raw swap partition reports and exercises swap | required | required | required | required |
| Invalid root/overlay combination fails visibly | required | required | required | required |
| Mixed file/raw swap reports and exercises both sources | - | - | required | required |
| Auxiliary `boot1=UUID=...` survives disk reordering | - | - | required | required |
| Auxiliary `boot1=PARTUUID=...` survives disk reordering | - | - | required | required |
| Native-root/raw-swap partition alias is rejected | required | - | - | - |

## Execution record

q015 completed on 2026-08-27 with every Queue item completed. The authoritative
BR-T46 run is `plan/ws003-bringup/temp/q015-br-t46-final-007`: it ran from
2026-08-27T11:04:12Z through 11:20:24Z and passed all 31 cells (PC/AT 7,
PC-98 6, amd64 BIOS 9, amd64 UEFI 9). System QEMU 10.0.11 was used for PC/AT
and amd64, and the PC-98-capable QEMU 11.0.93 was used for PC-98. The OVMF
CODE/VARS SHA-256 values are respectively
`624e06de18b4fa535e90db7160d00d3d07d206422b89999bf1e27d920264e4e0`
and `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e`.

The saved `config.mk` hash was unchanged before and after the run:
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
The evidence SHA-256 values are
`d290ceb43b1f4b3c076c53b3b41d571dbfdf61b4526bd7821e03da5355efeb3c`
for `cells.tsv`,
`d64dfbcc76d3c0f8e86f27ba86a7280391ca7a4e86fe0666c404c5d1f8a7d8cf`
for `results.tsv`, and
`e38233677fbebf399d89cd2688c9a98d65f6ec2bea1b81baedcb3f26a4fffd1b`
for `metadata.txt`.

All ten file, raw, and mixed swap cells observed real positive page-out and
page-in counters, complete anonymous-page readback integrity, and the
object-backed shared-map pressure marker. The complete evidence tree contains
no USB BOT/storage fatal diagnostic. BR-T42 through BR-T45, focused USB/VM/
storage regressions, all four production build/link paths with `make -j16`,
relative-document link validation, harness negative self-validation, and
`git diff --check` also pass. `make check` was not run and `.internal/` was not
used.

A final ownership review hardened the USB-backed swap path before `final-007`:
xHCI now reserves its page-sized transfer request/DMA, all reusable storage
URBs wait for terminal HCD ownership release, controller stop gates both URB
submissions and non-URB HCD operations, and checked PCI IRQ removal retains all
resources until an in-flight handler drains. The focused timeout and first-
attempt-`EBUSY` host regressions pass. The pre-existing unchecked EHCI/UHCI
caller lifetime is carried separately as
[`ws004-p009`](ws004-hardware/phase009-pci-hcd-irq-teardown/phase.md).

An initial PC-98 default failure exposed that the production essential-program
selection could include getty without `/bin/login`; the common essential list
and BR-T46 configuration/rebuild boundary now include login. The resulting
missing-login teardown invalid-free remains separately tracked by
[`ws002-p021`](ws002-services/phase021-missing-login-session-teardown/phase.md)
and is not required to recur for this Queue. The stopped `final-005` run is
non-authoritative; `final-006` was superseded by the post-review run, and only
`final-007` is the completion record. The public
`mmap()` flag mask still makes its existing `MAP_FIXED` replacement branch
unreachable; BR-T46 legitimately uses `MAP_FIXED_NOREPLACE`, and the remaining
`MAP_FIXED` contract work stays outside q015.

## Self-driving and stop rules

- No physical boot, image write to removable media, or other user-operated
  action is required. QEMU evidence is sufficient for this Queue's declared
  result.
- Use `make -j16`; do not run `make check`, consume `.internal/`, or commit.
- Preserve existing unrelated working-tree changes. BR-T46 uses disposable
  image copies and isolated logs/temp directories, and must not overwrite the
  user's saved `config.mk`.
- A defect inside the common parser, the four x86 handoffs, boot/root selection,
  swap activation, generated layouts, or BR-T46 harness remains in scope and
  should be repaired without asking for routine implementation choices.
- Mark the affected Phase `uncleared` and stop dependent items if a documented
  Phase reconsideration boundary is reached, the PC-98 production path cannot
  be exercised by its available emulator, an existing 32-bit VM slot limit
  cannot safely represent the required swap contract, or a genuinely
  unrelated platform defect prevents runtime acceptance.
- Do not weaken the public contract, substitute parser fixtures for QEMU
  runtime evidence, or silently expand into CPAR menus, FAT LFN, runtime
  swapon/swapoff, broad bootloader rewrites, or physical-machine acceptance.

## Approval boundary

q015 was explicitly approved on 2026-08-27. Execute only the five listed
Phases and apply the self-driving and stop rules above.
