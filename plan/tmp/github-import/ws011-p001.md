<!-- awesome-plan project=zedbsd record=ws011-p001 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws011/phase001/phase.md`

親: [ws011](https://github.com/awemorris/zedBSD/issues/12)

# ws011-p001: `net.conf` v1 format and parser

WSID: `ws011`  
Phase ID: `p001`  
Combined ID: `ws011-p001`  
Status: complete
Parent WS: [WS011](https://github.com/awemorris/zedBSD/issues/12)

## Objective

Freeze the strict YAML-like grammar and implement a native parser, schema
validator, model, and canonical writer without changing current boot behavior.

## Work packages

- [x] Specify indentation, scalars, mappings, lists, comments, limits, and
  source-position diagnostics.
- [x] Implement the bounded parser without an external YAML dependency.
- [x] Validate names, IPv4 data, prefixes, references, topology, and cycles.
- [x] Implement deterministic canonical serialization.
- [x] Add valid, invalid, limit, duplicate-key, and round-trip fixtures.

## Completion conditions

- Every accepted construct has a documented meaning and canonical output.
- Invalid input fails without partial output or configuration.
- Parse/write/parse produces an equivalent model.
- Malformed and limit fixtures do not crash, hang, or exceed declared limits.
- Existing boot configuration remains unchanged until `ws011-p003`.

## Acceptance

Run `NCF-T001`–`NCF-T006` from the [shared test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/tests/README.md).

## Completion record

The normative grammar is [format-v1.md](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase001-netconf/format-v1.md); empty optional
collections are omitted and flow syntax is rejected. The native model/parser/
validator/writer is in `userland/base/net/netconf.[ch]`. The host contract test
passes with strict warnings, the production amd64 `net` binary compiles and
passes its ELF check, changed C/header files were formatted with clang-format
19, and `git diff --check` passes.
