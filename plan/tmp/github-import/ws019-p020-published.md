<!-- awesome-plan project=zedbsd record=ws019-p020 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase020/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019-p020: reliable df capacity observations

Date: 2026-09-09
Status: completed (q151); [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase020-df-capacity/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 60 active minutes

The installer admission design requires complete successful capacity output.
Current df silently wraps block conversion/percentage arithmetic and ignores
buffered stdout errors. Fix the existing standard command before consuming it.
Retain its 512-byte default, -k and operand columns; implement the standard -P
form using the existing portable single-line output. No new helper command.

Use checked quotient/remainder arithmetic, reject inconsistent statvfs free
counts, preserve per-operand errors and report final flush failure. Avoid
mutating argv for the default root operand. Cover zero/full filesystems,
rounding, maximum representable conversions, overflow, inconsistent records,
multiple operands, option handling and buffered output failure with an
independent host fixture. Run ordinary and ASan/UBSan fixtures, then the three
maintained x86 builds (-j16 with explicit CI configs). Do not repeat unrelated
runtime campaigns. p004 remains incomplete until public admission/integration.
