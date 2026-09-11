<!-- awesome-plan project=zedbsd record=ws001-p004 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase004/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p004: parsers, editor, traversal, and archive

WSID: `ws001`

Phase ID: `p004`

Status: complete milestone; audit findings remain

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Implement the planned `bc`, `ed`, `find`, `m4`, and `pax` packages, including
standalone build/install behavior, parser error handling, and QEMU smoke tests.

## Design and acceptance

- Bound parser input, recursion, arithmetic, paths, and archive sizes.
- Preserve useful failure behavior for unimplemented grammar or options.
- Test package-specific normal and malformed fixtures.
- Verify staged install and Phase 4 QEMU execution.

The Phase 9 audit later found imported source in `bc`, `ed`, and `m4`; that
policy conflict was resolved by `ws001-p010`. Current semantic gaps remain in
the [WS001 ledger](https://github.com/awemorris/zedBSD/issues/2).

## Result and resumption

The original Phase 4 implementation milestone and its subsequent provenance
replacement are complete. Resume utility conformance only through a new Phase.

## Completion conditions

- `bc`, `ed`, `find`, `m4`, and `pax` build and install as local packages.
- Their declared normal and malformed-input fixture tests pass.
- The Phase 4 QEMU gate passes and provenance conflicts are resolved or handed
  to an explicitly named follow-up Phase.
