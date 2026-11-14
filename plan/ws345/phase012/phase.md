<!-- awesome-plan project=zedbsd record=ws345-p012 -->

# ws345-p012 規約の全文の見直し

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-15 P3 q2131、Q1 の受け入れ待ち）

## 目的

WS345 が変えた C の code を plan/coding-style.md の全文と照らして読み、書き方だけを直す（AGENTS §2、conformance-reviewer）。

## 試験

- host-build warning 0、p011 の 1 の受け入れの試験と回帰が前と同じ。

## 受け入れ

- 見直しの記録（直した所・直さなかった理由）と回帰が前と同じ。

## 結果（2026-11-15 P3 q2131-i01）

範囲: WS345 が変えた C（libbrowser の `view/`・`page/`・`bind/`・`base/`・`net/`、browser の `main.c` と `shell/`、`userland/tests/browser-threads/`、`plan/tools/browser-component/client.c`）のうち、WS345 の開始（ws.md を作った commit 4249e9a927）から今までの差分の行。commit の message が全て `WIP` なので Phase ごとには分けられず、その差分の行に `plan/tools/style-check.py` を当てて出た物を直した（他の WS が同じ file に入れた行も差分に含まれる）。

### 直した所

| 規則 | 件数 | 例 |
| --- | --- | --- |
| 条件の中の関数の呼び出し（§6） | 約 40 | `bind_window_runner`・`bind_thread_runner`・`strcmp`・`__atomic_load_n`・`__atomic_compare_exchange_n`・`net_loader_client_*`・`page_*` を先に変数へ。短絡の順は保った（呼ばれる条件の中に入れて残した） |
| `}` の後の空行と comment（§5） | 約 60 | front.c・remote.c・frames.c・frame-load.c ほか |
| 条件演算子（§6） | 約 10 | 二つの引数に入れ子で使う物・return の物・文を分ける物を if に。短い対称の物（`width > 0 ? w : 300` など）は規約の例外として残した |
| 関数の末尾が `return error;`（§11） | 2 | `remote_up`・`view_remotes_message` の後ろに失敗と成功を分けた |
| 直の `return f(...)`（§11） | 2 | `frame_load_activate`・`page_remote_loaded` |
| 引数の入れ子の呼び出し（§8） | 4 | `paint_software(paint_drawing_list(...))`・`view_remotes_key(view_frames_remotes(view_front_frames(...)))` ほか |
| comment の中の開発の管理情報 | 1 | view.c の `J6 of the design` を削除。WS345 の他の源にある WS・Phase・Queue の番号・plan/ への参照は無かった（他の WS の `ws074-pNNN` などは範囲外で未変更） |

動作・API・ABI は変えていない。変えた file は `userland/desktop/libbrowser/{base/once.c,base/thread.c,bind/timer.c,page/frame-load.c,page/images.c,page/network.c,page/script.c,view/front.c,view/frames.c,view/remote.c,view/style.c,view/view.c}`、`userland/desktop/browser/main.c`、`userland/desktop/browser/shell/{latency.c,render.c,shell.c}`、`plan/tools/browser-component/client.c`。

### 直さなかった物

- `goto release_roots`（frame-load.c、前方の 1 つの cleanup label への jump で規約の例外）。
- 短い対称の条件演算子（remote.c の `width`・`height`・`value` ほか）。
- main.c の lock の間の `paragraph-comment` の 6 件（`page->table->mutex_lock` の関数 pointer を style-check が lock と見ないための誤検出。規約の臨界区間の形になっている）。
- 他の WS の既存の違反（差分の外。view.c の `page_sheets_blocking` ほか約 15 件）は記録だけ: `python3 plan/tools/style-check.py userland/desktop/libbrowser/view/view.c` で一覧が出る。
- 1 file ごとの全文の読みは、量（libbrowser の対象 file 約 50、計 7 万行）のため、機械の検査の出た所と WS345 の新しい file（front.c・frames.c・remote.c・style.c・thread.c・once.c・shell の threads.c・render.c・latency.c・threads-posix.c）の抜き読みに留めた。comment の中身（restate）・関数の形の全面の見直しは未実施。

### 試験

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`（gcc、`-Wall -Wextra -Werror`） | exit 0、warning 0 |
| gcc・clang（main の checkout の `build/llvm/bin/clang` を読み取りで使用）の `-fsyntax-only -Wall -Wextra -Werror` で shell の render.c・shell.c・latency.c・threads.c と直した libbrowser の file、`plan/tools/browser-component/client.c` | 出力無し |
| host-threads-table / host-loader-client / host-terminate / host-frames / host-threaded-view（plain） | 16 / 23 / 39 of 39 / 33 / 81、失敗 0 |
| `sh plan/ws345/tests/threaded-latency.sh plain` | `threaded-latency: PASS (10 scenarios, plain)` |
| `sh plan/ws345/tests/host-prompt-rule.sh` | 13 checks, 0 failed |

### 未実施

- `make -j4 CONFIGMK=config/config.{x86_64,rpi5,vf2}.mk build/<機種>/packages/browser/.stamp`: この worktree に `build/llvm` が無く（toolchain の build・install は Q1 の許可が要るので行わず）、`entry-amd64.S` の clang で止まった（3 機種とも）。代わりに上の host と clang の `-Werror` を流した。3 機種の build は Q1 が main か toolchain のある worktree で。
- tsan・asan の変種、QEMU・Wine・実機（見直しは書き方だけで、前の Phase の結果から変わる所は無い。Q1 の判断で要れば）。
- 消してほしい物（Q1 へ）: worktree 直下の `build-host.log`・`build-zed.log`（untracked、commit していない）。
