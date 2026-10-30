<!-- awesome-plan project=zedbsd record=ws327-p017 -->

# ws327-p017 規約の全文の見直し

- 親: [WS327](../ws.md)。担当: 実装の担当（Sonnet 5.5 high）か conformance-reviewer（Q1 が決める）。依存: p016。
- Status: cleared の候補（2026-10-30 P5、q1710-i01。下の結果）
- Disposition: normal

## 範囲

WS327 が足した・変えた C の source（`git diff --stat <WS327 の前の main>..<p016 の後の main> -- '*.c' '*.h'` の file、Q1 が SHA を渡す）:

- `userland/desktop/include/keiland/keiland_thread.h`・`keiland_socket.h`、`libkeiland/platform/thread-posix.c`・`socket-posix.c`・
  `thread-win32.c`・`socket-win32.c`
- Mailer（`sync.c`・`conn.c`・`tls.c`・`imap.c`・`mail.h`）、Music（`play.c`・`play.h`）、Video Player（`media.c`・`main.c`・`videoplayer.h`）、
  `media-app/avcodec.c`、Photos（`thumbs.c`・`media-worker.c`）、Settings（`look.c`・`settings.h`・`storage-scan.[ch]`・`storage-trash.[ch]`）、
  IME（`ja-user.c`）、libkeiland（`ui/picker.h`・`ui/picker-model.c`・`audio/audio.c`）
- 解説の例 `docs/howto/keiland-examples/08-worker.c`・`08-language.c`・`08-network.c`
- 試験 `plan/ws327/tests/*.c`、`plan/tools/mail/host-mail-backend.c`・`host-mail-sync.c`

## やる事

- `plan/coding-style.md` の全文（§14 の checklist の全項目）と照らし、**書き方だけ**を直す（動作を変えない）。WS327 が変えた行と、
  その関数の中だけ（file の他の所の古い書き方は直さない）。
- header の comment の持ち越しの直し（実装の Phase の間に見つかった header の comment の誤り。p008〜p016 の結果の「Q1 へ」と Q1 の記録に
  あれば。例: `kl_socket_close(NULL)` は何もしない事、`kl_thread_start` の失敗で `*thread` が NULL）。header の宣言・型・値は変えない（ABI）。
  `sh plan/ws327/tests/check-base.sh p017` はこの Phase だけ 2 つの header の変更を許す。
- source の comment に開発の管理情報（WS・Phase・Bug の番号、plan/ への参照、日付の経緯）が無い事。

## 確かめ

- 直した file の build: zedBSD amd64（`build-zedbsd.sh p017 amd64 "<触った program>"`）、Linux の libkeiland と settings・keiland-ime、
  `sh plan/ws327/tests/build-win32.sh`。全て warning 0。
- `sh plan/ws327/tests/host-test.sh build/ws327-p017-linux`、`sh plan/ws327/tests/host-apps.sh build/ws327-p017-linux ime picker`、
  `sh plan/tools/mail/run-host-mail-backend.sh`、`sh plan/tools/music/host-music-play.sh`、`sh plan/tools/media/run-host-codec.sh`、
  `sh plan/ws314/tests/doc-examples.sh build/ws327-p017-linux/lib`（例を直したら）→ 全て PASS。
- `python3 -I plan/ws327/tests/check-layers.py` → PASS。

## 受け入れ

上が PASS、直した点の一覧を結果に、`git commit -m WIP -- <触った file> plan/ws327/phase017/phase.md`（ws.md は Q1）。

## 結果

P5（q1710-i01、worktree p5-ws327、基点 main 5aca73738）。conformance-reviewer 3 本に file を分けて頼み（同じ worktree、互いに別の file）、
P5 が差分を読んで commit した。日付は host の `date`（2026-10-30）。

### 直した点

- fc4409af7（`platform/thread-posix.c`・`socket-posix.c`・`thread-win32.c`・`socket-win32.c`、2 つの header の comment）:
  入れ子の呼び出しを名前の付いた変数に（`thread_mutex()`・`pthread_self()`・`GetCurrentThread()` など）、条件の中の呼び出し
  （`offsetof`・`LOBYTE`/`HIBYTE`）を外へ、最後の `return error;` を失敗と Succeeded に分けた（`kl_socket_connect`・
  `kl_socket_connect_result`・`socket_wait_connect`・`socket_waker_pair`）、閉じ括弧の後の空行と目的の comment。
  header（宣言・型・値は不変）: `kl_socket_close` に「A NULL socket does nothing.」（両方の層が NULL で早く返る事を確かめた）、
  `kl_thread_start` に「On failure *thread is NULL.」（両方の層が入口で NULL にし成功の時だけ入れる）。
