# Queue: kernel driver and platform ownership migration

Last updated: 2026-08-29

QID: `q026`

Queue status: finished

Queue finished: **Yes**

Authorization: after reviewing the remaining `src/kern` driver-like sources
and historical platform directories, the user explicitly instructed the agent
to perform that work. This authorizes the finite WS018 sequence below. It does
not authorize Noct source changes or the later FAT/bootfs semantic migration.

Timebox: no fixed wall-clock limit; four finite Phases, executed as far as this
session safely permits.

Parent: [master plan](master.md)

Previous Queue: [q025](queue-q025.md)

## Purpose

Complete the dependency-ready driver/platform ownership wave: make each PC/AT
and PC-98 graphics backend own its complete `/dev/graphics` frontend, move
fonts and disk-label implementations to driver ownership, collapse each
historical platform directory into one canonical platform translation unit,
move Xzed exclusively to capability-discovered evdev, then move generic input
and console implementations plus independent mouse producers to their final
driver locations and delete `/dev/mouse`.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws018-p009` | [Phase](ws018-kernel-architecture/phase009-independent-graphics-frontends/phase.md) | uncleared | Source/build migration and amd64 runtime pass; PC/AT and PC-98 backend runtime matrix remains explicit residual verification |
| 2 | `ws018-p002` | [Phase](ws018-kernel-architecture/phase002-disklabel-platform-layout/phase.md) | completed | Disk labels live under `src/drivers/disklabel`; each platform has exactly one `src/kern/platform/<platform>.c`, with historical directories absent |
| 3 | `ws018-p007` | [Phase](ws018-kernel-architecture/phase007-xzed-evdev-consumer/phase.md) | completed | Xzed discovers and consumes keyboard and relative/absolute pointer input only through evdev |
| 4 | `ws018-p008` | [Phase](ws018-kernel-architecture/phase008-input-hid-driver-ownership/phase.md) | completed | Input/console implementations have final driver owners, mouse backends publish evdev directly, and `/dev/mouse` is absent |

## Dependency order

```text
ws018-p009 -> ws018-p002

ws018-p007 -> ws018-p008
```

The two chains are architecturally independent, but this Queue processes them
in registry order so build-manifest edits and Phase evidence stay reviewable.

## Known bounds and uncertainty

- The public `<zedbsd/graphics.h>` and evdev UAPIs are frozen for this Queue.
- Source duplication below those interfaces is intentional; a new common
  graphics or mouse frontend is outside scope.
- Missing maintained physical/PC-98 runtime infrastructure may make only the
  corresponding runtime evidence `uncleared`; it does not permit weakening
  source, host-fixture, or build gates.
- FAT consolidation and native VFS/bootfs removal remain p010--p012 and are
  excluded because they change filesystem access semantics beyond this
  ownership wave.

## Execution rules

- Do not inspect or modify `userland/noct/NoctLang` or `/home/awe/NoctLang`.
- Preserve unrelated work; do not reset, checkout, clean, or broadly overwrite
  files.
- Use `make -j16`, focused WS018 fixtures, and `qemu-system-x86_64` for amd64
  runtime verification. Do not use `make check` or `.internal/`.
- Use disposable image copies for guest mutation.
- Stop the affected Phase at its documented reconsideration boundary, record
  exact evidence and resume condition, then continue an independent item.
- Commit each completed Phase as `WIP` and push it. If push cannot proceed,
  retain the local commit and continue.

## Completion definition

q026 is finished after all four entries are `completed` or `uncleared`, actual
results are synchronized to P/W/M/Q, and every supported build plus applicable
focused/runtime gate has recorded evidence. A finished Queue may retain an
`uncleared` runtime-only result, but may not claim ownership deletion that is
not present in the source tree.

## Execution result

Finished on 2026-08-29. Three Phases completed and `ws018-p009` remains
`uncleared` solely for its explicit graphics runtime matrix.

- Independent PC/AT and PC-98 graphics frontends, driver-owned fonts, and all
  source/build ownership work in p009 are implemented. Its focused tests, six
  builds, configuration toggles, amd64 Xzed render, and console restoration
  passed. PC/AT VGA/Cirrus, PC-98 GDC/Cirrus, and booted graphics-disabled
  node-absence runs remain recorded in that Phase as residual verification.
- Disk-label implementations live under `src/drivers/disklabel/`. The
  historical `src/kern/{pcat,pc98,rpi4,sun4u,x68k}/` directories are absent,
  and `src/kern/platform/` contains only its README and exactly one complete
  translation unit per platform.
- Xzed is an evdev-only keyboard and pointer consumer. Generic input sources,
  the console frontend, and the two independent physical mouse drivers now
  have their final `src/drivers/` owners; `/dev/mouse`, its UAPI, and the
  shared mouse frontend are absent.
- Focused ordinary and sanitizer fixtures, all six supported kernel builds,
  amd64 Xzed operation through production evdev, PC-98 boot/input registration,
  live legacy-source audits, independent review, and `git diff --check` passed.
  Neither `make check` nor `.internal/` was used, and no Noct source was
  inspected or changed.
