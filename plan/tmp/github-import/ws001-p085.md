<!-- awesome-plan project=zedbsd record=ws001-p085 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase085/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p085: terminfo packages and standalone base builds

Legacy designation: Phase 8.5

WSID: `ws001`

Phase ID: `p085`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Add local `terminfo`, `terminfo-extra`, curses, `tic`, and `infocmp` packages;
align consumers such as `tput`; and convert base programs to independent
Makefiles honoring `PREFIX` and conventional installation directories.

## Design and acceptance

- `terminfo` is a data-only package with a Makefile and major terminals.
- Minor terminals are separated into `terminfo-extra`.
- With `PREFIX=/`, terminal data uses `/lib/terminfo` rather than a nonexistent
  `/share`; other prefixes use their documented hierarchy.
- Base packages build/install independently and no external base source is
  introduced.
- Terminal compiler/reader, curses, package, and QEMU cases pass.

Exact cases are indexed in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The Phase 8.5 package/build milestone completed. Semantic incompatibilities
found later remain in the WS001 ledger.

## Completion conditions

- `terminfo`, `terminfo-extra`, curses, `tic`, and `infocmp` build/install as designed.
- All affected base packages honor `PREFIX`, including `/lib/terminfo` for `/`.
- Terminal stack/tools, standalone package, top build, and Phase 8.5 QEMU tests pass.
