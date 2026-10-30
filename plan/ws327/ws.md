<!-- awesome-plan project=zedbsd record=ws327 -->

# WS327 libkeiland の thread と network の薄い wrapper（POSIX と Win32 で同じ interface）と app の移行

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: completed（2026-10-30 P5 が完了の形に書いた。Q1 の確認と master.md の行の移動を待つ）
- 目標: libkeiland に、pthread の類を包む thread の層と、socket の概念を見せる network の層を足し、Keiland の app の thread と socket をそれに移す。POSIX（zedBSD・Linux・FreeBSD）と Win32 で同じ interface で実装できる形。libbrowser は使わない。
- 設計: [design.md](design.md)（§5.4 の Win32 の表は Future Work F-WS327-win32-build が参照する）。

## ユーザーの決定（2026-10-30、原文）

1. 「また、libkeilandに、スレッド、ネットワークのごく薄いラッパーを入れたいです。スレッドはpthreadのラッパー的なもので、Win32でも同じインタフェースで使えればOKです。ネットワークはソケットのラッパーで、ソケットという概念を晒してよくて、BSDソケットでもWinSockでも実装できるものとし、複数のソケットからからのpoll/selectをインタフェースで提供します。できそうですか？」
2. 「WS327を立ててOKです。appの対応も入れます。libbrowserは単体のライブラリなので、このラッパーは不使用です。」
3. （2026-10-30、Q1 経由）「P2ですが、Windows用の実装を書いてしまってOKです。インタフェースがWindowsでも通用する形になっているかのチェックです。動作確認は不要です。mingwは使ってよいです。」
   - 解釈（Q1）: `thread-win32.c`・`socket-win32.c` を書き、`x86_64-w64-mingw32-gcc` で cross compile（warning 0）まで。wine での試験はしない。写せない所・口を変えた方がよい所は design.md に書いて Q1 に返す。p005 の後の p006。
4. （2026-10-30）「P2をいったんラップアップして、標準廉価実装手順を適用するようにWS237のphaseを整理しなおして、再起動してほしいです。」
   - 解釈（Q1）: WS237 は完了済みの i915 の WS なので WS327 の事。Phase を設計の担当（P2）と実装の担当（Sonnet 5.5 high）に分けた（p007〜p017）。
5. （2026-10-30）「WS327をP5で実行します。P5はこのWSが終わったら返却してください。」（p016 の残り・p017・完了の形）

## 結果

- **層**: `userland/desktop/include/keiland/keiland_thread.h`（thread の作成・join・detach、mutex・条件変数・once、thread ごとの値、
  名前・優先度・CPU、sleep・時計、KL_VERSION 103）と `keiland_socket.h`（不透明な socket、IPv4・IPv6 の address、名前の解決、
  非 blocking、connect の結果、複数の socket と waker の `kl_socket_poll`、KL_VERSION 104）。実装は `libkeiland/platform/`
  `thread-posix.c`・`socket-posix.c`（zedBSD・Linux・FreeBSD）と `thread-win32.c`・`socket-win32.c`（mingw x86_64・i686 で compile と link、
  warning 0、走らせていない: ユーザーの決定 3）。Win32 で header の約束が揺れる所は design §5.4（W1〜W8・S1〜S9）、header を変える必要は無い。
- **app の移行**: Mailer（conn・tls・imap・sync）、Music、Video Player と media-app、Photos（thumbs・media-worker）、Settings（look・
  storage-scan・storage-trash）、IME の辞書の writer、libkeiland の picker-model・audio。app と libkeiland の platform/ の外は pthread・
  socket を直に呼ばない: OS 境界の checker に T1・N1 を足した（`plan/tools/keiland-os-boundary/check.sh`、FAIL は WS327 の前と同じ 64 行）。
- **解説**: `docs/howto/keiland-programming.md` の 8 章（08-worker・08-language を新しい層に、network の節と `08-network.c`、
  `08-network.png`）。
- **確かめ**（p016・p017）: zedBSD amd64・arm64・riscv64 と Linux の build warning 0、host の thread 27（28）・socket 53 を gcc・clang の
  plain・TSan・ASan+UBSan で、FreeBSD 15.1 の guest で thread 27・socket 53、host の ime・picker・mail・music・codec、doc-examples 76、
  QEMU（音の device 無し）の app の回帰（Mail・Video Player・Settings・Terminal・IME・壁紙・WS327 の 4 項目、8 章の例 5 つの絵）。
  詳細は phase016・phase017 の結果（Phase の directory の削除の後は git の履歴）。
