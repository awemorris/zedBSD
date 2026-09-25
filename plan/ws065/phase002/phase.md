<!-- awesome-plan project=zedbsd record=ws065p002 -->

# ws065-p002: 展開の拡張

Phase ID: `ws065-p002`
Parent: [WS065](../ws.md)
Status: cleared
Queue: q452-i01
Disposition: normal

## 範囲

`${v:offset}`・`${v:offset:length}`（負の値を含む）、`${v/p/r}`・`${v//p/r}`・`${v/#p/r}`・`${v/%p/r}`、`${v^}`・`${v^^}`・`${v,}`・`${v,,}`、`${!v}`（間接）。`$@`・`$*` への適用は bash と同じ。

## 受け入れ

bash を参照にした case が bash と同じ。dash との差分試験が変わらない。

## 実装（2026-09-26）

`userland/base/sh/expand.c` だけ。

- `parse_brace`: `${!name}`・`${!1}`・`${!#}` を間接に（`${!}`・`${!-word}` などは `$!` のまま）。operator に `:`（substring。`:-`・`:=`・`:?`・`:+` は従来どおり）、`/`・`//`・`/#`・`/%`、`^`・`^^`、`,`・`,,` を足した。
- `indirect_parameter`: 値を parameter の名前として引き直す。名前でなければ `bad substitution`。
- `brace_substring`: offset と length は `$(( ))` と同じ算術（`?` の対の外の最初の `:` で分ける）。UTF-8 の locale では文字の単位。負の offset は末尾から、負の length は末尾からの位置（開始より前なら `substring expression < 0`）。`@`・`*` は `$0` を 0 番とする列を切り、`append_list`（`append_positionals` から分けた）で並べる。
- `brace_transform`: pattern は trim と同じ読み（引用した文字は文字どおり）で最長一致。`//` は空の一致で 1 文字進む。置換の文字列の引用されない `&` は一致した文字列（bash 5.2 の `patsub_replacement`、`\&` と `"&"` は文字どおり）。大小の変換は ASCII、pattern は 1 文字に対して照合。`@`・`*` は要素ごと。
- `set -u` では未設定の parameter が誤り（bash と同じ）。

## 検証（2026-09-26）

### host

| 確認 | 結果 |
| --- | --- |
| bash を参照にする case（`cases/bash-extensions.sh` に 5 件追加: substring、置換、大小、間接、`set -u`） | 21/21（`bash --posix` と同じ） |
| dash との差分試験 | 1433/1447（前 1412/1425）。新たな差は oils の `var-sub-quote`「`${foo//'c d'/zzz}`」の 1 件だけ。dash はこの展開を持たず status 2、我々は展開して（bash と同じ `a b zzz`）後の `argv.py` が無く 127 |
| 作業中に見つけた回帰 | `${!-x}`（POSIX の `$!` と既定値）が一時 `bad substitution` になった。間接は `!` の次が名前・数字・単独の `#` のときだけにして直し、case に足した |

bash と違う点（残す）: `${!#}` は最後の引数（bash の既定と同じ。`bash --posix` は `$!` の類として読む）。二重引用符の中の `${v/b/$'\t'}` の `$'...'` は展開しない（`bash --posix` も既定値の語では展開せず、一貫しない）。

### guest（QEMU 8 GiB 4 vCPU NVMe）と回帰

| 確認 | 結果 |
| --- | --- |
| sh の差分試験（guest、1447 件） | 1405/1447（前 1400/1442）。失敗の一覧は p001 と比べて host と同じ `var-sub-quote` の 1 件が増え、p001 の `<( )` の 1 件（case を直した）が消えただけ |
| expat の configure・`make -j4`・`tests/runtests`（guest） | configure status 0（3.55 秒）、`make -j4` status 0、runtests 4932/4932。`make check` は試験の driver が bash を要る既知の制限で status 2（ws046 から同じ） |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-ws065p002/login.png`） |

実機は未実施。

## 結果（2026-09-26、cleared）

`${v:o}`・`${v:o:l}`、`${v/p/r}`・`${v//p/r}`・`${v/#p/r}`・`${v/%p/r}`、`${v^}`・`${v^^}`・`${v,}`・`${v,,}`、`${!v}` を bash と同じ意味で足した。`$@`・`$*` への適用も bash と同じ。POSIX の展開（`${v:-x}` など、`$!`）は変わらない。

## 訂正（2026-09-26、ws062-p003 で判明）

上の boot test は `IMAGE=build/ws053-full-hal-guest/hdd-image.img` を環境変数で渡したが、当時の `plan/tools/boot-test.sh` は image を引数でしか受け取らず、実際には 2026-09-24 の hybrid の `build/amd64/hdd-image.img`（この Phase の変更を含まない）を起動していた。この Phase の変更を含む image の起動は、同日の guest（`build/ws053-full-hal-guest`、native、NVMe）への SSH の試験で確かめていた。WS067 までの変更を全て含む guest image の boot test を改めて行い PASS（`build/boot-test-amd64-guest-recheck/login.png`、native の root と swap の partition を確認）。`boot-test.sh` は `IMAGE=` も受け取るように直した（ws062-p003）。

