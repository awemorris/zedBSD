<!-- awesome-plan project=zedbsd record=ws314 -->

# WS314 libkeiland の公開の API を component ごとの header に分け、プログラミングの解説を書き、fd と UNIX への依存を API から外す

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: completed の判定待ち（Q1）（2026-10-30 Q1 が作成、q1690 で P2 が p001 から実行。全 Phase が cleared）
- 目標: (1) `userland/desktop/include/keiland/keiland.h` の定義を component ごとの `keiland_*.h` に分け、`keiland.h` はそれらを include する入口にする。
  (2) Keiland のプログラミングの解説（英語）を `docs/` に書く。(3) app・desktop の program が fd・pipe・socket・fork に触れない API に置き換え、
  `kl_app_watch_fd()` と `kl_instance_fd()` を公開の API から外す。その後に解説へ新しい API を足す。

## ユーザーの決定（2026-10-30、原文）

1. 「fdはWindowsに移植できないので、このインタフェースは許容できないです。Mailer, Installerは、kl_process_みたいなサブプロセスAPIで抽象化したいです。」
2. 「Mailerに関しては、appにイベントをポストするAPIを導入すれば、fdやパイプのメカニズムを削除できますね。同様にwindowにイベントをポストするAPIも、この件では必須ではないものの、普通に考えればあった方がいいです。」
3. 「Installerに関しては、kl_process_*を導入するのがいいです。Terminalではどうなってますか？」
4. 「Settingsも、kl_instance_*からfdのAPIを削除して、instanceを指定してそのappにイベントをポスト可能にするのがいいです。」
5. 「Browserに関して、ネットワークI/OはすべてKeiland非依存のlibbrowserに閉じ込める必要があります。KeilandのBrowserは、libbrowserのシェルに過ぎない、というのが設計意図です。直接にシェルがネットワーク接続やsocketやfdを触ってはいけません。」
6. 「Keilandデスクトップから、UNIX依存を削除する必要があります。それは設計意図から逸脱しています。」
7. 「libkeilandについて提案していただいたことをすべて承認します。」（下の「承認された案」）
8. 「include/keiland/keiland.hにすべての定義が入っていますが、include/keiland/keiland_*.hに、それぞれのkl_xxxのコンポーネントの定義を、分割して移したいです。トップレベルはkeiland.hのままで、そこからincludeされます。さらに、kl_uiのimmediate mode UIのプログラミングモデルを、docs/にドキュメント化してほしいです。Keilandのプログラミングの解説ドキュメントです。appの作成、windowの作成、タイトルバーの作成、uiの作成とメインループ、widgetの作成、メニューの作成、その他の機能の使い方、の順で、人間が呼んでも理解しやすい構成にしてほしいです。英語で記述してください。そのあとで、fdを削除するという、先ほどまでの合意事項のリファクタリングを行います。ドキュメントには、fdのAPIのことは書かないで進めておき、fdの削除後、作成されたAPIに関する説明を追記します。」
9. 「以上のことを、WSを立てて、サブエージェントですすめてほしいです。」
10. （Q1 経由）「ws314のp001で、コメントから開発情報を削除してほしいです。」

11. 「keiland-programming.mdをレビューしました。まず、コードスニペットがわかりづらかったです。スニペットではなく、完全に動くソースファイル全体をなるべく毎回示すのがいいと思います。また、そのせいでkl_uiの説明がぜんぜん理解できなかったです。kl_buttonのスニペットしか示されていないのに、kl_uiがどういうものであるかを説明していて、これは意味不明でした。kl_uiのサンプル全体をまず示して、その中でkl_buttonがどういう風に使われているのかを示し、全体としてkl_uiやフレーム、immediateモードという概念を、どう使うのかというサンプルから、帰納的に理解しつつ、演繹的な説明を参照できるように、書き換えていきたいです。一方で、Opus 5.5はこのようなドキュメント作成が苦手です。そこで、このようなサンプル全文を提示して帰納的な理解を促しつつ演繹的な説明も行うスタイルを、Opus 5.5がドキュメント概要として設計し、keiland-programming.mdに、ここにこういう説明を入れる、というコメントを入れていってほしいです。そのあとで、Sonnetで書き直しを行います。Sonnetは人間向けの文章が得意だからです。」（2026-10-30）
12. 「kl_appに、システムの言語の変更を通知するイベントを追加してほしいです。KeilandのリファクタのWSに追加しておいてください。」（2026-10-30）
13. 「kl_appの言語変更イベントは、変更された言語名も通知するのがいいです。連続する変更が短時間にあると、kl_settingsの読みが最新とは限らないからです。」（2026-10-30、p015 に効く）

