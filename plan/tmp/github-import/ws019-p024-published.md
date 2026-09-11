<!-- awesome-plan project=zedbsd record=ws019-p024 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase024/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019 Phase 024: Noct terminal input progress

Phase ID: ws019-p024
Status: completed (q158); [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase024-noct-terminal-input/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

## Evidence and objective

The real-PTY installer confirmation test hangs after a lone Escape byte.
Confirmed with the canonical q155 host Noct binary in JIT mode. Exact input,
output and timeout are reported by `tests/installer-confirmation-host.py`.
`api-term-ansi.c:in_fill` returns success whenever the input ring is nonempty.
The decoder retains an incomplete Escape sequence, so the read loop repeatedly
receives false progress and never polls or reaches the Escape timeout. The
same structure also affects fragmented CSI and multibyte input.

Repair actual terminal input progress through the canonical Noct package patch
mechanism; do not edit extracted sources without updating source verification.
Retain the upstream pin and add an identified local patch. Do not replace
interactive confirmation with a noninteractive installer option.

## Design and acceptance

The decoder consumes complete events. When it requests more bytes, the fill
operation must poll for genuinely new input even when a partial event remains.
Bound reads by remaining ring capacity. Keep Escape disambiguation, EOF and
ordinary timeout distinct; incomplete CSI/UTF-8 must not spin indefinitely on
timeout or EOF. Review every fill caller before changing this contract.

Test real PTYs in JIT/interpreter mode for exact confirmation, editing,
mismatch, overflow, Escape, Ctrl-C/Ctrl-D and restoration of terminal attributes.
Add fragmented Escape/CSI/UTF-8 and EOF/timeout cases for the terminal API.
Preserve noninteractive refusal. Verify archive-plus-patches manifests, rebuild
host/target Noct and maintained architecture builds, and run native confirmation
acceptance. p004 resumes public integration after these gates; partial host
success alone does not clear this phase.
