<!-- awesome-plan project=zedbsd record=ws046p003 -->

# ws046-p003: automake の idiom と GNU の機能

Phase ID: `ws046-p003`
Parent: [WS046](../ws.md)
Status: cleared（q403-i01、2026-09-24）
Queue: q403（q403-i01）

## 目的

p002 の POSIX の核に、[p001 の設計](../phase001/phase.md)の GNU の機能を足す: 関数（`subst`・`patsubst`・`strip`・`findstring`・`filter`・
`filter-out`・`sort`・`word`・`wordlist`・`words`・`firstword`・`lastword`・`dir`・`notdir`・`suffix`・`basename`・`addsuffix`・`addprefix`・`join`・
`wildcard`・`realpath`・`abspath`・`if`・`or`・`and`・`foreach`・`call`・`eval`・`value`・`origin`・`flavor`・`shell`・`info`・`warning`・`error`）、
target ごとの変数（`+=` は引き継いだ値に足す、前提の target にも及ぶ）、`VPATH` と `vpath`、無い include file を rule で作って読み直す（Makefile の作り直しと exec）、
関数だけの行（`$(eval ...)` を含む）。

## 受け入れ

- 差分試験: `gnu.sh` 31/31、`automake.sh` 15/15、`posix.sh` 45/45（host）。guest でも同じ。
- `style-check.py` の違反 0、host と zedBSD の build で warning 0、boot test。

## 結果（q403-i01、2026-09-24）

- `function.c`: GNU の関数 35 個（表で引く。引数は bracket の外の comma で分け、最後の引数は残りの comma を持つ。`if`・`or`・`and`・`foreach` は使う引数だけを展開、
  `call` は `$(0)`〜`$(n)` を一時の表に置いて値を展開、`eval` は `read_eval()` で makefile の行として読む）。
- `read.c`: 関数だけの行（`$(foreach ...$(eval ...))` など）は展開して読み直す。`target: NAME = value`（`export`・`override` も）、`vpath`、
  見つからない include の記録（main が作る）。
- `variable.c`・`expand.c`: target ごとの変数の `+=` は引き継いだ値に足す、command line の変数は target ごとの変数に勝つ、一時の表を捨てる。
- `rule.c`: target ごとの変数の定義、`vpath` の表と `$(VPATH)` の探索、暗黙の rule の前提を vpath でも探す。
- `update.c`: target ごとの変数を前提と recipe に及ぼす、VPATH で見つけた file の path を自動変数に、作り直すときは名前の場所に。
- `main.c`: 読んだ makefile と無い include を先に更新し（`-n`・`-q`・`-t` に関わらず、GNU make と同じ）、file が変わったら同じ引数で自分を exec し直す（`MAKE_RESTARTS` で 10 回まで）。

計 約 8200 行。

## 検証

| 検証 | 結果 |
| --- | --- |
| host の差分試験（GNU make 4.4.1 と比較） | **91/91**（posix 45、automake 15、gnu 31）（`evidence/host-q403.txt`） |
| guest（amd64、QEMU）の差分試験 | **91/91**（`evidence/guest-q403.txt`） |
| host の実 package（p004 の先取りの確かめ） | expat 2.8.5（依存の追跡あり）を我々の make で configure（make の 3 つの検査が yes）・`make`・`make check`（1/1 PASS）・`make install DESTDIR=`、install された file の一覧が GNU make のときと同じ |
| 規約 | `style-check.py` の違反 0 |
| build | host の `-Wall -Wextra -Wdeclaration-after-statement`、zedBSD の amd64（`-Werror`）で warning 0 |
| boot test（amd64） | login prompt（`build/boot-test-amd64-q403/login.png`。BUG-030 の USB の error が 1 回出た） |
| 実機 | 未実施 |