## 承認された案（2026-10-30 Q1 の提案、上の 7 で承認）

- **app・window への event の post**: thread から呼べる `kl_app_post`（と `kl_window_post`）。Mailer の pipe 2 本を外す。起こす仕組みは platform の層に閉じる。
- **`kl_process_*`**: 起動（argv・env・cwd）、stdin への書き込み、stdout・stderr の受け取り（byte・行）、終了の event と code、kill、pty の mode と大きさ
  （Terminal、Windows なら ConPTY）。Installer・Terminal と、別の program を起動する Files・Preview・PDF Viewer・Phone・Settings を寄せる
  （高い水準の起動の API `kl_launch` の類も検討）。権限を上げる起動を API に含めるかは設計で示しユーザーに聞く。
- **`kl_instance`**: instance を app に結び、後の起動の要求を `kl_app` の event（`KL_APP_INSTANCE` の類）で届ける。`kl_instance_fd()` を外す。
- **Browser**: libbrowser が自分の network の thread を持ち、結果は「起こしてほしい」callback で呼ぶ側に知らせる（shell は中で `kl_app_post`）。
  shell から `pollfd`・socket・fd を消す（DNS の thread と media の engine の起こしも同じ callback に）。
- 範囲: app と libkeiland の公開の API から UNIX の依存を外す。compositor・`libkeiland-backend-*`・sessiond は platform の層として残す（Q1 の解釈）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws314-p001](phase001/phase.md) | `keiland.h` を `keiland_*.h` に分ける（定義の移動だけ、API・ABI は変えない。KL_VERSION の履歴の comment の置き場所も）。comment から開発の管理情報を除く（決定 10） | cleared | — |
| [ws314-p002](phase002/phase.md) | `docs/` に Keiland のプログラミングの解説（英語）: app → window → titlebar → ui と main loop → widget → menu → その他。fd の API は書かない | cleared | p001 |
| [ws314-p003](phase003/phase.md) | 設計: post・`kl_process`・`kl_instance` の event・libbrowser の network の thread（[design.md](design.md)、design-reviewer） | cleared（判断 A〜D 待ち） | p001 |
| [ws314-p004](phase004/phase.md) | `kl_app_post`・`kl_window_post` の実装と Mailer の移行 | cleared（QEMU の Mailer は未実施） | p003 |
| [ws314-p005](phase005/phase.md) | `kl_process_*` の核と Installer・Terminal・Phone の移行（design.md §4） | cleared（Installer・Phone の QEMU は未実施） | p003・p004 |
| [ws314-p006](phase006/phase.md) | `kl_instance` の event と Settings の移行 | cleared（Settings の 2 回目の起動の QEMU は未実施） | p004 |
| [ws314-p007](phase007/phase.md) | libbrowser の network の thread と Browser の shell の移行（BROWSER_API_VERSION 3） | cleared | p004 |
| [ws314-p008](phase008/phase.md) | `kl_app_watch_fd`・`kl_instance_fd` を公開の API から外す（KL_VERSION 102）、Linux・FreeBSD の build | cleared | p004〜p007・p013・p014 |
| [ws314-p009](phase009/phase.md) | 解説に新しい API（post・process・instance・launch・language）を足す | cleared | p008・p012 |
| [ws314-p010](phase010/phase.md) | 規約の全文の見直し | cleared | p009 |
| ws314-p011 | 解説の構成の設計（Opus）: 章ごとに完全に動く source の全文を示し、そこから帰納的に分からせ演繹的な説明も引ける構成。keiland-programming.md に「ここにこういう説明を入れる」の comment を入れ、例の全文は compile・実行で確かめる | cleared | p002 |
| ws314-p012 | 解説の書き直し（Sonnet）: p011 の構成と comment に沿って人間向けの文章に | cleared | p011 |
| [ws314-p013](phase013/phase.md) | `kl_launch` と Files・Settings・PDF Viewer・Terminal の新しい窓の移行（design.md §5） | cleared | p005 |
| [ws314-p014](phase014/phase.md) | Preview の API から fd・pid を外し、`waitpid`・`kill` を Preview の platform の file へ（design.md §4.5） | cleared | p003 |
| [ws314-p015](phase015/phase.md) | kl_app に system の言語の変更を知らせる event（`KL_APP_LANGUAGE` の類）を足す。event は変わった先の言語の名前（"ja" など）を運ぶ（決定 13）。今の kl_settings の読み（kl_settings_dispatch）との関係、translation（kl_tr）の読み直しの手順、解説への記述（p009 と一緒に） | cleared（解説は p009） | p004 |

