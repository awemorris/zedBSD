# WS020 shared tests

Parent: [WS020](../ws.md)

| Case ID | Phase | Required observation |
| --- | --- | --- |
| `MAC-T001` | p001 | **PASS:** Architecture/Board/Variant values round-trip and compiled amd64 artifacts remain invariant across all three Variants |
| `MAC-T010` | p002 | **PASS:** The production checker proves the exact MBR/GPT, partition, and loader contracts for the three fixed layouts |
| `MAC-T011` | p002 | **PASS:** The kernel distinguishes the strict zero-tail primary-only format from ordinary damaged-copy recovery, including on larger physical media |
| `MAC-T020` | p003 | Hybrid boots under SeaBIOS and OVMF; BIOS-only boots only under SeaBIOS; UEFI-only boots only under OVMF |
| `MAC-T030` | p004 | One provisional Intel Mac boot passes, followed only at final acceptance by five consecutive cold boots of the frozen artifact |

`make menuconfig-host-test` runs the saved-configuration matrix from
`menuconfig-target-host-test.py`. After `make toolchain`, run the compiled
artifact half of `MAC-T001` with:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws020-intel-mac/tests/target-artifact-invariance.noct \
  . plan/ws020-intel-mac/temp/p001-artifact-invariance
```

The runner creates fresh owned build trees for `vmunix`, every BIOS loader,
and `BOOTX64.EFI` under all three amd64 Variants, then compares artifact
hashes, object paths, and expanded source/object/compile flags exactly.

Run the `MAC-T010`/`MAC-T011` production image matrix with:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws020-intel-mac/tests/image-layout-test.noct \
  . plan/ws020-intel-mac/temp/p002-layout
```

The runner covers each fixed layout, CRC-preserving semantic GPT corruption,
cross-layout and forbidden-loader cases, fixed `ZBL1` Stage-2-LBA semantics,
all ESP/payload backup-VBR copies, valid and malformed backup-GPT rejection,
the fixed UEFI-only final-33-sector zero reservation, and atomic destination
retention. Each runner snapshots `config.mk` and owns all mutable target/image
artifacts below its evidence path; distinct evidence paths are safe to run in
parallel. The kernel-side intentional-primary-only classification, bounded
larger-media acceptance, degraded-copy compatibility, and sanitizer/analyzer
matrix is run with:

```sh
plan/ws004-hardware/tests/run-gpt-host-test.sh
```

Run the six-cell `MAC-T020` firmware matrix with an absent evidence path:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws020-intel-mac/tests/qemu-variant-matrix.noct \
  . plan/ws020-intel-mac/temp/p003-qemu
```

The runner uses a fixture-private build and explicitly enables xHCI and USB
mass storage. It fixes QEMU to single-thread TCG and one vCPU because SMP is
outside this image/firmware-path Phase. Every positive cell must reach `login:`
and remain healthy for a short settle interval. Negative cells combine the
production byte-level absence check with a bounded firmware observation and an
empty zedBSD debug log. The UEFI-only source always has a zero final GPT
reservation; after an OVMF boot the disposable copy may retain that state or
contain only a complete, CRC-valid mirrored backup GPT repaired by firmware.

For the UEFI-only/SeaBIOS negative cell, the production checker first proves
the all-zero MBR bootstrap and absence of every zedBSD BIOS loader. The runtime
gate then requires SeaBIOS to select the disk and hand the pure protective MBR
to `0000:7c00`, keeps QEMU alive for the bounded settle interval, and requires
the zedBSD debug log to remain exactly empty. Neither the handoff marker nor a
timeout can pass the cell by itself.

The strict six-cell run is currently not complete: independent fresh attempts
intermittently reached `init: system running` and started `getty_console`, but
then produced no `login:` prompt before the bound. This occurred with both
firmware paths and remained with single-thread TCG/one vCPU, so the runner
correctly fails rather than converting retries into acceptance. The runtime
getty/init scheduling issue is separate from the byte-level layout gates.

Reusable runners added by an authorized Phase live here. Disposable boot
media and QEMU logs live below `../temp/` and remain untracked. Do not consume
`.internal/` or invoke aggregate `make check`.
