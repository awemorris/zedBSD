<!-- awesome-plan project=zedbsd record=ws065p003 -->

# ws065-p003: builtin の拡張

Phase ID: `ws065-p003`
Parent: [WS065](../ws.md)
Status: cleared
Queue: q453-i01
Disposition: normal

## 範囲

`source`（`.` と同じ、PATH の次に今の directory も探す bash の動き）、`let`、`test`・`[` の `==`、`declare`・`typeset`（`-r`・`-x`・`-i` は値の型なしで、`-g`、`-p`、scalar のみ）、`pushd`・`popd`・`dirs`、`printf -v 名前`、`builtin`。

## 受け入れ

bash を参照にした case が bash と同じ。dash との差分試験が変わらない。

## 実装（2026-09-26）

- `source`: `.` と同じ（special builtin）。bash --posix と同じく PATH だけを探す（今の directory は探さない。`/bin/sh` として動く bash は posix mode）。`.`・`source` の file の後の operand は実行中の位置 parameter になり、終わると戻す。file が関数の外で `set` で置き換えたときだけ残す（bash と同じ。`shift` や関数の中では戻す）。
- `let`（`cond.c`）: 各 operand を算術式として評価し、最後が 0 なら status 1。誤りは status 1。
- `test`・`[` の `==`: `=` と同じ（glob しない）。
- `declare`・`typeset`（`command.c`、declaration utility）: `-i`・`-l`・`-u`・`-r`・`-x`（`+` で外す、`+r` は誤り）、`-g`、`-p`（bash の形: `declare -irx n="6"`、制御文字は `$'...'`）、`-f`・`-F`。関数の中では local（`-g` で大域）。`-a`・`-A`・`-n` は「not supported」で status 2（配列は Future Work）。名前でない operand は status 1。`local` も先頭が option なら同じ option を取る（`local -` と dash の意味は変えない）。
- 変数の属性（`vars.c`）: `SH_VAR_INTEGER`・`SH_VAR_LOWER`・`SH_VAR_UPPER`。代入のたびに算術式として評価（誤りは shell の誤り）、または小文字・大文字に。local の保存と復元は既存の flags に乗る。
- `printf -v 名前`（`-v名前` も）: printf の出力を buffer に集めて代入（`open_memstream` が無いので printf.c の出力を `out_char`・`out_format` に通した）。単独の `/usr/bin/printf` では `-v` は名前が無いとして status 2（`builtin-standalone.c` に stub）。
- `printf %q`（範囲に追加。POSIX では未規定の変換）: bash と同じ引用（空は `''`、制御文字があれば `$'...'`、それ以外は特別な文字の前に `\`、`#`・`~` は先頭だけ）。
- `builtin`（`exec.c` の `command_prefix`）: `builtin 名前` は builtin だけを探す（関数を飛ばす）。`command` と組み合わせられる。`builtin` の後は declaration utility として扱わない（bash と同じ: `builtin export w=$v` は分割される）。`builtin` という関数があればそれが呼ばれる。
- `pushd`・`popd`・`dirs`（`cd.c`）: stack の top は今の directory。`pushd dir`・`pushd`（上 2 つの交換）・`pushd +N`/`-N`（回転）・`-n`、`popd`・`popd +N`/`-N`・`-n`、`dirs -c`・`-l`・`-p`・`-v`・`+N`/`-N`。HOME は `~` で出す。

## 検証（2026-09-26）

### host

| 確認 | 結果 |
| --- | --- |
| bash を参照にする case（`cases/bash-extensions.sh` に 12 件追加: `let`、`==`、`declare`・`typeset`、`declare -f`・`-F`、`local` の属性、`printf -v`、`printf %q`、`builtin`、`builtin` と `command`、`source`・`.` の operand、`pushd`・`popd`・`dirs`） | 33/33（`bash --posix` と同じ） |
| dash との差分試験 | 1438/1458（p002 の commit の sh は同じ case で 1432/1458）。dash に無い builtin を使う oils の 18 件は我々が bash と同じになり、`BASH_REFERENCE` に移した。新たな差は 6 件: `Source nonexistent`（status 2。dash と同じにした `.` の not found）、`kill -l`・`-L` の不正な引数（`builtin kill` で届くようになった既存の動き）2 件、`${x@Q}` と `set` の引用の形 2 件、`env -i` の起動の変数 1 件（どれも F-019 に記録） |

bash と違う点（残す）: 読み取り専用への代入の誤りの status は 2（dash と同じ）、`declare -a`・`-A`・`-n` は not supported（F-018）、`declare` だけの一覧は `set` と同じ形（関数を出さない）。

### guest（QEMU 8 GiB 4 vCPU NVMe）と回帰

| 確認 | 結果 |
| --- | --- |
| build（`-Werror`） | 最初は単独の `/usr/bin/printf` の link で `sh_var_name`・`sh_var_set`・`sh_realloc` が未定義。`builtin-standalone.c` に stub を足して warning 0 |
| sh の差分試験（guest、1458 件） | 1412/1458（p002 は 1405/1447）。p002 と比べて増えた失敗は host の 6 件のうちの 4 件だけ |
| expat の configure・`make -j1`・`make -j4`・`tests/runtests` | configure 7.1〜7.5 秒、`-j1` 9.9〜10.4 秒、`-j4` 4.0〜4.1 秒、status 0、runtests 4932/4932（`make check` は driver が bash を要る既知の制限で status 2） |
| guest の smoke（`declare -i`、`printf -v %q`、`pushd`、`let n++`） | 期待どおり |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-ws065p003/login.png`） |

実機は未実施。

## 結果（2026-09-26、cleared）

`source`（と `.` の operand）、`let`、`test ==`、`declare`・`typeset`（`-i`・`-l`・`-u`・`-r`・`-x`・`-g`・`-p`・`-f`・`-F`）と `local` の同じ option、`printf -v`・`%q`、`builtin`、`pushd`・`popd`・`dirs` を bash と同じ意味で足した。POSIX の意味の dash との差分は変わらない（新たな差は dash に無い機能か、F-019 に記録した既存の差）。

## 訂正（2026-09-26、ws062-p003 で判明）

上の boot test は `IMAGE=build/ws053-full-hal-guest/hdd-image.img` を環境変数で渡したが、当時の `plan/tools/boot-test.sh` は image を引数でしか受け取らず、実際には 2026-09-24 の hybrid の `build/amd64/hdd-image.img`（この Phase の変更を含まない）を起動していた。この Phase の変更を含む image の起動は、同日の guest（`build/ws053-full-hal-guest`、native、NVMe）への SSH の試験で確かめていた。WS067 までの変更を全て含む guest image の boot test を改めて行い PASS（`build/boot-test-amd64-guest-recheck/login.png`、native の root と swap の partition を確認）。`boot-test.sh` は `IMAGE=` も受け取るように直した（ws062-p003）。

