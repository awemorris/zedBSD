# Queue: legacy HCD retirement, Wi-Fi credentials, and host Noct refresh

Last updated: 2026-08-31

QID: `q041`

Queue status: in-progress

Queue finished: **No**

Authorization: the user explicitly requested that, after the current fixes,
the remaining workstreams continue automatically.  The standing priority
order is WS004, WS005, then WS008.  Every selected Phase has a frozen design,
known dependencies, and an automatic verification path; no selected item
contains a pending product decision.

Timebox: none. Process each finite item to `completed` or `uncleared`, record
any newly discovered human decision in its P/W/M books, and continue to the
next independent item.  Do not weaken a safety boundary to force completion.

Parent: [master plan](master.md)

Previous Queue: [q040](queue-q040.md)

## Purpose

Complete the oldest dependency-ready hardware lifetime repair, then implement
the independently usable local Wi-Fi profile store, and finally refresh the
host Noct script interpreter to one immutable latest-upstream revision.  This
Queue deliberately advances software-only work while the Archer printed label
and later physical WLAN driver remain external checkpoints.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p016` | [Phase](ws004-hardware/phase016-legacy-hcd-request-retirement/phase.md) | completed | UHCI frame and EHCI fresh Async Advance retirement, failure retention, callback re-entry, toggle continuity, 8,189-check model/sanitizer/analyzer gates, configured x86 builds, and UHCI/EHCI QEMU enumeration/bulk/reboot lifecycle pass; unavailable fault injection is explicitly model-only |
| 2 | `ws005-p005` | [Phase](ws005-networking/phase005-wifi-credential-store/phase.md) | uncleared | Host parser/store/CLI and regressions pass; native non-root ownership and truthful directory-sync acceptance wait for newly planned `ws001-p015` and `ws001-p016` |
| 3 | `ws008-p008` | [Phase](ws008-noct/phase008-latest-host-toolchain-pin/phase.md) | pending | Resolve public Noct `main` once, pin the full immutable commit for the host toolchain only, leave a clean detached `build/NoctLang`, and pass clean plus incremental `make toolchain` smoke without touching target Noct paths |

## Dependency and deferral decisions

- `ws004-p016` depends only on completed p009, p011, and p015.  It does not
  depend on the Archer label, NVMe hardware, or the later WLAN common core.
- `ws005-p005` depends on the frozen p002 file/command contract.  Completed
  p003 is available for later networkd use but is not required by local
  `set-key`; physical WLAN work does not block this Phase.  q041's read-only
  kernel audit discovered two prerequisite defects, now owned by WS001 p015
  and p016.  They are not silently added to this Queue; p005 is requeued only
  after both complete.
- `ws008-p008` changes only the host interpreter below `build/NoctLang` and
  the immutable top-level pin.  Target package p009 remains blocked and is not
  smuggled into this Queue.
- `ws004-p027`/p028 remain held by the missing printed Archer label.
  `ws004-p021` and physical p025 are ready/physical work but are lower than the
  explicitly selected sequence and are not bundled here.
- `ws017-p001` still needs the recorded mapping-permission decision;
  `ws019` needs the missing Noct installer contract; WS011 VLAN/bridge design
  remains open; WS013--WS015 remain manually blocked.  None is executed by
  q041.

## Fixed boundaries

- p016 may not approximate hardware retirement with an arbitrary delay.  If
  a controller-observed boundary or required QEMU event cannot be established,
  retain ownership and mark the exact sub-gate `uncleared`.
- p005 never contacts `networkd`, invokes `wifi`/`ifconfig`/`dhcpc`, trusts
  `HOME`, writes secrets to logs, or edits `/etc/net.conf`.  Tests use only
  synthetic credentials in disposable project-local directories/images.
- p008 resolves the public remote exactly once and pins a full commit.  It
  must not modify, commit, or push upstream Noct, re-enable target packages,
  touch the old dirty checkout, or silently retain a moving ref.
- Do not consume `.internal/` or run aggregate `make check`.  Use `make -j16`,
  focused owner tests, project-local temporary directories, disposable QEMU
  images, and `git diff --check`.
- Commit locally after each processed Phase.  Do not retry the previously
  rejected push without a new explicit informed authorization.

## Completion definition

q041 is finished when all three selected items have been processed to
`completed` or `uncleared` and their exact evidence/resume conditions are
synchronized into P, W, and M.  An honest controller/QEMU limitation or
upstream/toolchain failure may leave one item uncleared without blocking the
next independent item.
