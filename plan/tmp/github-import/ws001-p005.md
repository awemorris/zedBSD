<!-- awesome-plan project=zedbsd record=ws001-p005 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase005/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p005: process, credentials, and System V IPC tools

WSID: `ws001`

Phase ID: `p005`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Implement the planned process, credential, priority, and System V IPC utilities
against explicit kernel/UAPI behavior rather than host-only substitutes.

## Design and acceptance

- Define kernel/UAPI ownership and permission checks before command behavior.
- Keep parsing/formatting host-testable but validate actual syscalls in QEMU.
- Cover invalid IDs, permissions, ranges, and resource cleanup.
- Run Phase 5 focused host cases, build/install gates, and QEMU integration.

Shared cases are indexed in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The planned Phase completed. Any incomplete syscall semantics remain explicit
kernel/API rows in [WS001](https://github.com/awemorris/zedBSD/issues/2), not hidden by utility success.

## Completion conditions

- Selected process, credential, priority, and IPC commands build and install.
- Required kernel/UAPI permission, invalid-ID, range, and cleanup cases pass.
- Host logic, top-level build, and Phase 5 QEMU tests pass.
