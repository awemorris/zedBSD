<!-- awesome-plan project=zedbsd record=ws019-p025 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase025/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p025: Public installer runtime contracts

Status: completed, q159; see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase025-public-runtime-contracts/results.md)
Queue: q159 (explicit user authorization, 2026-09-09)

Normalize the interpreter to `/bin/noct`, including the packaged launcher.
Repair the shell's exit-status contract: preserve `$?` across input lines,
retain child exit codes and pipeline status, and do not implicitly enable
errexit for script files. This addresses BUG-014; it does not assert complete
POSIX shell language coverage.

Implement `/dev/null` and `/dev/zero` in `src/drivers/generic/`, registered
through the common character-device namespace (BUG-015). Reads respectively
return EOF and zero bytes; writes consume the requested bytes; polling must
not block. Keep kernel-buffer and userspace-copy ownership unchanged.

Verification: focused shell status tests (separate lines, scripts, pipelines,
conditionals, exact child status, missing commands), device callback tests,
amd64/pcat/pc98 builds, and native normal-image shell/device checks. Then
resume p004 public installation acceptance using the real devices.

The previous public4 run ended before installation because its temporary
truncate fixture did not create a nonexistent file. Preserve that failure;
replace the fixture with a write from the implemented zero device.

Normative reference: [POSIX Shell Command Language, exit status](https://pubs.opengroup.org/onlinepubs/9799919799/utilities/V3_chap02.html).
The existing builtin predicate API is retained internally; its dispatch boundary
adapts boolean builtins to 0/1 while external/nested execution supplies its exact
status. `$?` retains the last completed command across input calls. Native/host
acceptance is scoped to these contracts, not a claim of full shell conformance.

Host validation passed: `tests/shell-status-test.py` (20 cases), existing
WS001 deterministic job-control suite (`/tmp/q159-status-jobs.log`), and
`tests/memory-device-test.c` against the production driver. Maintained amd64,
pcat and pc98 disk-image builds passed. Normal UFS image verification confirms
the `/bin/noct` and `/bin/zedinst` contents and launcher mode.

Native fixture corrections: `temp/q159-runtime` stopped because this OS's ls
accepts one operand; `temp/q159-runtime2` stopped because the QEMU keyboard
fixture lacked apostrophe mapping. Both runs preserved the failures; the second
confirmed the interpreter and both 0666 character devices, and cross-line status
checks. Neither is recorded as complete runtime acceptance.
