# Queue: Noct 2.0.1 and userland source lifecycle

Last updated: 2026-09-02

QID: `q063`

Queue status: finished

Queue finished: **Yes**

Authorization: the user deferred q062's exact AX211 physical test, requested
WS008's current Noct release for both toolchain and userland, selected
`userland/base/noct/`, requested top-level source acquisition and a common
per-item lifecycle, and explicitly authorized a tracked target patch.

Timebox: none. Execute only the three finite items below in dependency order.
Do not broaden the target patch beyond CMake/final-link integration, claim
i386/PC-98 target Noct, re-enable Remacs, or consume `.internal/`.

Parent: [master plan](master.md)

Previous Queue: [q062](queue-q062.md)

## Purpose

Replace Noct Git acquisition with one verified official `v2.0.1` source
archive, make it the common host and amd64 target source identity, relocate the
target package under `userland/base/noct/`, and give every userland item a
uniform `download -> patch -> build -> install` interface. Top-level
`make download` must materialize declared external inputs without adding blobs
to Git or making ordinary builds silently network-dependent.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws010-p005` | [userland source lifecycle](ws010-scripting/phase005-userland-source-lifecycle/phase.md) | complete (`q063`) | Every tracked userland item exposes download/patch/build/install; top-level download verifies all declared initial external inputs |
| 2 | `ws008-p010` | [host CLI contract repair](ws008-noct/phase010-host-script-cli-contract-repair/phase.md) | complete (`q063`) | Host toolchain is built from verified Noct 2.0.1 and passes `NOCT-T080`--`T086`, clean/incremental toolchain, and ordinary build gates |
| 3 | `ws008-p009` | [base Noct relocation and target resume](ws008-noct/phase009-base-noct-relocation-target-resume/phase.md) | complete (`q063`) | Target integration moves under base, the strict two-hunk patch attaches the existing link adapter, and the optional amd64 artifact passes static/package and bounded QEMU gates |

## Accepted decisions

- Official release: `v2.0.1`; tag commit
  `ed621e79139f55d06dd1a474243afbf0ce5efe0a`.
- Source archive URL:
  `https://github.com/awemorris/NoctLang/archive/refs/tags/v2.0.1.tar.gz`;
  exact size `2524680`; SHA-256
  `68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`.
- The tarball is stored below `userland/base/noct/distfiles/` after download
  and is ignored by Git. Host and target extractions use that same byte stream.
- The target extraction is `userland/base/noct/noct/`. The legacy
  `userland/noct` integration is removed. Its dirty ignored checkout was
  preserved outside the repository before removal.
- The target-only tracked patch may change exactly the hard-coded adapter path
  and enable/include/invoke the existing zedBSD final-link adapter. It does not
  alter BeUI, Noct APIs, CLI, runtime, JIT, or public headers.
- Target Noct remains amd64-only and optional at `/usr/bin/noct`. Remacs stays
  held in this Queue.
- Every userland item accepts the four lifecycle targets; an item without an
  external input or patch may use a successful no-op for those stages.
- Top-level `make download` is configuration-optional and includes unselected
  declared firmware/source inputs. Ordinary `make -j16` does not implicitly
  acquire them except where an explicitly selected package needs its input.

## Completion definition

Q063 finishes when all three items are completed or honestly uncleared with a
concrete resume condition, P/W/M/Q records agree, and no registry item remains
pending or in progress. Automatic source/lifecycle/host/target/build evidence
must be recorded. Physical input interaction may be left as a bounded QEMU
gate; this Queue does not request a human hardware checkpoint.

## Execution result

Finished on 2026-09-02 with all three items complete.

- `ws010-p005`: all 177 maintained userland Makefiles expose the uniform
  `download -> patch -> build -> install` lifecycle. Config-free preparation,
  repeated top-level `make download`, both firmware caches, standalone Noct
  build/install, corruption rejection, and ordinary `make -j16` passed. Git
  tracks acquisition identities, licenses, and patches, but not downloaded
  source or firmware bytes.
- `ws008-p010`: host Noct now comes from the official `v2.0.1` archive at tag
  commit `ed621e79139f55d06dd1a474243afbf0ce5efe0a`, exact size `2524680`,
  and SHA-256
  `68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`.
  `NOCT-T080`--`NOCT-T086`, all 73 live `--path` recipe consumers,
  clean/incremental `make -j16 toolchain`, host-artifact recovery, and the
  ordinary configured build passed. The host executable SHA-256 is
  `db128557cacc7385976e491a26528bf14e3cfd47a2b9dbff78a63a64617653f6`.
- `ws008-p009`: target integration is owned by `userland/base/noct/`; target
  and host extractions use the same verified archive. The strict two-hunk
  target-only patch updates and invokes the zedBSD final-link CMake adapter.
  It is explicitly not a BeUI adapter and does not modify upstream BeUI,
  language, runtime, CLI, JIT, or public headers. The optional amd64-only
  static target, package artifact, and staged `/usr/bin/noct` are
  byte-identical at SHA-256
  `e8ee34e05a79f89baefe30f57932cb7c543b4285edfddd61e9083e4e1ad92641`.
  Disposable q35/xHCI USB-root non-JIT, RW-to-RX/JIT, and upstream BeUI cells
  passed. Remacs remains held and i386/PC-98 target Noct is not claimed.

The first non-JIT attempt used the legacy IDE path and encountered the already
recorded intermittent `BUG-001` `BIO_FLUSH` failure before Noct ran. The same
image passed on retry, and all canonical q063 runtime gates passed on the
primary q35/xHCI USB-root path. `BUG-001` therefore remains a known,
non-blocking IDE defect and is not attributed to Noct. No physical hardware
result is claimed by q063.
