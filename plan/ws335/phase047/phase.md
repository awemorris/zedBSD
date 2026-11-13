<!-- awesome-plan project=zedbsd record=ws335-p047 -->

# ws335-p047: 規約の全文の見直し（compat を前提にした TLS の書き換え）

Status: 実装済み・試験 PASS（2026-11-14 P3、q2071-i01。cleared は Q1）
Parent: [WS335](../ws.md)。設計: [p042](../phase042/phase.md)。定義: conformance-reviewer。依存: p046。

## 範囲

p043〜p045 が変えた C の file を `plan/coding-style.md` の全文と照らして読み、書き方だけを直す（動作は変えない）:

- `userland/desktop/libbrowser/net/tls.c`・`net/net.h`・`net/http.c`・`base/os-net.h`
- `userland/desktop/mailer/tls.c`・`mail.h`・`sync.c`・`imap.c`・`smtp.c`（変えた行の周り）
- p045 が変えた時: `base/os-system.h`・`os-system-posix.c`・`os-system-win32.c`、`plan/ws334/tests/unit/test-os-system.c`

見る点の例: 関数の直前の comment、comment に開発の管理情報が無い事（AGENTS.md §10）、名前、`void *` と型の扱い、誤りの道の書き方、行の長さ。header（compat）・Makefile・`mk/` は変えない。

## 確かめ

`sh plan/ws335/tests/run-nettls.sh`、`sh plan/ws335/tests/run-mailer-tls.sh`、`sh plan/ws074/tests/host-build.sh plain`、x86_64 の libbrowser・mailer の build（warning 0）、Windows を変えたら `sh plan/ws334/tests/tls-wine.sh`、`git diff --check`。

## 終わり

commit（`git commit -m WIP -- <変えた path> plan/ws335/phase047/phase.md plan/ws335/ws.md`）、結果の節（直した点の一覧と確かめの最後の行）。これの後に Q1 が WS335 を完了の形にする。

## 結果

2026-11-14 P3（q2071-i01）。変えた行（p043〜p045 の差分）と、tls.c 2 本の全文を `plan/coding-style.md` と照らして読んだ。動作は変えていない（comment と空行だけ）。

直した点（commit は 7d7e857b92）:

- `userland/desktop/libbrowser/net/tls.c`（4 種・計 8 か所）: `net_tls_open`・`net_tls_start` の `tls_set_host` の前に空行と purpose comment（閉じ括弧の後の空行）、`tls_load` の `tls_load_once` の呼び出しに comment と空行、`tls_set_host` の address の道の success return の前に comment、`net_tls_reason` の閉じ括弧と次の関数の comment の間の空行、socket の段落の comment を「Gives the connection its socket.」に（SNI は次の段落の comment へ）。
- `userland/desktop/mailer/tls.c`（2 か所）: `ml_tls_add_ca_file` の success return の前に空行と comment、`tls_load` の `tls_load_locked` の呼び出しに comment。

読んで直す所が無かった file: `net/net.h`・`net/http.c`・`base/os-net.h`・`mail.h`・`sync.c`・`imap.c`・`smtp.c`（p043〜p045 の変更は comment の「OpenSSL」の言い換えだけで、開発の管理情報・古い記述は無い）、`os-system.h`・`os-system-posix.c`・`os-system-win32.c`（p045 の変更は削除だけ。残りに `wb_os_trust_roots` の名残は無い）。

記録だけにした（p043〜p045 の変更ではない既存の違反、直さない）:

- `os-system-posix.c`:160・166・169・175、`os-system-win32.c`:135・297・313・322・332（WS334 の code の paragraph-comment・blank-after-brace）。
- `plan/ws334/tests/unit/test-os-system.c` に style-check の指摘 22 件（forward-declaration 7、paragraph-comment 9、blank-after-brace 5、nested-declaration 2。WS334 の試験で、p045 の変更は削除だけ）。
- `net/http.c` に style-check の指摘 38 件（WS074 の既存。p043 の変更は comment 1 行）。

欠陥に見える所: 無し。確認: `mailer/tls.c` の `ml_tls_read` が `errno` を直に読む（`SSL_ERROR_SYSCALL`）。libbrowser 側は `wb_os_socket_last_error()`。動作の違いは確かめていない（動作は変えない範囲のため直していない）。

## 確かめ

- `python3 plan/tools/style-check.py net/tls.c mailer/tls.c`: 出力無し（前は tls.c に 3 件）。
- `sh plan/ws335/tests/run-nettls.sh`: 最後の行 `run-nettls: PASS (http 15 cases, loader 11 cases, libssl-compat and libcrypto-compat linked)`。
- `sh plan/ws335/tests/run-mailer-tls.sh`: `run-mailer-tls: PASS (8 cases)`。
- `make CONFIGMK=config/config.{x86_64,rpi4,vf2}.mk BUILD_JOBS=4 -j4 build/<name>/packages/{libbrowser,mailer,browser}/.stamp`: 3 つとも rc=0、warning 0（`build/p047/*.log`）。
- `git diff --check`: 出力無し。
- 未実施: `check-exports.sh`（compat の .so を引数に取る。compat は変えていないので流していない）、Windows の `tls-wine.sh`（Windows の C は削除だけで変えていない）、guest の `run-mailer-guest.sh`・`run-browser-guest.sh`・image の build（動作を変えていないため）。
