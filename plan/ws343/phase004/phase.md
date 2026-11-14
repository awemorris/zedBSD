<!-- awesome-plan project=zedbsd record=ws343-p004 -->

# ws343-p004 規約の全文の見直し（fetch の main.c）

- 親: [ws.md](../ws.md)。Queue: q2102-i01（P8、worktree `/home/awe/zedBSD-worktrees/p8-ws343p004`、基点 main 7b2d532461）。
- Status: cleared（2026-11-15 Q1、merge eb3f3dcc28）

## 目的と範囲

- WS343 が変えた C を [coding-style.md](../../coding-style.md) の全文と照らし、書き方だけを直す（動作・API・計算の順序・error の値は変えない）。
- 対象: `userland/base/fetch/main.c`（p003 の https の書き直し）。`plan/ws343/tests/passkey-compare.c`（p002 の host の比較の driver）は下の「範囲の外」。tests の shell・Python（`run-*.sh`・`ct-matrix.sh`・`https-server.py`）は C ではないので対象外。

## 確かめ

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| style-check | `python3 plan/tools/style-check.py userland/base/fetch/main.c` | 前 21 件（blank-after-brace 15・paragraph-comment 5・comment-form 1）、後 1 件（comment-form。494 行目の文字列 `"Accept: */*..."` の中の `*/` を comment の終わりと見た誤検出で、直さない） |
| 目で見る規則 | grep と読み | 開発の管理情報（WS・Phase・plan/ の参照）なし、goto なし、`return f(...)` なし、条件の中の関数なし、入れ子の呼び出しの引数なし。成功の return で終わらない関数が 1 つ（`connection_receive`）あり、直した |
| host 試験 | `sh plan/ws343/tests/run-fetch-https.sh` | `run-fetch-https: PASS (18 cases)`（見直しの後） |
| build（3 arch） | `make -j4 CONFIGMK=config/config.<p>.mk build/<p>/packages/fetch/.stamp`（p = x86_64・rpi5・vf2、各 sysroot の後） | 3 つとも exit 0、log の warning 0 |
| QEMU（x86_64、公開の site） | `GUEST_RUNTIME=build/p8-ws343p004-guest sh plan/ws343/tests/run-fetch-qemu.sh` | `run-fetch-qemu: PASS (4 cases)`（example.com・rfc9110・http→https の rfc8446 の SHA-256 一致、自己署名の拒否） |
| diff | `git diff --check` | 空 |

## 直した事（`userland/base/fetch/main.c`）

- blank-after-brace 15 件: 閉じ括弧の後の文の前に空行と目的の comment を足した（`fetch_once`・`send_request`・`read_head`・`read_field`・`parse_url`・`connect_server`・`tls_start`・`connection_send`・`make_redirect` の各所）。
- paragraph-comment 5 件: 初期化の段落（`output_name = NULL;` など）に comment を足した。
- `connection_receive` の終わりが失敗の return だったのを、失敗を先に `if (reason != SSL_ERROR_ZERO_RETURN)` で返し、最後を「Succeeded」の `return 0;` にした。`tls_report` を呼ぶ条件と返す値は同じ。
- 計 21 件（style-check の数）+ 1 件。動作の変更は無い（host 試験・QEMU で確かめた）。

## 範囲の外（Q1 への報告）

- `plan/ws343/tests/passkey-compare.c`（568 行）は style-check で 42 件（call-in-condition 13・paragraph-comment 10・blank-after-brace 10・forward-declaration 5・conditional 4）。直していない。理由: この試験は今の main で既に動かない（`sh plan/ws343/tests/run-passkey-compare.sh` が openssl の build で `compat/crypto/core_names.h` が見つからず FAIL、読む vectors の `build/ws335-vectors` も無い）ので、直しても動作の同値を確かめられない。試験の追従の方針（動かない開発用の試験は消す）に当たるので、消すか直すかは Q1 の判断。消す場合は `run-passkey-compare.sh`・`passkey-compare.c`・`config-amd64-compat.mk`（ct-matrix.sh も同じ組なら）と phase002 の参照。
- 欠陥に見える所は無かった。

## 結果

実装済み・試験 PASS（上の表）。実機は使っていない。aarch64・riscv64 の QEMU は未実施（build だけ）。cleared は Q1。