- p017 で WS327 が変えた C の行を coding-style の全文と照らして直した（header は comment だけ: `kl_socket_close(NULL)` は何もしない、
  `kl_thread_start` の失敗で `*thread` は NULL）。

## 制限・移管

- Windows の build（Makefile・DLL）と走らせての確かめは範囲の外: Future Work F-WS327-win32-build。
- Music の音ありの QEMU は p016 の run 1・2（P2）で通ったのが最後。run 3 は音の device 無し（BUG-368 の irq.c の fatal が intel-hda 付きで
  1 回出たため、open）。
- apps.photos.browse の R の後の FAIL は BUG-370（Photos の自分の保存で全面表示が閉じる、WS327 の外）。その helper と仕様は今の
  mediastorage に合わせて書き直した（`plan/tools/aat/scenarios/helpers_photos.py`、`tests/scenarios/apps/photos/browse.md`）。
- BUG-367（loopback TCP の close）は WS327 の中で見つかり閉じた。zedBSD の image に `/etc/hosts` が無い（"localhost" が解けない、p003）は
  Q1 の判断の待ち。
- 人の確かめが要る写真（needs-person 8 項目）は `build/ws327-p016-apps3/*/png/`（p5-ws327 の worktree）。

## Phase

| Phase | 目的 | 結果 |
| --- | --- | --- |
| ws327-p001 | 設計（design.md）、design-reviewer | cleared |
| ws327-p002 | thread の層（`thread-posix.c`）と host 試験 | cleared |
| ws327-p003 | network の層（`socket-posix.c`）と host 試験、zedBSD の guest | cleared |
| ws327-p004 | app の移行（一括） | canceled（p008〜p012 に分けた） |
| ws327-p005 | 解説と規約の見直し（一括） | canceled（p015・p017） |
| ws327-p006 | Win32 の実装（一括） | canceled（p013・p014） |
| ws327-p007 | 委譲の設計（標準廉価実装手順） | cleared |
| ws327-p008 | Mailer の移行 | cleared |
| ws327-p009 | Music・Video Player・media-app | cleared |
| ws327-p010 | Photos | cleared |
| ws327-p011 | Settings | cleared |
| ws327-p012 | IME・picker-model・audio | cleared |
| ws327-p013 | `thread-win32.c`（mingw の compile） | cleared |
| ws327-p014 | `socket-win32.c`（mingw の compile） | cleared |
| ws327-p015 | 解説（8 章） | cleared |
| ws327-p016 | 受け入れ（境界・build・host・FreeBSD・Win32・QEMU） | cleared の候補（2026-10-30 P5。FAIL は Q1 が受け入れた物だけ） |
| ws327-p017 | 規約の全文の見直し | cleared の候補（2026-10-30 P5） |

## 試験の行き先（Q1 への案）

- 今後も回帰に使う物を `plan/tools/keiland-layers/` へ移し plan/tools.md に登録する案: `host-test.sh`・`thread-test.c`・`socket-test.c`
  （層の host 試験）、`build-win32.sh`・`win32-link.c`（Win32 の compile と link）、`freebsd-layers.sh`（FreeBSD の guest）、
  `host-apps.sh`（ime・picker の host 試験の link）、`aat_ws327.py`（Music・Video Player の窓を閉じて終わる、Settings の Storage、IME の
  終わりの辞書）。移す時に script の中の `plan/ws327/tests` の path を直す。
- この WS だけの物（削除の候補）: `check-layers.py`・`layers-allow.tsv`（check.sh の T1・N1 に入った）、`check-base.sh`（実装の
  Phase 用）、`build-zedbsd.sh`、`guest-apps.sh`・`config-amd64-aat.mk`・`config-amd64-guest.mk`・`config-amd64-layers.mk`・
  `guest-layers.sh`、`doc-lines.py`、`make-frozen.sh`・`frozen.sha256`（あれば）、`__pycache__`。Phase の directory（phase001〜017、
  phase016 の `check-t1-n1.patch`・`tcp-close-repro.c` を含む。後者の写しは `plan/tools/tcp-close/`）。
- 参照の直し: `plan/bugs/BUG-367.md` の `plan/ws327/phase016/tcp-close-repro.c` → `plan/tools/tcp-close/tcp-close-repro.c`、
  `plan/bugs/BUG-368.md` の plan/ws327 の参照（Q1 の範囲）。

