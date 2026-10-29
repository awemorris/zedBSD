<!-- awesome-plan project=zedbsd record=ws314-p009 -->

# ws314-p009 解説に新しい API（post・process・instance・launch・language）の章を足す

- 親: [WS314](../ws.md)。設計: [design.md](../design.md) §3〜§6・§13。Queue: q1690（前半は担当 P6、2026-10-30）。
- Status: cleared（前半の構成と例は 2026-10-30 に済み（host・QEMU）、後半の文章は 2026-10-30 に P6 が書いた）
- Disposition: normal
- ユーザーの決定（ws.md の原文）: 8「…fdの削除後、作成されたAPIに関する説明を追記します。」、11（完全に動く source の全体を示し、
  帰納と演繹で読ませる。構成は Opus、文章は Sonnet）、12・13（`KL_APP_LANGUAGE` と言語の名前）。

## 目的・受け入れ

- 前半（Opus、この記録の「構成」と「確かめ」）: 新しい章と節を設計し、章ごとに完全に動く例を `docs/howto/keiland-examples/` に足し、
  `docs/howto/keiland-programming.md` に例の全文（`<!-- example: ... -->` の印）と書き直しの指示（`<!-- WRITE: ... -->`）を入れる。
  既存の文章は変えない（目次・章のつなぎ・古くなった文に WRITE を足すのは可）。host（compile・link・文書と file の一致）と
  QEMU（zedBSD の guest で各例を動かし画面を撮る）で確かめる。
- 後半（Sonnet）: WRITE に沿って文章を書き、WRITE を消す。

## 構成（前半の設計）

- 章は 7 章の後に 3 つ（決定 8 の「追記」）。各章は p011 の型（例が何をするかと画面 → 例の全文 → 行の link と引用で読む → 規則 → 落とし穴
  と次の章へ）。2 本目の例は 4 章の 04-counter-parts.c と同じく「###」の節。例はどれも 04-counter.c の形（struct・resize・draw・frame・
  input・dirty など）から育て、新しい所だけを説明させる。thread は C11 の `<threads.h>`（zedBSD の libc にある。UNIX の呼び出しを例に
  出さない）。

  | file | 章 | 何をするか | 確かめる API |
  | --- | --- | --- | --- |
  | 08-worker.c | 8 Work on other threads | 2,000 万未満の素数を worker の thread で数える。進みは `kl_window_post`（malloc の報告、受け手が free）、結果は `kl_app_post` で record を返す。Stop は atomic の flag。窓を閉じる時は stop・join・dispatch(0) で残りの post を回収 | `kl_app_post`・`kl_window_post`、`KL_WINDOW_POST`・`KL_APP_POST`（閉じた窓宛ては window NULL） |
  | 08-language.c | 8 の後半 When only the latest answer counts | `KL_APP_LANGUAGE` ごとに世代を進め、reader の thread（mtx・cnd）が `kl_tr_read`、`kl_app_post` で返し、最新の世代だけ `kl_tr_use`、古いのは `kl_tr_discard`。Switch Language は `kl_settings_set_int("ui.language")` で desktop に頼み、変化は event で戻る | `KL_APP_LANGUAGE`・`kl_app_language`・`kl_tr_read/use/discard`（design §13.3 の別 thread の形、決定 13） |
  | 09-sort.c | 9 Other programs | `sort` を INPUT・OUTPUT・ERROR で起こし、単語を書き（短い書きは READY_INPUT で続き）、`close_input`、行で読む（LINE_CUT の続きは捨てる）、stderr、終了の code。Reverse（-r）と Bad Option（stderr と code 2 を見せる） | `kl_process_start/write/close_input/read_line/exit/close` |
  | 09-shell.c | 9 の後半 A program in a terminal | `/bin/sh` を `KL_PROCESS_TERMINAL`（TERM=dumb・PS1）で起こし、byte で読み、制御列を捨てて行に、field の行を書く。窓の大きさで `kl_process_resize`、出力の後に `kl_process_terminal_mode` で echo 無し・行の入力なら field を伏せる。閉じる時は HANGUP | `KL_PROCESS_TERMINAL`・`kl_process_read/resize/terminal_mode/kill` |
  | 10-instance.c | 10 Running once and starting programs | `kl_instance_open` を接続の前に（3 つの結果）、`kl_instance_bind`、`KL_APP_INSTANCE` で `kl_instance_take` を 0 まで、token で `kl_activate`。Start Another は `kl_launch`（argv[0] と要求、from=窓）で自分を起こし、要求が event で戻る | `kl_instance_*`・`KL_APP_INSTANCE`・`kl_launch`・`kl_activate` |

