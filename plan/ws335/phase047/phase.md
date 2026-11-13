<!-- awesome-plan project=zedbsd record=ws335-p047 -->

# ws335-p047: 規約の全文の見直し（compat を前提にした TLS の書き換え）

Status: planned
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

（担当が書く）
