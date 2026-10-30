<!-- awesome-plan project=zedbsd record=ws327-p017 -->

# ws327-p017 規約の全文の見直し

- 親: [WS327](../ws.md)。担当: 実装の担当（Sonnet 5.5 high）か conformance-reviewer（Q1 が決める）。依存: p016。
- Status: planned
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

（担当が書く）
