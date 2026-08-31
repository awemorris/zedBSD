# WS020 shared tests

Parent: [WS020](../ws.md)

| Case ID | Phase | Required observation |
| --- | --- | --- |
| `MAC-T001` | p001 | **PASS:** Architecture/Board/Variant values round-trip and compiled amd64 artifacts remain invariant across all three Variants |
| `MAC-T010` | p002 | **PASS:** The production checker proves the exact MBR/GPT, partition, and loader contracts for the three fixed layouts |
| `MAC-T011` | p002 | **PASS:** The kernel distinguishes the strict zero-tail primary-only diagnostic from ordinary exact/bounded one-copy recovery, including noncanonical source-shape lookalikes |
| `MAC-T020` | p003 | **PASS:** Hybrid boots under SeaBIOS and OVMF; BIOS-only boots only under SeaBIOS; UEFI-only boots only under OVMF |
| `MAC-T030` | p004 | One provisional Intel Mac boot passes, followed only at final acceptance by five consecutive cold boots of the frozen artifact |
| `MAC-T021` | p005 | **PASS:** A fresh checked UEFI-only source, sparsely presented as a 60,549,120-sector xHCI USB disk, publishes exactly two partitions, resolves its payload UUID to `/dev/sda2`, and reaches login without mutating the source |
| `MAC-T022` | p006 | **PASS:** With a structurally valid Protective MBR whose advertised extent disagrees with a CRC/structure-valid GPT, the kernel warns, gives the GPT precedence, and reaches the exact root/data/swap/login state without mutating the checked source |

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
larger-media acceptance, exact/bounded one-copy recovery, CRC-valid invalid-
geometry fallback, maximum-two backup discovery, unrelated-third-LBA
rejection, and sanitizer/analyzer matrix is run with:

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

The q047 run retained under `../temp/p003-q047-final/` passes all six strict
cells in one uninterrupted matrix. The four positive cells reach exact
`init: system running` and `login:` markers and settle cleanly; both negative
cells pass their byte-level absence, bounded firmware-observation, live-settle,
and empty-guest-log gates. Earlier q036 apparent init/getty stalls remain
separately recorded under the now-resolved `BUG-002`; `ws002-p022` repaired
their USB submit/terminal-publication IRQ handoff. They are not counted as
passes or used to weaken this result.

Run the focused current-production handoff preflight with an absent evidence
path:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws020-intel-mac/tests/qemu-uefi-larger-media.noct \
  . plan/ws020-intel-mac/temp/p005-production-preflight
```

`MAC-T021` builds only the UEFI-only profile in a private tree, applies the
production byte checker, freezes the source identity, and enlarges only a
disposable copy to the observed 60,549,120-sector USB capacity. It is a focused
storage/handoff gate and does not replace or weaken p003's six-cell oracle. The
q047 refresh is retained under `../temp/p005-q047-refresh/`; its source and
atomically published `build/amd64/hdd-image.img` share SHA-256
`f811a0f5eff70f8081b6725f417355afa9ef1bf14e0c6d24fd1900823ad09c96`.

Run the GPT/Protective-MBR extent-precedence gate with an absent evidence path:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws020-intel-mac/tests/qemu-uefi-relocated-gpt.noct \
  . plan/ws020-intel-mac/temp/p006-relocated-gpt
```

`MAC-T022` builds a fresh UEFI-only source in a private tree and applies the
same production byte checker used by `MAC-T021`. It creates a disposable
60,549,120-sector sparse copy, leaves the compact PMBR byte-for-byte unchanged,
relocates a complete CRC-valid matching GPT pair to the physical end, and boots
that copy through OVMF/Q35/xHCI. This deterministic fixture reproduces the
observed medium; it does not restrict the parser policy to matching pairs or to
partitions in the old PMBR-advertised prefix. The policy under test is general:
an extent disagreement is a warning, and a CRC/structure-valid GPT is
authoritative. The guest oracle requires the exact mismatch warning, the two
partitions carried by this source, the checked payload UUID, root/data overlay,
active swap, init, and the exact login prompt. Both the pristine source and
repaired QEMU input are hash-checked after the run. The ordinary pristine
larger-media boot remains the separate `MAC-T021` gate.

The first and only post-p022 `MAC-T022` run is retained under
`../temp/p006-q047-post-p022-once/` and passed in 27 seconds. Its checked
source SHA-256 is
`692160cf708904c7444a920022135aad3e10a6f0e78766f88f874a9a6451331d`,
payload UUID `A93F-BBBE`, and relocated-copy SHA-256 is
`e0c185a6db8ada60566663a93f1771d10a08ea3ca6b7a56e4603ea927931c1a0`.
The companion pristine `MAC-T021` and uninterrupted six-cell `MAC-T020` runs
are retained under `../temp/p006-q047-post-p022-pristine-once/` and
`../temp/p006-q047-post-p022-matrix-once/`; both passed without retry.

Reusable runners added by an authorized Phase live here. Disposable boot
media and QEMU logs live below `../temp/` and remain untracked. Do not consume
`.internal/` or invoke aggregate `make check`.
