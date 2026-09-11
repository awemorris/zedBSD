<!-- awesome-plan project=zedbsd record=ws002-p018 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase018/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p018: POSIX.1-2024 `/bin/sh`

WSID: `ws002`

Phase ID: `p018`

Status: partial with recorded compatibility handoffs

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Remove zedBSD administration builtins and direct power ioctls, separate
interactive libedit behavior, and move `/bin/sh` toward Issue 8 grammar,
expansion, redirection, execution, trap/job-control, strict-mode, and selected
widely used extension behavior.

## Acceptance and result

The shell reached the minimum needed by login, cron, scripts, and integrated
QEMU operation. It is not promoted to full POSIX compliance: remaining grammar,
expansion, job-control, and error-semantic gaps stay in
[WS001](https://github.com/awemorris/zedBSD/issues/2). Host shell tests and installed QEMU scenarios
are indexed in [WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md).

## Interruption and resume point

The Phase stopped at a useful partial result under its partial-success rule.
Resume standards work by extracting a new WS001 Phase from the shell ledger,
not by changing this historical result to complete.

## Completion conditions

- Removed zedBSD administration builtins and startup behavior do not remain in `/bin/sh`.
- The declared grammar, expansion, redirection, execution, strict/extension,
  interactive, signal, and job-control test set passes.
- Login, cron, and non-interactive scripts work in QEMU.
- Any omitted POSIX requirement is recorded in WS001; otherwise this Phase remains partial.
