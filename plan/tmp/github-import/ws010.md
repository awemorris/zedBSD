<!-- awesome-plan project=zedbsd record=ws010 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws010/ws.md`

親: [master](https://github.com/awemorris/zedBSD/issues/1)

# WS010: Noct scripting and build tools

<!-- traceability:start -->

## Goal traceability

- Primary Milestone: **MG001 — 継続開発できる基盤が揃う**
- Related Milestones: MG007
- Objectives: O1, O2, O4, O5
- 貢献する成果: Noctによるビルド/イメージツールを維持する。
- 上位定義: [MasterのObjectives / Milestone Goals](https://github.com/awemorris/zedBSD/issues/1)

既存Phaseは本WSを親として上位成果に接続する。Primaryは分類と責任の所在であり、
各PhaseがRelatedすべてを満たすという意味ではない。成果・検証・限界は各Phaseの
現行記録を根拠とする。今回の対応付けは状態変更・未定義作業の追加・実行許可ではない。

<!-- traceability:end -->


Last updated: 2026-09-02

WSID: `ws010`

Status: complete (`q063`)

Parent: [master plan](https://github.com/awemorris/zedBSD/issues/1)

Last verified Phase: `ws010-p005`

Resume point: no Phase remains; extract a new requirement before resuming.

WS021 consumes this completed host-Noct bootstrap and extends `make toolchain`
with LLVM 23.1.0. It does not rebuild the host interpreter with the target
compiler; only zedBSD target artifacts use the resulting LLVM installation.

Shared tests: [WS010 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/tests/README.md)

## Goals

- Build the upstream `awemorris/NoctLang` host interpreter through
  `make toolchain`, with its verified release extraction and build artifacts
  below `build/`.
- Replace every Python script reached by the amd64, PC-98 i386, and PC/AT i386
  disk-image dependency graphs with a Noct script.
- Make Noct the supported scripting runtime for those image builds and their
  QEMU acceptance tooling.
- Preserve bootable amd64, PC-98 i386, and PC/AT i386 disk images.

## WS completion conditions

The original scripting milestone is complete when `make toolchain` obtains and builds Noct below
`build/`; none of the three required disk-image builds or their acceptance
harnesses invokes Python; all directly and transitively reached Python tools
have Noct replacements; and the amd64, PC-98 i386, and PC/AT i386 disk images
boot to the declared ready/login point in their emulators. The resumed WS is
complete when p005 additionally gives every userland item the common
download/patch/build/install lifecycle and top-level `make download`
materializes the declared external source-distribution inputs.

## Phase registry

| Combined ID | Phase | Status | Completion result |
| --- | --- | --- | --- |
| `ws010-p001` | [Noct host toolchain](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/phase001-toolchain/phase.md) | Complete | `make toolchain` obtains pinned Noct and passes the new WS010 smoke test |
| `ws010-p002` | [x86 build-tool migration](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/phase002-x86-build-tools/phase.md) | Complete | The frozen 15-script production closure is replaced by Noct |
| `ws010-p003` | [x86 dependency-closure audit](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/phase003-python-removal/phase.md) | Complete | Forced dry-runs show no Python on the three production paths |
| `ws010-p004` | [three-platform boot acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/phase004-boot-acceptance/phase.md) | Complete | amd64, PC/AT i386, and PC-98 i386 reach `login:` in QEMU |
| `ws010-p005` | [userland source-distribution lifecycle](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/phase005-userland-source-lifecycle/phase.md) | Complete (`q063`, 2026-09-02) | All 177 maintained userland Makefiles expose download/patch/build/install and top-level download materializes declared external inputs without tracking blobs in Git |

## Scope

The migration covers only repository-owned Python directly or transitively
reached by the three required disk-image builds and boot harnesses. Other
platform, audit, menu, and asset-generation Python is outside this WS.
`.internal/` is excluded completely: it is neither an input nor a migration or
test source. Upstream files inside the ignored Noct extraction are also outside
zedBSD source ownership.

Tests required by this WS are implemented under `plan/ws010/tests/`
without copying from or depending on `.internal/`.

## Fixed decisions

- Host source/extraction path: `build/NoctLang`.
- Host interpreter path: `build/NoctLang/build-static/noct`.
- Upstream repository: `https://github.com/awemorris/NoctLang.git`.
- Current immutable host source is the post-2.0.1 `bb239816` archive shared with
  target Noct; [WS019-p023](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase023-noct-large-seek/results.md)
  records the update. Git checkout/fetch is not the active acquisition model.
- Noct scripts use `.noct`; Makefiles invoke the built interpreter explicitly.
- New Makefile targets do not reintroduce a formal aggregate test interface.
- `.internal/` is never a required runtime/build dependency at WS completion.

## Interruption rule

At any future pause, record the last migrated script, remaining reachable
Python inventory, last passing tool contract, last booted platform, and next
exact command in the active Phase and this WS. A wrapper that merely invokes
Python is not a migrated script.
