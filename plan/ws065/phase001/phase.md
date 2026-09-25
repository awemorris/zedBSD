<!-- awesome-plan project=zedbsd record=ws065p001 -->

# ws065-p001: 構文の拡張

Phase ID: `ws065-p001`
Parent: [WS065](../ws.md)
Status: cleared
Queue: q451-i01（cleared）
Disposition: normal

## 範囲

`$'...'`（Issue 8）、`[[ ... ]]`（文字列・pattern の `==`・`!=`、`=~`、`<`・`>`、単項の検査、`&&`・`||`・`!`・括弧。語は分割も glob もしない）、`function name { ...; }` と `function name() ...`、`(( expr ))`（status は値が 0 なら 1）、`a |& b`（`2>&1 |`）、`<<< word`（here-string、改行を足す）、`>& file`（語が数字でも `-` でもないとき、stdout と stderr を file へ）、`<( cmd )`・`>( cmd )`（`/dev/fd/N`）。`&>` は足さない（ws.md の表）。

## 受け入れ

bash を参照にした case（`plan/ws065/tests/`）が bash と同じ。dash との差分試験が host 1417/1425 のまま。

## 進行の記録（q451-i01、2026-09-26）

### 変えたこと（`userland/base/sh/`）

| 拡張 | 実装 | 場所 |
| --- | --- | --- |
| `$'...'` | 字句の段階で escape（`\a \b \e \E \f \n \r \t \v \\ \' \" \?`、8 進 1〜3 桁、`\xHH`、`\uHHHH`・`\UHHHHHHHH`（UTF-8）、`\cX`）を解き、単一引用符の語として書き直す。NUL で打ち切り（bash と同じ）。`"..."` の中では特別でない | `parser.c`（`lex_dollar_single` ほか） |
| `[[ ... ]]` | 予約語 `[[`。`!`・`&&`・`\|\|`・括弧、単項（test の検査と `-a`・`-o 名前`・`-v 名前`）、`==`・`=`・`!=`（右辺は pattern、引用した文字はそのもの）、`=~`（ERE。右辺は括弧と `\|` を含む 1 語として読み、引用した文字は escape）、`<`・`>`、`-eq` などは両辺を算術式として評価、`-nt`・`-ot`・`-ef`。語は分割も glob もしない。偽は status 1、評価できないと 2。`set -e` の対象 | `parser.c`（`parse_cond_*`・`lex_regex_word`）、新しい `cond.c`、`test.c`（`sh_test_unary`・`sh_test_file_compare`）、`options.c`（`sh_option_named`） |
| `function 名前 { ...; }`・`function 名前() ...` | 予約語 `function` | `parser.c` |
| `(( 式 ))` | `(` の直後に `(` が続くとき、対応する `))` までを算術式として読む。`))` で閉じなければ読んだ文字を返して入れ子の subshell として読み直す（command substitution の中の capture からも外す: `sh_input_give_back_text`）。値が 0 なら status 1。式の誤りは message と status 1（shell は終わらない）。`set -e` の対象 | `parser.c`（`parse_arith_command`・`read_arith_text`）、`exec.c`、`input.c` |
| `for (( 初期; 条件; 更新 ))` | 本体は `do ... done` か `{ ... }`。空の式は省略（条件は真） | `parser.c`（`parse_arith_for`）、`exec.c`（`eval_arith_for`） |
| 算術の `++`・`--`・`,` | 前置・後置の `++`・`--`（変数の前後にあるとき）。数の前や、後置の直後に被演算子が続く `x--1` は POSIX どおり符号（`x-(-1)`、dash と同じ。bash は誤り）。`,` は順に評価して最後の値 | `arithmetic.c` |
| `a \|& b` | `2>&1` を左の command の redirection の最後に足して `\|` | `parser.c`（`add_stderr_to_pipe`） |
| `<<< 語` | 語を展開（分割・glob なし）し、改行を足して here-document と同じ pipe で渡す | `parser.c`、`redir.c`、`shell.h`（`SH_REDIR_HERESTRING`） |
| `>& file` | 語が数字でも `-` でもないとき、stdout と stderr をその file へ（noclobber が効く）。fd 1 以外の `n>&file` は bash と同じく `ambiguous redirect` | `redir.c`（`apply_both_outputs`） |
| `n>&m-`・`n<&m-` | m を n へ動かして m を閉じる（ksh・bash）。`>& file` の変更で `1>&2-` が `2-` という file を作ってしまったのを直す途中で足した | `redir.c` |
| `<( 命令 )`・`>( 命令 )` | 語の始まりの `<(`・`>(` を読み、展開の時に pipe と子を作って `/dev/fd/N` にする。shell の側の端はその command が終わるまで開き、終わったら閉じて子を回収する（`sh_eval` ごと）。`done < <(cmd)` の形も動く | `parser.c`（`lex_process_substitution`）、`expand.c`（`expand_process`）、`exec.c`、`lexer.h`、`expand.h` |
| `${#s}` の文字数 | locale（`LC_ALL`・`LC_CTYPE`・`LANG` の最初に設定されたもの）が UTF-8 なら文字数（XCU 2.6.2 が求める）。それ以外は byte 数 | `expand.c`（`character_count`） |

