<!-- awesome-plan project=zedbsd record=ws046p002 -->

# ws046-p002: POSIX make の核

Phase ID: `ws046-p002`
Parent: [WS046](../ws.md)
Status: cleared（q401-i01、2026-09-24）
Queue: q401（q401-i01）

## 目的

[p001 の設計](../phase001/phase.md)に沿って `userland/base/make` を作る: Makefile を読む（論理行、comment、指令、代入、rule、recipe）、
変数と展開（入れ子の参照、置換参照、`=`・`::=`・`:=`・`+=`・`?=`・`!=`）、explicit・suffix・double-colon rule、特別な target、`include`、
更新の算法、recipe の実行、option と `MAKEFLAGS`、再帰（`$(MAKE)`、`MAKELEVEL`）、内蔵の rule と macro。

## 受け入れ

- host で build した make が差分試験の `posix.sh` 45 件で GNU make と同じ（`plan/ws046/tests/make-diff.py --make`）。
- 新しい code が `plan/coding-style.md` に合う（`style-check.py` の違反 0）。host の `cc -Wall -Wextra` と zedBSD の build で warning 0。
- config に `make` を足して amd64 の image を build、guest で posix の case を走らせる（p004 の前の確かめ）、boot test。

## 結果（q401-i01、2026-09-24）

`userland/base/make`（9 file、約 6000 行、Zlib）を p001 の設計どおりに作った: `make.h`・`main.c`（引数と `MAKEFLAGS`、既定の変数、内蔵の rule）・
`read.c`（論理行、comment、条件、`define`、`include`、`export`・`unexport`・`override`、代入と rule の区別）・`variable.c`（表、origin の優先、
代入の 5 種、recipe の環境）・`expand.c`（`$(...)`、入れ子、置換参照、自動変数と D・F）・`function.c`（pattern の置換。関数は p003）・
`rule.c`（target の表、特別な target、pattern rule、POSIX の順の suffix rule の探索）・`update.c`（更新の算法、`-q`・`-t`・`-k`、`.DEFAULT`）・
`job.c`（recipe の実行、prefix、`-n` と `$(MAKE)`、error の表示、中断で target を消す）・`util.c`。
`/usr/bin/make` として config（amd64・pcat・pc98・intelmac・ws035 の userland）に足した。

p002 の範囲外だが、条件の指令・`define`・`export`・`override`・pattern rule・order-only は read.c と rule.c の構造に含めたので実装した（p003 の case で確かめる）。

## 検証

| 検証 | 結果 |
| --- | --- |
| host の差分試験（GNU make 4.4.1 と比較） | posix **45/45**、automake 15/15、gnu 17/31（残りは p003 の関数・target ごとの変数・VPATH・include の作成）（`evidence/host-q401.txt`） |
| guest（amd64、QEMU）の差分試験（`make-diff.py --export`、guest で `make-guest.sh`、`--compare`） | posix **45/45**、automake 14/15、gnu 17/31（`evidence/guest-q401.txt`）。automake の 1 件は make ではなく kernel の不具合 [BUG-032](../../bugs/BUG-032.md)（`mkdir -p ./.deps` が EINVAL） |
| 規約 | `style-check.py` の違反 0 |
| build | host の `cc -std=c99 -Wall -Wextra -Wdeclaration-after-statement` で warning 0、zedBSD の amd64 の build（`-Wall -Wextra -Werror`）で warning 0 |
| boot test（amd64） | login prompt（`build/boot-test-amd64-q401/login.png`） |
| 実機 | 未実施 |

BUG-032 は WS046 の実 package の build（p004）を止めるので、ws046-p006 として計画した（kernel の mkdir(2) の errno）。
