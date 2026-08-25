# Current Work: next executable workstreams

Last updated: 2026-08-25

CWID: `cw001`

CW status: finished

CW finished: **Yes**

Parent: [master plan](master.md)

## 1. Purpose

This document is the execution index for the next seven work items selected
from the master plan. It points to the owning WS, the Phase to execute or
extract, and the authoritative source documents. Detailed design, work
packages, acceptance evidence, and interruption state remain in each WS and
Phase document; this file tracks only cross-WS order and outcome.

`cw001` may finish with unresolved work. An item that was attempted but cannot
meet its local completion conditions is marked **Carried forward**, with the
reason and next dependency recorded. In particular, inability to reproduce the
X11 mouse defect does not block closure of this CW.

## 2. Status model

Every CW item uses exactly one of these states:

| State | Meaning |
| --- | --- |
| Not started | No implementation or Phase extraction has begun for this CW |
| In progress | Phase extraction, implementation, or verification is active |
| Complete | The local Phase completion conditions have passing evidence |
| Carried forward | The item was attempted but could not be cleared; evidence, reason, and resume condition are recorded |

`CW finished` becomes **Yes** when every item is either **Complete** or
**Carried forward**, all affected WS/master status rows have been updated, and
no item remains Not started or In progress. `CW finished: Yes` therefore means
the selected work cycle has been closed; it does not claim that every item was
successfully implemented.

## 3. Execution registry

| Priority | CW item | Owning WS / Phase | Authoritative documents | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | Dell Latitude 5320 hardware inventory | WS003 `BR-00`; `ws003-p001` | [Master](master.md), [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase001-hardware-inventory/phase.md) | Carried forward | BIOS/UEFI, Secure Boot, CPU, GPU, xHCI, NVMe, WLAN, wired/USB network devices and IDs are recorded; unavailable physical access is carried forward explicitly |
| 2 | `/etc/net.conf` v1 format and parser | `ws011-p001` | [WS011](ws011-net-config/ws.md), [Phase](ws011-net-config/phase001-netconf/phase.md), [tests](ws011-net-config/tests/README.md) | Complete | Normative grammar, bounded parser, semantic validator, canonical writer, and round-trip/error fixtures pass |
| 3 | PCIe/DMA/interrupt/xHCI foundation audit | WS004 `HW-00`; `ws004-p001` | [Master](master.md), [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase001-foundation-audit/phase.md), [audit](ws004-hardware/phase001-foundation-audit/audit.md) | Complete | Capability/gap audit is recorded and safe common fixes plus focused regression evidence are completed; hardware-only findings may be carried forward |
| 4 | Documentation structure and link validation | WS009 `DOC-00`; `ws009-p001` | [WS009](ws009-documentation/ws.md), [governance](governance.md), [Phase](ws009-documentation/phase001-information-architecture/phase.md), [docs index](../docs/README.md) | Complete | Product-document hierarchy, style/status rules, navigation, and repeatable relative-link validation are established |
| 5 | evdev compatibility profile | WS006 `IN-00`; `ws006-p001` | [WS006](ws006-input/ws.md), [Phase](ws006-input/phase001-evdev-profile/phase.md), [reference](../docs/reference/evdev.md) | Complete | Public event ABI/profile and Linux/FreeBSD difference table are frozen with header/layout tests; USB HID implementation is not required here |
| 6 | POSIX ledger re-ranking and next bounded Phase | WS001; `ws001-p011` | [Master](master.md), [WS001 ledger](ws001-posix/ws.md), [Phase](ws001-posix/phase011-basename/phase.md) | Complete | Open ledger rows are re-ranked and one bounded next implementation/audit Phase is defined and executed or explicitly carried forward |
| 7a | X11 launch and `startx` packaging repair | WS007 `GFX-00`; `ws007-p001` | [WS007](ws007-graphics/ws.md), [Phase](ws007-graphics/phase001-x11-launch/phase.md) | Complete | Intended image/package contains `startx` and launches a repeatable Xzed session |
| 7b | X11 mouse-coordinate defect investigation | WS007 `GFX-01`; `ws007-p002` | [WS007](ws007-graphics/ws.md), [evdev profile](ws006-input/phase001-evdev-profile/phase.md), [Phase](ws007-graphics/phase002-x11-mouse/phase.md) | Carried forward | Reproduce and fix the pointer defect with evidence; if it cannot be reproduced after the Phase's bounded matrix, record that evidence and mark Carried forward |

## 4. Execution order and dependencies