## Resume point

- 2026-10-30 Q1: 作成。P2 が q1690 で p001 から。
- 2026-10-30 P2: p001 cleared（73f0ec352、30 の header、zedBSD 3 arch・Linux の前後の symbol が同じ）。p002 cleared（5fe9561c5、
  docs/howto/keiland-programming.md）。
- 再開点（2026-10-30 P2、context の残りが少ないため返却）: p003（設計）から。未着手。手がかり:
  fd の API の利用者は担当の範囲の中だけ（mailer/main.c・sync.c、installer/main.c・helper.c、settings/main.c・window.c、
  browser/shell/window.c、terminal/window.c、と libkeiland の instance.c・ui/app.c・ui/window.h）。`kl_instance_*` の利用者は settings だけ。
  pipe・fork/exec・socket・poll の利用（file の数）: mailer pipe 1・socket 1・poll 2、installer pipe 2・exec 2・poll 1、terminal exec 1、
  settings exec 1、browser poll 2、libbrowser pipe 1・socket 2・poll 6、files exec 1、preview exec 2・socket 1、pdfviewer exec 1、phone exec 1。
  既存の `kl_window_post(window, code)`（KL_WINDOW_POST、dispatch の中で同じ thread から入力を積む）がある。`kl_app_post` は thread から呼べる別物として設計する。
- 2026-10-30 P2（再開）: p003 cleared（[design.md](design.md)、design-reviewer 23 件を反映）。ユーザーの判断 A〜D を Q1 へ（phase003 の phase.md）。
  p004 を実装中（libkeiland の post と Mailer、host の試験 tests/post-test.c は PASS）。
- 2026-10-30 P2: p004 cleared（KL_VERSION 97）。p005 は実装・build（3 arch・Linux、warning 0）・Linux の host の試験まで（KL_VERSION 98）。
  残り: p005 の zedBSD（QEMU）の試験、`keiland-os-boundary` の許可の行（Q1）。
- 2026-10-30 P2: p006 実装・build（3 arch・Linux、warning 0）・host の試験（KL_VERSION 99）。p005・p006 の zedBSD の試験は `plan/ws314/tests/guest.sh`。
- 再開点（2026-10-30 P2、context の残りが少ないため返却）: p003〜p006 cleared（[design.md](design.md) §13 に p015 の設計を足した）。
  次は p013（`kl_launch`: platform/ に二重の fork の起動（親で path・argv・envp・token を作り、子は async-signal-safe だけ、design §5）、
  Files の `apps.c`・Settings の `posix_spawn(files)`・PDF Viewer の `notes` と `waitpid(-1)`・Terminal の新しい窓。終わったら
  `plan/tools/keiland-os-boundary/app-allow.tsv` の terminal・settings・pdfviewer・files の A4 の行を外す）、p014（Preview の API）、p015
  （`KL_APP_LANGUAGE`、design §13、KL_VERSION 100 の見込み。Q1 に番号を確かめる）、p007（libbrowser、design §7）、p008、p009（p012 の後）、p010。
  手がかり: 実装は `libkeiland/platform/`（platform.h・platform-posix.c・process-posix.c）と `libkeiland/process.c`・`instance.c`・`ui/app.c`。
  試験: `plan/ws314/tests/host-test.sh {post,process,instance} BUILD`（Linux の build、TSan は build dir の名前に tsan）、zedBSD は
  `plan/ws314/tests/guest.sh OUT IMGBUILD`（image は `config-amd64-guest.mk`）、build は `build-zedbsd.sh NAME ARCH "PROGRAMS"`
  （riscv64 の sysroot の header が古いまま残る時は header を touch して再 build）。判断 A〜D はユーザーの返事待ち（B は推しの形で実装済み）。