- 2feddde05（Mailer・Music・Video Player・media-app・Photos・Settings・IME・picker・audio の WS327 の行とその関数）: file の先頭の
  comment の WS・Phase・BUG の番号を消した（Mailer の 5 file、music/play.c、videoplayer/media.c、ime/ja-user.c など、WS327 の関数の中の
  BUG-300 の 3 か所も）、`sync_wait` の New mail の字下げと空白（`if (arrived) {…}` を `if (!arrived) continue;` と平らな段落に。
  loop の最後の文なので同じ動き）、critical section の lock の後・unlock の前後の空行、成功の return の comment と順。
- d0a7522cb（例 `08-worker.c`・`08-network.c`、`keiland-programming.md` の写しと行の link、`thread-test.c`・`socket-test.c`・
  `win32-link.c`、`host-mail-backend.c`・`host-mail-sync.c` の WS327 の行）: 条件・引数の中の呼び出しを外へ、`if (error == 0) error = …`
  の連なりを一つずつに、三項演算子を if に、`client_local` を `client_address` に。thread-test.c は初期化の途中の失敗で
  `kl_mutex_destroy` を呼ぶ様になった（試験が通る時は走らない道）。

### 確かめ（2026-10-30 14:00〜14:10）

| command | 結果 |
| --- | --- |
| `sh plan/ws327/tests/build-zedbsd.sh p017 amd64 "mailer music videoplayer photos settings keiland-ime terminal"` | `zedbsd amd64 make=0 warnings=0` |
| Linux `libkeiland.so`・`settings`・`keiland-ime`（`build/ws327-p017{a,b,c}-linux`） | warning 0 |
| `sh plan/ws327/tests/build-win32.sh` | `ws327-win32: PASS`（x86_64・i686 warning 0、link） |
| `sh plan/ws327/tests/host-test.sh build/ws327-p017a-linux`・`…p017c-linux` | `SUMMARY pass=12 fail=0` |
| `sh plan/ws327/tests/host-apps.sh build/ws327-p017b-linux ime picker` | `ws327-host-apps: PASS`（ime 230、picker） |
| `plan/tools/mail/run-host-mail-backend.sh`・`plan/tools/music/host-music-play.sh`・`plan/tools/media/run-host-codec.sh` | 各 PASS |
| `python3 -I plan/ws327/tests/doc-lines.py 08-worker.c 08-network.c 08-language.c` | `ws327-doc-lines: PASS` |
| `sh plan/ws314/tests/doc-examples.sh build/ws327-p017c-linux/lib` | `SUMMARY pass=76 fail=0 not-done=0` |
| `python3 -I plan/ws327/tests/check-layers.py` | `ws327-check-layers: PASS` |
| `sh plan/tools/keiland-os-boundary/check.sh` | FAIL 64 行（p016 と同じ、前からの物） |
| `git diff --check` | 差 0 |

QEMU は p016 の run 3（この Phase の後の tree の image）で。`check-base.sh p017` は流していない: その frozen の一覧が `thread-posix.c`・
`socket-posix.c`・`plan/ws327/tests/` を含み、この Phase の範囲（それらの書き方の直し）と食い違う（実装の Phase 用の道具）。

### 残した物（範囲の外: WS327 が変えていない行）

- WS327 が触れていない file・関数の WS・BUG の番号（mailer の account.c・code.c など、music/play.h の ws177-p021、videoplayer.h、
  videoplayer/main.c の先頭と BUG-300 の 4 か所、settings/look.c・settings.h、photos/thumbs.c の先頭、avcodec.c）。
- `host-mail-backend.c` の古い行の条件の中の `strcmp` など、ja-user.c の古い段落、例の WS327 でない行の `if (error == 0)` の連なり。

## p008 から（2026-10-30 Q1）

- Mailer の file の先頭の comment に WS・Phase の番号が残っている（AGENTS.md §10 に反する）。規約の見直しで消す。`sync.c` の `sync_wait` の "New mail" の辺りの空白・字下げも直す。
- 読む前の poll の待ちの events を変えても捕まえる試験が無い（read の timeout の試験）。p016 で足すかを決める。