The numeric order is the preferred program order, not an assertion that every
item must run serially.

1. Attempt the WS003 physical inventory first when the Latitude 5320 is
   available. If unavailable, record the access blocker and continue with
   software-only work rather than holding the CW open indefinitely.
2. WS011 `p001` is immediately executable and does not change boot behavior.
3. WS004 begins with source/QEMU auditing; hardware-specific conclusions use
   the WS003 inventory when available.
4. WS009 `DOC-00` can proceed independently and defines the documentation
   structure used by later producer Phases.
5. WS006 `IN-00` freezes ABI/semantics; USB HID and consumer migration remain
   later work.
6. WS001 re-ranks its existing ledger before selecting implementation scope.
7. WS007 performs `GFX-00` before `GFX-01`. The mouse investigation consumes
   the evdev profile where applicable, but failure to reproduce is a permitted
   Carried forward result.

## 5. X11 mouse bounded-result rule

`GFX-01` must define a bounded reproduction matrix before debugging. At
minimum, it records QEMU machine/display/input devices, resolution, relative
versus absolute input, launch path, observed cursor coordinates, and repeated
runs. Outcomes are:

- reproduced and repaired with regression evidence: **Complete**;
- reproduced but not safely repairable in the Phase: **Carried forward**;
- not reproduced across the completed bounded matrix: **Carried forward**, not
  Complete, with the matrix and likely missing condition recorded;
- matrix not run: remains **Not started** or **In progress**, so CW cannot end.

## 6. Per-item update record

Update this table whenever an item changes state. Links should point to the
created Phase's interruption/result section rather than duplicating evidence.

| CW item | Status | Last verified result | Blocker or carry-forward reason | Next action |
| --- | --- | --- | --- | --- |
| 1 | Carried forward | Active host is WSL2; target DMI/PCI/USB/UEFI evidence is unavailable | Physical Latitude access is required | Resume `ws003-p001` on the target or from a supplied inventory |
| 2 | Complete | Grammar frozen; host parser test and amd64 native `net` build pass | None | Continue WS011 later at `ws011-p002` |
| 3 | Complete | PCI rescan and DMA-boundary host tests pass; configured amd64 build passes | Latitude-only PCIe/ACPI/IOMMU facts remain in `ws003-p001` | Define ECAM/MSI prerequisites before HW-01 xHCI work |
| 4 | Complete | Noct validator passes across `docs/` and `plan/` (246 relative links at closure) | Web URLs and fragment anchors are outside DOC-T00 | Continue with DOC-20 or a producer-linked reference Phase |
| 5 | Complete | LP64 and ILP32 header/layout checks pass; compatibility table published | Kernel event device is intentionally deferred to IN-01 | Extract `ws006-p002` for the input core |
| 6 | Complete | Four tiers recorded; basename host suite and native amd64 ELF build pass | Native runtime/locale/allocation proof remains in the utility row | Select the next tier-1 candidate or active tier-0 blocker |
| 7a | Complete | Rebuilt amd64 image runs `startx` by name and displays Xzed/zwm/zshell/zterm | None | Continue later with GFX-02 session lifecycle regression |
| 7b | Carried forward | Repeated relative deltas track and reverse exactly; overflow-safe bounds unit test passes | Original mismatch not reproduced; absolute/evdev path unavailable | Resume with original-device reproducer or WS006 Xzed evdev migration |

## 7. CW closure checklist

- [x] Every registry item is Complete or Carried forward.
- [x] Every extracted Phase has an authoritative `phase.md` and test references.
- [x] Carry-forward items record evidence, reason, dependency, and resume point.
- [x] X11 mouse non-reproduction, if applicable, is recorded as Carried forward.
- [x] Parent WS status, last verified Phase, and resume point are current.
- [x] `plan/master.md` reflects the resulting WS states.
- [x] `CW status` is changed to `finished` and `CW finished` to **Yes**.

## 8. Closure and future resume points

`cw001` closed with six Complete entries and two permitted Carried forward
entries. The next CW should be selected from the master rather than reopening
this execution registry wholesale.

- Resume `ws003-p001` when the physical Latitude or a supplied inventory is
  available.
- Resume `ws007-p002` only with an original-device reproducer or after the
  WS006 evdev/Xzed path exists.
- Immediately executable follow-ups include `ws011-p002`, `ws006-p002`, a
  WS004 ECAM/MSI prerequisite design, WS009 DOC-20, and the next WS001 tier-1
  proof candidate.
