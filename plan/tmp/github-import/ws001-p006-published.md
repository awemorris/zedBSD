<!-- awesome-plan project=zedbsd record=ws001-p006 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase006/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p006: development utilities

WSID: `ws001`

Phase ID: `p006`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Implement the selected POSIX development utilities and their local parsers,
reports, and staged package interfaces.

## Design and acceptance

- Keep production code zedBSD-local and independently buildable.
- Test deterministic output, malformed source/input, and unsupported options.
- Exercise Phase 6 fixtures and host scripts, then QEMU integration.
- Record toolchain/provider limitations instead of silently skipping them.

Exact cases are indexed in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The Phase implementation gates completed. Full standards review remains
governed by the component rows and future bounded Phases.

## Completion conditions

- Every selected development utility builds and installs from local base source.
- Deterministic output, malformed-input, and unsupported-option cases pass.
- Phase 6 host, build, and QEMU gates pass with limitations recorded in WS001.