- 2026-10-30 P2（q1690-i02）: p013 cleared（d5659a603、`kl_launch`、KL_VERSION 100）、p014 cleared（ea131385d、Preview の API から fd・pid）、
  p015 cleared（`KL_APP_LANGUAGE`・`kl_app_language`、KL_VERSION 101）。QEMU は `guest.sh build/p2-q/run9` で 3 つまとめて PASS。
- 再開点（2026-10-30 P2、context の残りが少ないため返却）: 次は p007（libbrowser の network の thread、design §7、BROWSER_API_VERSION 3）。
  未着手。手がかり: shell の利用は `browser/shell/shell.c:306` の `browser_view_timeout` と `shell/window.c` の watch、libbrowser の API は
  `include/browser/browser.h:266-268`（`browser_view_poll_fds`・`_timeout`・`_process`）と `libbrowser/view/view.c:637-702`、network は
  `libbrowser/net/loader.c`（1763 行）・`http.c`・`tls.c`・`cookie.c`・`host.c`、`page/network.c`・`page/media.c`。その後 p008（KL_VERSION は
  102 から、Q1 に確かめる）、p009（post・process・instance・launch・language の節、`plan/ws314/tests/doc-examples.sh`）、p010。
  試験の道具: `plan/ws314/tests/guest.sh OUT IMGBUILD`（--glass、image に files・textedit）、host は `host-test.sh {post,process,launch,language,instance}`
  と `preview-test.sh`。判断 A〜D はユーザーの返事待ち。
- 2026-10-30 P2（q1690-i02）: p007 cleared（libbrowser の loader に `browser-net`・`browser-resolve` の thread、`browser_wake`、shell は
  `kl_app_post`。host（TSan も）・QEMU `guest-browser.sh` PASS）。次は p008（KL_VERSION 102 から、Q1 に確かめる）→ p009 → p010。
- 2026-10-30 P2（q1690-i02）: p008 cleared（KL_VERSION 102、fd の API を公開から外した。zedBSD 3 arch・Linux・FreeBSD（backend-test 全 PASS）・
  QEMU `guest.sh`・`guest-browser.sh` PASS）。次は p009（解説に post・process・launch・instance・language の節）→ p010。
- 再開点（2026-10-30 P2、context の残りが少ないため返却）: p009 は未着手。手がかり: `docs/howto/keiland-programming.md`（4311 行、章 1〜7、
  各章は「完全な例の全文 → Reading the program（行の link）→ The rules → Pitfalls」、例は `docs/howto/keiland-examples/NN-*.c` と PNG、
  冒頭の章の一覧と「The programs」の表も足す）。足す節: post（`kl_app_post`・`kl_window_post`、worker の thread）、process（`kl_process_*`、
  行・byte・終了・terminal の mode）、launch（`kl_launch`）、instance（`kl_instance_open`・`kl_instance_bind`・`KL_APP_INSTANCE`・`kl_instance_take`）、
  language（`KL_APP_LANGUAGE`・`kl_app_language`、design §13.3 の翻訳の読み直しの 2 つの形、7 章の「Text in the user's language」と繋ぐ）。
  確かめは `sh plan/ws314/tests/doc-examples.sh build/ws314-p008-linux/lib`（compile・link・本文と file の一致）と `doc-examples.sh guest`（画面）。
  その後 p010（conformance-reviewer、WS の変えた C の全文）。
