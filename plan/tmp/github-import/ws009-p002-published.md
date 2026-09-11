<!-- awesome-plan project=zedbsd record=ws009-p002 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws009/phase002/phase.md`

親: [ws009](https://github.com/awemorris/zedBSD/issues/10)

# WS009 Phase 002: build-from-source guide

Last updated: 2026-08-25

Phase ID: `ws009-p002`

Status: complete

Parent: [WS009](https://github.com/awemorris/zedBSD/issues/10)

Product guide: [Build zedBSD from source](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/docs/howto/build-from-source.md)

Tests: [WS009 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws009-documentation/tests/README.md)

## Objective

Make the supported source-to-image and QEMU paths reproducible without relying
on retired repository test targets or undocumented host state.

## Work packages

- [x] Audit the top-level build, toolchain, target selection, and run targets.
- [x] Document host prerequisites and the pinned Noct bootstrap.
- [x] Document `make -j16`, output artifacts, and explicit amd64/i386 QEMU.
- [x] Separate upstream PC/AT QEMU from the custom PC-98 requirement.
- [x] Add expected success markers and failure diagnostics.
- [x] Repair the stale root README standards/test references.
- [x] Re-run the documented toolchain/build commands and link validator.

## Completion conditions

The pinned toolchain is present, `make help`/`list-targets` agree with the
guide, a `make -j16` image build succeeds, amd64 boots through
`qemu-system-x86_64` to the documented marker, and relative links pass.

## Evidence and result

`make toolchain` passes the pinned Noct smoke. `make help` and
`make list-targets` match the documented interface. `make -j16` rebuilds
`build/amd64/hdd-image.img`; an explicit `qemu-system-x86_64` run reaches
`init: system running` and `login:`. DOC-T00 passes across 283 relative links
before closure.

The guide records rather than hides the custom PC-98 QEMU requirement. This
Phase reproduced the active amd64 selection; earlier WS010 evidence remains
the current i386 PC/AT and PC-98 boot record.

## Resume point

Select DOC-40 for the existing init/service system, or a producer-linked public
reference Phase.