### 試験の道具

- `plan/tools/sh/sh-diff.py`: dash が別の意味に読む（`((` を入れ子の subshell として走らせるなど）extension の case は `bash --posix` を参照にする（`BASH_REFERENCE` の 25 件と `cases/bash-extensions.sh` の全件）。
- `plan/tools/sh/cases/bash-extensions.sh`（新、bash を参照にする 16 件）、`cases/arithmetic.sh`（新、`--` の POSIX の意味、dash を参照にする）。

### 結果（host で build した sh）

| 試験 | 結果 |
| --- | --- |
| bash を参照にする extension の case | 16/16 |
| dash との差分試験（oils と自前、extension の case は bash を参照） | 1412/1425（変更前 1417）。新たな差 5 件: `(( undef++ ))` と `set -u`（dash も bash も誤りにし、我々は 1 にする）、C locale での `\u`（bash は `\u03BC` を文字どおり出す）、`$'\c''`（mksh と同じ読み。bash は誤り）、oils の「1>&2- (Bash bug)」（我々は正しく `hello world`、dash は `Bad fd number`）、`set` と `eval` の往復（`set` が `$'...'` で出さないと通らない。`bash --posix` も通らない） |
| dash と違って bash と同じになった case | 26 件（`BASH_REFERENCE` に入れた） |

### guest（QEMU 8 GiB 4 vCPU NVMe）と回帰

| 確認 | 結果 |
| --- | --- |
| sh の差分試験（guest、`guest-batches.sh` を NVMe の image に向けた複写、1442 件） | 1400/1442（前 1390/1425。新たな差は host と同じ 5 件、順序に依存する `noclobber on &> >`、それと `diff <(..) <(..)` の 1 件） |
| `diff <(..) <(..)` | sh は正しい（`cat <(..) <(..)`・`< <(..)`・`>(..)` は guest で動く）。zedBSD の `/dev/fd/N` が文字 device として見え、`diff` が「device identity」で違うとする。[BUG-054](../../bugs/BUG-054.md) に記録し、case は `cat <(echo one) <(echo two)` にした。調べる途中の `ls /dev/fd/` で devfs の inode の pool が尽き sshd が落ちた（BUG-054 に記録、guest は起動し直した） |
| make の差分試験（guest） | 100/100 |
| expat の configure・make・`make -j4`（guest） | configure 7.0〜8.0 秒（起動ごとのばらつき。host で前後の sh を比べると 8.2〜8.3 秒で同じ）、直列 10.2〜10.5 秒、`-j4` 4.29〜4.32 秒、`tests/runtests` 4932/4932 |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-ws065p001/login.png`） |

実機は未実施。

## 結果（2026-09-26、cleared）

`$'...'`、`[[ ]]`、`function`、`(( ))`、`for (( ))`、`++`・`--`・`,`、`|&`、`<<<`、`>& file`、`n>&m-`、`<( )`・`>( )`、UTF-8 の `${#s}` を bash と同じ意味で足した。bash を参照にする 16 件が一致、POSIX の意味（`x--1` など）は dash と一致。