- 7 章（07-services.c）: 翻訳を `kl_settings_open`＋`kl_tr_follow`＋`kl_settings_dispatch` から、`kl_app_language` と `KL_APP_LANGUAGE` の
  `kl_tr_open`（同じ thread の形、design §13.3）に直した。7 章の行の link は差分の対応で機械的に付け替えた（文章は変えていない）。
  「Text in the user's language」の節に書き直しの WRITE（古い引用と存在しない行 157-158 の link を消す、`kl_tr_follow` は kl_app を
  使わない program 向けと一文で、catalog の形式と置き場所）。「The desktop's settings and system」に ui.language と KL_APP_LANGUAGE の
  関係の WRITE。
- 日本語の catalog: `docs/howto/keiland-examples/locale/ja/services.tr`・`language.tr`（文書に ```text の block で全文）。
- 古くなった既存の文（「全ての呼び出しは開いた 1 つの thread から」が 2 か所、1 章の event の kind の表、1 章の落とし穴、目次、例の表、
  build の注意（FreeBSD の -lstdthreads、10-instance の XDG_RUNTIME_DIR））に WRITE を足した。文書の先頭に p009 の全体の WRITE。

## 確かめ

- host（`sh plan/ws314/tests/doc-examples.sh build/p009-linux/lib`、worktree の Linux build
  `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p009-linux build/p009-linux/lib/libkeiland.so`、warning 0）:
  13 本の例を clang・gcc で `-std=gnu17 -Wall -Wextra -Werror`、libkeiland.so に link、catalog 2 つ（`kl_tr_open_directory` で
  全ての msg・plural が訳になる。誤った訳を渡すと FAIL することも確かめた）、文書の block と file の一致 15、marker 15。
- QEMU（`sh plan/ws314/tests/doc-examples.sh guest`、lean な desktop の image（`plan/tools/titlebar/build-menu-image.sh build/amd64`、
  worktree の tree で再 build）、例はその sysroot と libkeiland で zedBSD 向けに -Werror で build、runtime build/p009-run、boot の disk は
  cache=unsafe、SSH の待ち 300 s、catalog は guest の /usr/share/keiland/locale/ja/ へ複写、compositor は `--testing --glass` 1280x800、
  判定は VNC の画面と SSH で読んだ各例の stderr（console・serial の log は読まない））。
- 道具の直し: 1 回目は guest の実行中に同じ worktree で host の確かめを流し、`fresh_out` の link（build/tmp/doc-examples）が新しい run へ
  移って guest の run の出力先が消え、04-counter-parts 以降の画面と program の複写が失敗した（10 FAIL、例の誤りではない）。script は
  自分の run の directory（`$fresh_dir`）を使う形に直した。名前を渡すと一部だけ流せる（`guest BUILD NAME...`）。

## 結果

- host: 2026-10-30 `SUMMARY pass=71 fail=0 not-done=0`。
- QEMU（2026-10-30 2 回目、`build/tmp/doc-examples.run.0Zkifh`）: `SUMMARY pass=29 fail=0 not-done=0`（image・zedBSD の build 13・guest up・
  画面 14）。各例の stderr に失敗の行は無い。画面で確かめた事（`build/review/ws314-doc-NN.png`、切り抜きを
  `docs/howto/keiland-examples/{08-worker,08-language,09-sort,09-shell,10-instance,07-services-ja}.png`）:
  - 01〜07: p011 と同じ画面（07 は KL_APP_LANGUAGE の形に直した後も glass の card・Appearance: Light・0 files opened）。
  - 08-worker: Start の 2.5 s 後に bar が途中、「489319 primes below 7200000 so far.」、Start が無効・Stop が有効（worker の
    `kl_window_post` が届き、窓が描き続けた）。
  - 09-sort: Sort の後に右の列が apple〜pear の順、「sort ended with the code 0.」（stdin の書き・close_input・行の読み・終了の code）。
  - 10-instance: Start Another を 2 回と shell からの `/tmp/10-instance From-the-shell`（exit 0 で受け渡して終わった）の後に一覧が
    「Request 1 from Start Another」「Request 2 from Start Another」「From-the-shell」（`kl_launch` で起きた copy が要求を渡し、
    `KL_APP_INSTANCE` で届いた）。runtime の directory は 0700 の /tmp/run10（compositor の /tmp は他人も書けるので ENOTSUP になる）。
  - 08-language: Switch Language を 200 ms 間隔で 3 回の後、窓が日本語（こんにちは、「…: ja。」、「使ったカタログ: 3。捨てたカタログ: 0。」、
    title 言語）、panel の時計も日本語（desktop の ui.language が変わった）。この run では 3 つの読みが全て次の変更より先に戻った。
  - 07-services-ja: 08-language の後に 07 を起動し、card が日本語（外観: ライト、開いたファイル 0 件、開く...・パスをコピー・通知、
    title サービス、`kl_app_language` と catalog）。
  - 09-shell（3 回目、`build/tmp/doc-examples.run.yiy56w`、09-shell だけ）: 「$ echo hello from sh」「hello from sh」、
    `stty -echo; printf "Password: "; read word` の後に「Password:」、note「The shell turned the echo off…」、field が点 6 つ。
    2 回目は `stty -echo` を prompt で打っただけで、note が出なかった。調べた（診断の stderr の一時の版、元へ戻した）ところ
    `kl_process_terminal_mode` は prompt の間 mode 0 を返す: zedBSD の sh は行の編集の間 ECHO・ICANON を切り自分で echo する
    （p005 の phase.md の記録と同じ。library の誤りではない）。`read` の待ちの間は stty の設定（行のみ）が見える。試験の入力と WRITE を
    これに合わせた。
- 未実施: 窓を閉じる時の後始末の道（08-worker の `primes_finish`、08-language の reader の終わり、09-shell の HANGUP、10-instance の
  `kl_instance_close`。script は program を kill で終える）、08-worker の Stop、09-sort の Reverse と Bad Option（stderr と code）、
  10-instance の `kl_activate` で窓が前へ出ること（窓は 1 つで見分けられない）、08-language で読みが捨てられる場合（この run では 0）、
  Linux の compositor での実行。実機は対象外。

## 後半（2026-10-30 P6）

- `docs/howto/keiland-programming.md` の WRITE を全て文章にし、WRITE の comment を消した。第 8 章（worker と post、言語の読み手）・第 9 章
  （process、terminal の shell）・第 10 章（instance と launch）を、第 1〜7 章と同じ型（説明と画面、program 全体、読み方、規則、落とし穴、
  次章への橋）で書いた。第 7 章「Text in the user's language」は `KL_APP_LANGUAGE` の形に書き直し（`kl_settings_open`・`kl_tr_follow` の
  例と `kl_settings_dispatch` の段落を外し、`kl_tr_follow` は fd の無い program 向けの 1 段落に）、catalog の説明と日本語の画面
  （07-services-ja）を足した。導入（章の一覧・program の表・C11 thread の build・thread の文）、第 1 章（規則・event の表・落とし穴）、
  第 7 章の settings と末尾の文も直した。切り抜きの PNG は WRITE が名指した 6 枚を入れた。
- 引用と行の link は file から生成した（手で写していない）。code の block と keiland-examples/ の file は変えていない。
- 確かめた事: `sh plan/ws314/tests/doc-examples.sh build/p009-linux/lib` が `SUMMARY pass=71 fail=0 not-done=0`、
  `python3 -I plan/tools/record-audit/links.py` が `TOTAL 0 broken`。WRITE と OLD TEXT の comment は 0。
- 気付き（例の code は変えていない）: 08-worker.c の `primes_finish` は worker を `thrd_join` した後、残りの `POST_FINISHED` を
  `primes_post` に渡し、そこでもう一度 `thrd_join` する（同じ thread を 2 回 join する）。本文はこの道に触れていない。

## 残り

- なし（p009 は cleared）。
