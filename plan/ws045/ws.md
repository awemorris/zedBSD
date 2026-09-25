<!-- awesome-plan project=zedbsd record=ws045 -->

# WS045: base の text utility の GNU 拡張（sed・awk・grep ほか）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Related Milestones: MG006
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: WS043 で各 utility を POSIX の範囲で作り終えた後。よく使われる GNU 拡張から順に Phase を立てる
<!-- awesome-plan-current:end -->

## 目標

2026-09-24 ユーザー指示: 「sed, awk, grepなどは、GNU拡張も実装したいので、まずPOSIXの範囲で実装したあと、あとでGNU拡張も少しずつ使えるようにしていきましょう。」

WS043 で POSIX の範囲に作り直した base の utility に、GNU の拡張を少しずつ足し、GNU を前提にした script（package の build、
configure、利用者の script）がそのまま動くようにする。

## 方針

- WS043 の utility が POSIX の範囲で完成してから始める（依存）。
- 実際の script で使われる順に入れる。候補（使われ方を調べて Phase で確定する）:
  - sed: `-i`（in place）、`-s`、`-z`、`\+`・`\?`・`\|`（BRE の GNU 拡張）、`\w`・`\s`・`\b`、`\U`・`\L` の置換、`0,/re/`・`first~step`、`e`・`F`・`Q`・`R`・`W`・`T`・`v`。
  - grep: `-r`・`-o`・`-w`・`-h`/`-H`・`-A`/`-B`/`-C`・`--include`・`--color`・`-P` の要否。`egrep`・`fgrep`（POSIX には無いが古い script が使う。`argv[0]` で `-E`/`-F` にし、各 config の program の一覧に足す）。
  - expr: GNU の前置の演算子（`+ token`、`length`・`match`・`substr`・`index`）。GNU は POSIX mode でもこれを keyword として読む。
  - awk: gawk の拡張のうち使われるもの（`gensub`、`systime`・`strftime`、複数文字の RS（regex）と `RT`、`**`・`**=`、`func`、`\x` の escape、`IGNORECASE`）。`length(array)` は ws043-p003 で入れた。
- 差分試験は `plan/tools/utils/util-diff.py` を使い、GNU 拡張の case は POSIXLY_CORRECT を外して比べる。
- 規約（plan/coding-style.md）の全文を適用する。

## Phase 一覧

Phase は WS043 の完了後に立てる。

2026-09-24 追記（ws042-p006）: guest の sh の試験で要ると分かった GNU 拡張: `grep -o`（`egrep -o` を含む）、`tail --bytes`（長い option）、`echo -e`（`/bin/echo`）。 ws042-p012: `grep -A`（oils の `set | grep -A1`）。
