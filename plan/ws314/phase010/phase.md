<!-- awesome-plan project=zedbsd record=ws314-p010 -->

# ws314-p010 規約の全文の見直し

- 親: [WS314](../ws.md)。Queue: 規約の見直しの割当（担当 P2、2026-10-30）。
- Status: cleared（2026-10-30。書き方だけを直し、build・host 試験で動作が同じことを確かめた。残りは下の「残り」）
- Disposition: normal

## 目的・受け入れ

WS314 が変えた C を `plan/coding-style.md` の全文と照らし、書き方だけを直す（動作・API・ABI・error の値を変えない）。
source の comment の開発の管理情報を消す（AGENTS.md §10）。build warning 0（zedBSD 3 arch・Linux）、host 試験、`doc-examples.sh`。

## 対象

WS314 の commit（`git log c910bba8c~1..HEAD -- plan/ws314` の 27 commit）が変えた C の file（66 file、`keiland_*.h` を除く）と
`docs/howto/keiland-examples/*.c`。WS314 の行は `git blame` で WS314 の commit に当たる行を取り、`plan/tools/style-check.py` の
検出をその行に絞って数えた。

## 直した事

- `libkeiland/platform/process-posix.c`（新規 1276 行）:
  - 条件の中の関数の呼び出し 8 を外した（`strchr`・`strncmp`・`keiui_stream_room` 4・`process_now_ms` 2・`read`）。
    値を名前の付いた変数に取り、その変数を検べる形にした（`slash`・`same`・`output_room`・`error_room`・`now`・`got`・`room`）。
    `keiui_stream_room` は stream の残りの大きさだけに依り、同じ lock の間で変わらないので、読む位置を 1 か所にしても同じ。
  - `process_spawn` の `goto fail`（4 か所）を、後始末の関数 `process_spawn_cleanup` を呼んで return する形に置いた。順序は同じ。
  - `process_find` の最後が失敗の return だった所を、見つけた時に loop を抜け、失敗を先に返し最後に成功の return で終わる形にした。
    以前に EACCES を覚えた後で見つかれば 0 を返す動作は同じ（見つけた所で `error = 0`）。
  - 閉じ括弧の直後の文 16 に空行と comment を足した（`keiui_child_start` の後始末、`process_environment` の copy、`process_child_*` の
    `closefrom`、`process_io_run` の pipe の組み立て、など）。boolean を式で作っていた `full` を `if` / `else if` にした
    （`&&`・`||` の短絡の順は同じ）。
- `libkeiland/process.c`: 閉じ括弧の直後の文 3 に空行と comment。
- `libkeiland/platform/platform-posix.c`: `keiui_thread_self` の `return pthread_self();` を変数に取る形に。
- `libkeiland/ui/app.c`: 閉じ括弧の直後の文 9 に空行と comment。`keiui_app_slot` の `return app_slot(app);` を変数に取る形に。
- `libkeiland/instance.c`: 閉じ括弧の直後の文 3 に空行と comment。
- `libbrowser/net/loader.c`: 閉じ括弧の直後の文 2 に空行と comment。
- `preview/client.c`: 閉じ括弧の直後の文 1。
- comment の開発の管理情報を消した: `libbrowser/net/loader.c`（`plan/ws074/design.md §9`・`ws074-p058`）、`libbrowser/page/network.c`
  （`plan/ws074/design.md §19, ws074-p053`）、`libkeiland/instance.c`（`ws089-p016`）。
- `docs/howto/keiland-examples/*.c`（11 file）: 閉じ括弧の直後の文 60 に空行と一行の comment を足し、条件の中の `tolower` 2 と
  `strcmp` 1 を変数に取った（`name_letter`・`text_letter`・`japanese`）。文書の埋め込み（11 file の ```c の block）を file と byte で合わせ、
  文書の `#L` の link 全て（本文の番号も）を、足した行の分だけずらした。文書の抜粋（`08-language.c` の `strcmp`）も合わせた。

## style-check の件数（対象の file の合計、前は `8dbbd75be`）

| 検出 | 前 | 後 |
| --- | --- | --- |
| blank-after-brace | 75 | 2 |
| call-in-condition | 11 | 1 |
| goto | 4 | 0 |
| conditional | 1 | 1 |
| paragraph-comment | 59 | 56 |

後の blank-after-brace 2・call-in-condition 1・conditional 1 は WS314 以前からの行（他の WS の既存の違反。下）。
paragraph-comment の 56 のうち 28 は `keiui_lock_take` / `keiui_lock_give` が作る critical section の空行で、規約（§5）が
comment なしで認める形。`style-check.py` の critical section の検出が `keiui_lock_take` を知らないための誤検出
（`plan/tools/` は担当の範囲外なので直さない。Q1 へ: `CRITICAL_ACQUIRE`・`CRITICAL_RELEASE` に `keiui_lock_take`・`keiui_lock_give`
を足せば消える）。残りは WS314 の行ではない既存の行。

## 確かめ

- Linux: `make -j32 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws314-p010-linux all` make 0、warning 0。
- zedBSD: `build/ws314-p010-build-all.sh`: amd64（desktop 19 + tests 9）・arm64・riscv64（desktop 19）・installer の image の installer: 全て make 0、warning 0。
- host 試験（`build/ws314-p010-linux`）: `host-test.sh` post（PASS posts=21000）・process・launch・language・instance（各 PASS failures=0）、
  `preview-test.sh`（PASS failures=0）、`loader-test.sh`（loader-test: ok）。
- `doc-examples.sh build/ws314-p010-linux/lib`: SUMMARY pass=71 fail=0 not-done=0（clang・gcc の `-Wall -Wextra -Werror`、link、文書との一致）。
- `git diff --check`: 出力なし。

## 未実施

- QEMU（zedBSD）・実機の回帰（Q1 に頼む）。`doc-examples.sh guest` の写真（comment と空行だけの変更なので未実施）。
- FreeBSD の build（host に無い。`process-posix.c` の FreeBSD の枝は直した部分に触れない）。

## 他の WS の既存の違反（直していない、記録のみ）

- `userland/desktop/libbrowser/net/loader.c`: `conditional`（`?:`）1 と `call-in-condition` 1（WS074 の行）。
- `userland/desktop/browser/shell/internal.h`・`shell.c` ほか、`libkeiland/instance.c` の WS089 の行などの comment に `WS169 p005`・`ws081-p006`・
  `ws189-p003` など開発の管理情報が多数ある（AGENTS.md §10、規則: 開発の管理情報）。
- `style-check.py` が出す paragraph-comment の既存分（上の 56 のうち WS314 の行以外）。

## 残り・再開点

- 機械で見える規則は WS314 の行について全て直した。機械で見えない規則（「comment が文を言い直すだけ」「関数呼び出しごとの comment」「引数の中の
  入れ子の呼び出し」）は、`process-posix.c`・`process.c`・`platform-posix.c`・`launch.c`・`app.c` の WS314 の関数を全て読んで確かめた。
  mailer・installer・terminal・settings・files・phone・pdfviewer・imageview・browser shell・preview の WS314 の hunk は style-check と
  grep（`return f(...)`、入れ子の呼び出し、`?:`、各関数の最後の `return error;`）で確かめたが、全文は読んでいない。
