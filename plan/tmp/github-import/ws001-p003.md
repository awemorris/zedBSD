<!-- awesome-plan project=zedbsd record=ws001-p003 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase003/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p003: locale catalogs and terminal descriptions

WSID: `ws001`

Phase ID: `p003`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Add locale database/catalog foundations and the first terminal-description
support needed by POSIX utilities, with malformed-input and staged-install
coverage.

## Design and acceptance

- Keep generated locale/catalog data reproducible from local fixtures.
- Separate parsers and database lookup from command frontends.
- Validate catalog, locale, and character-map errors deterministically.
- Run Phase 3 host tests and bounded QEMU integration.

Fixtures and executable cases are indexed in
[WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The planned Phase 3 foundation completed. Terminfo packaging and standalone
base build work continued separately in `ws001-p085`.

## Completion conditions

- Selected locale, catalog, and terminal-description tools/data build and install.
- Valid and malformed fixture cases pass deterministically.
- Phase 3 host and QEMU integration tests pass.
