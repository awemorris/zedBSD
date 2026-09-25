<!-- awesome-plan project=zedbsd record=ws042 -->

# WS042: /bin/sh の POSIX 互換性

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-24（q371〜q399）
Primary Milestone: MG002
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

`/bin/sh`（`userland/base/sh`）を POSIX（XCU 2 章 Shell Command Language と sh の builtin）に合わせ、
ほかの POSIX 系で書かれた script（autoconf の configure、package の build script、system の rc/service）がそのまま動くようにする。

きっかけ: 2026-09-24 ユーザー指示「/bin/sh の互換性が低いという現状があります。これを徹底的に修正する phase を、優先度を上げて実施したいです。」

## 結果

- 差分試験（同じ shell code を host の dash と我々の sh で走らせ、stdout と status を比べる。oils の spec の POSIX に関わる file と自前の case）:
  開始時 744/1392（53.4%）→ **host 1417/1425**。残り 8 件は dash の誤りか、時間に依る case。
- guest（amd64、zedBSD の kernel・libc・utility）: **1388/1425**（環境を直した 1 件の再実行で 1389）。host との差は GNU 拡張
  （`grep -o`・`grep -A`・`tail --bytes`・`echo -e`）、環境（一時 directory の名前、`$PPID`、`/usr/bin`、`python2`・bash・`time` の有無、umask、SIGXFSZ の番号）、
  background の出力の順の競合に分類した。sh の不具合によるものは無い。
- 書き直し: 実行の核（`set -e` の規則、trap、redirect、background と `wait`、subshell、関数）、builtin（XCU と照合し、話題ごとの file に分けた）、
  展開、job control。行編集に vi mode（`set -o vi`）を足した（`userland/base/libedit`）。
- 実 script: guest で expat 2.8.5 の `configure` が status 0 で終わり、`expat_config.h` は host（dash と GNU の道具）と Linux 専用の 1 項目だけ違う。
  途中で `ls`（`-i`・`-L`・operand の順）、clang の package の `cc`・`ld`、kernel の `mmap` の `MAP_FIXED` を直した。
- 対話: guest の serial console で 41/41（syntax error、PS2、Ctrl-C、alias、here-document、fc、job control（`&`・`jobs`・`kill`・Ctrl-Z・`bg`・`fg`）、emacs と vi の行編集）。
- 規約: WS の source（`userland/base/sh`・`userland/base/libedit`・`userland/base/ls/main.c`）に `plan/tools/style-check.py` の違反 0（p009〜p011 で約 470 件と禁止の comment の形 178 件をなくした）。
  書き直しの前後で、端末への出力（libedit）と ls の出力が同じことを確かめた。
- build（amd64・aarch64、warning 0）と boot test（amd64・RPi4 の QEMU）。実機での確認は未実施。

## 制限・移管

- expat の **build** と試験を guest で走らせる確かめは make が要るので [WS046](../ws046/ws.md)（GNU 互換の make）の後。
- GNU 拡張（`grep -o`・`-A`、`tail --bytes`、`echo -e` ほか）は [WS045](../ws045/ws.md)。
- vi mode の残りの命令（履歴の検索 `/`・`?`・`n`・`N`、`f F t T ; ,`、`.`、`v`、補完）は Future Work F-006。`j` で最新の先へ出ると編集中の行は戻らない。
- config.guess が zedBSD を知らない件は、package ごとの `config.sub` の patch で扱う（openssh と同じ）。
- 見つけた不具合（tracking）: [BUG-029](../bugs/BUG-029.md)（tmpfs に約 230 個の file で system 全体が file を開けなくなる。guest の試験は 40 件ずつに分けて回避）、
  [BUG-030](../bugs/BUG-030.md)（amd64 の起動時の USB storage の読み取りの error 42、間欠）、[BUG-031](../bugs/BUG-031.md)（起動時の console の行の混ざり）。
- `plan/tools/style-check.py` の誤検出 2 種（`local` builtin の関数名、struct の member）を直した。読んで確かめる規則のうち道具が見ないもの
  （意味のある呼び出しの結果を直接 return しない、分けた呼び出しは 1 行に 1 引数）は、sh では直した箇所の周りだけで確かめた。
- 試験と道具は `plan/tools/sh/`（Tools 節）に移した: `build-host-sh.sh`・`sh-diff.py`・`cases/`・`fetch-oils.sh`・`guest-diff.sh`・`guest-batches.sh`・
  `sh-interactive.py`・`vi-host.py`。ls の書き直しの比較は `plan/tools/utils/ls-compare.sh`。

## Phase 一覧

Phase の記録（phase.md と証拠）は削除した。git の履歴にある（最後の版は 2026-09-24 の q399 の commit）。

| Phase | 内容 | Status |
| --- | --- | --- |
| ws042-p001 | 差分試験の土台と現状の測定（744/1392） | cleared（q371） |
| ws042-p002 | 展開 | canceled（p003 の書き直しと p004 の照合に吸収） |
| ws042-p003 | 文法・実行の意味（`set -e`、trap、redirect、background、subshell、関数） | cleared（q373。1376/1384） |
| ws042-p004 | builtin の残りと規約の照合 | cleared（q374。1417/1425） |
| ws042-p005 | 実 script: guest で expat の configure | cleared（q392。status 0） |
| ws042-p006 | guest での差分試験 | cleared（q393。1390/1425） |
| ws042-p007 | 対話の回帰（job control・行編集） | cleared（q394。29/29） |
| ws042-p008 | vi mode の行編集 | cleared（q395。41/41） |
| ws042-p009 | 規約の全文照合 (1) libedit | cleared（q396） |
| ws042-p010 | 規約の全文照合 (2) sh | cleared（q397。340 → 0） |
| ws042-p011 | 規約の全文照合 (3) ls | cleared（q398） |
| ws042-p012 | 最後の回帰と完了の確認 | cleared（q399。host 1417/1425、guest 1388/1425、対話 41/41、configure status 0） |
