<!-- awesome-plan project=zedbsd record=ws065 -->

# WS065: `/bin/sh` に POSIX が未規定とする bash 拡張を足す

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: q452（ws065-p002）
Resume point: p001 cleared。p002（展開の拡張）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー指示「/bin/shの互換性をまず向上させてもらえますか？また、stderrのリダイレクトなどで、bash拡張だがGNU/Linuxでは一般に使われていて、BSD系の/bin/shでは動かないような例はありますか？もし/bin/shに実装しても逸脱にならないなら、この機会に実装してしまうのがいいと思います。」

GNU/Linux の `/bin/sh` script（`/bin/sh` が bash の distribution で書かれたもの）がよく使う bash 拡張のうち、POSIX（XCU 2 章、Issue 8）が結果を未規定とする、または構文の誤りとする書き方だけを、bash と同じ意味で実装する。POSIX に合う script の意味は変えない。

## 出発点（2026-09-26）

- dash との差分試験は host 1417/1425、guest 1390/1425。host の 8 件は dash の癖（WS042 の分類のまま）、guest だけの差は utility の GNU 拡張（`grep -o`、`tail --bytes`、`egrep`）と環境（`/usr/bin/whoami` の場所など）で、sh の不具合ではない。
- bash 拡張の対応の調べ（host で build した sh）: `$'...'`、`[[ ]]`、`function`、`(( ))`、`let`、`|&`、`<<<`、`<( )`、`>&file`、`${v:o:l}`・`${v/p/r}`・`${v^^}`、`source`、`test ==`、`declare`、`pushd`、`printf -v` はどれも無い（構文の誤りか not found）。`local`・`set -o pipefail`・`type` はある。

## 何を足し、何を足さないか（POSIX との関係）

| 拡張 | POSIX での扱い | 判断 |
| --- | --- | --- |
| `$'...'` | Issue 8 で標準 | 足す |
| `[[ ... ]]`、`function` | 予約語になりうる語（XCU 2.4）で、結果は未規定 | 足す |
| `(( ... ))` | `((` で始まる command は未規定（XCU 2.6.3） | 足す |
| `\|&`、`<<<`、`<( )`・`>( )` | POSIX では構文の誤り | 足す |
| `>& file`（語が数字でも `-` でもない） | 未規定（XCU 2.7.6） | 足す（stdout と stderr を file へ） |
| `${v:o:l}`、`${v/p/r}` の類、`${v^}`・`${v,}` の類、`${!v}` | 未規定（bad substitution） | 足す |
| `source`、`let`、`declare`・`typeset`、`pushd`・`popd`・`dirs`、`builtin` | 結果が未規定の command 名として XCU 2.9.1 に列挙 | 足す |
| `test`・`[` の `==` | 未規定の operator | 足す |
| `printf -v 名前` | printf に option は無く、今の sh は `Illegal option` | 足す |
| `&>`・`&>>` | POSIX では `cmd &` と `>file` の 2 つの command（意味が変わる） | 足さない（逸脱） |
| `{a,b}`・`{1..3}` | POSIX では文字どおりの語 | 足さない（逸脱） |
| `echo -e` | XSI の echo は option を取らず `-e` を出す | 足さない（逸脱） |
| `$RANDOM` など bash の特別な変数 | 普通の変数の名前（未設定なら空） | 足さない（逸脱） |
| 配列 `a=(...)` | 構文の誤りなので足せるが、大きい | 後回し（Future Work） |

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws065-p001](phase001/phase.md) | 構文: `$'...'`、`[[ ]]`、`function`、`(( ))`、`\|&`、`<<<`、`>& file`、`<( )`・`>( )` | cleared（q451-i01。bash を参照にする 16/16、dash との差分 host 1412/1425・guest 1400/1442（差は説明のつく 6 件と BUG-054）） | — |
| [ws065-p002](phase002/phase.md) | 展開: `${v:o:l}`、`${v/p/r}`・`${v//p/r}`・`${v/#p/r}`・`${v/%p/r}`、`${v^}`・`${v^^}`・`${v,}`・`${v,,}`、`${!v}` | in-progress（q452-i01） | p001 |
| [ws065-p003](phase003/phase.md) | builtin: `source`、`let`、`test ==`、`declare`・`typeset`、`pushd`・`popd`・`dirs`、`printf -v`、`builtin` | planned | p001 |
| [ws065-p004](phase004/phase.md) | 規約の適合 | planned | p001〜p003 |

## 受け入れ

各拡張が bash と同じ出力・status になる（bash を参照にした差分試験の case を足す）。dash との既存の差分試験（host・guest）が今と同じ。expat の configure・make が同じ。規約。
