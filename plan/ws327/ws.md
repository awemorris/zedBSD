<!-- awesome-plan project=zedbsd record=ws327 -->

# WS327 libkeiland の thread と network の薄い wrapper（POSIX と Win32 で同じ interface）と app の移行

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-10-30 Q1。p001〜p003・p007 cleared、p008〜p015 を Sonnet 5.5 high に委ねる（ユーザー「P2は手順書を書いたとのことですので、Sonnetで実施してOKです。」））
- 目標: libkeiland に、pthread の類を包む thread の層と、socket の概念を見せる network の層を足し、Keiland の app の thread と socket をそれに移す。POSIX（zedBSD・Linux・FreeBSD）と Win32 で同じ interface で実装できる形。libbrowser は使わない。

## ユーザーの決定（2026-10-30、原文）

1. 「また、libkeilandに、スレッド、ネットワークのごく薄いラッパーを入れたいです。スレッドはpthreadのラッパー的なもので、Win32でも同じインタフェースで使えればOKです。ネットワークはソケットのラッパーで、ソケットという概念を晒してよくて、BSDソケットでもWinSockでも実装できるものとし、複数のソケットからからのpoll/selectをインタフェースで提供します。できそうですか？」
2. 「WS327を立ててOKです。appの対応も入れます。libbrowserは単体のライブラリなので、このラッパーは不使用です。」
3. （2026-10-30、Q1 経由）「P2ですが、Windows用の実装を書いてしまってOKです。インタフェースがWindowsでも通用する形になっているかのチェックです。動作確認は不要です。mingwは使ってよいです。」
   - 解釈（Q1）: `thread-win32.c`・`socket-win32.c` を書き、`x86_64-w64-mingw32-gcc` で cross compile（warning 0）まで。wine での試験はしない。写せない所・口を変えた方がよい所は design.md に書いて Q1 に返す。p005 の後の p006。

## 方針（Q1 の案、2026-10-30。設計で置き換える）

- thread（`kl_thread_*` の類）: 作成・join・detach、mutex・条件変数・once、thread ごとの値、atomic（C11 の <stdatomic.h> で足りれば使う）、名前（WS309 の名前の付いた thread）、優先度と CPU の指定。POSIX は pthread、Win32 は CreateThread・SRWLOCK・CONDITION_VARIABLE。
- network（`kl_socket_*` の類）: socket の作成・connect・bind・listen・accept・send・recv・shutdown・close、非 blocking、address（IPv4・IPv6）、名前の解決（getaddrinfo の包み）、複数の socket の待ち（`kl_socket_poll` の類、BSD は poll か select、WinSock は WSAPoll か select、WSAPoll の connect の失敗の不具合を補う）。handle は不透明な型。TLS は範囲の外。
- WS309・WS314 の方針: socket の待ちは app の network の thread の中で使い、結果は kl_app_post で main に返す。kl_app_dispatch に fd の待ちは戻さない。
- 実装は libkeiland/platform/ の POSIX の層に（WS314 と同じ置き場）。Win32 の実装は interface が移せる事を設計で示す（build は範囲の外、可能なら host の cross の compile だけ）。

## Phase（2026-10-30 Q1 が標準廉価実装手順の形に組み直した）

ユーザーの指示（2026-10-30、原文）「P2をいったんラップアップして、標準廉価実装手順を適用するようにWS237のphaseを整理しなおして、再起動してほしいです。」
（Q1 の解釈: WS237 は完了済みの i915 の WS なので WS327 の事。手順は [docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)。
設計の担当 = P2（Opus 5.5）、実装の担当 = Sonnet 5.5 high を Phase ごとに別の worktree で。`keiland_thread.h`・`keiland_socket.h` は p002・p003 で凍結した BASE とし、実装の担当は変えない。）

| Phase | 目的 | 担当 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| [ws327-p001](phase001/phase.md) | 設計（design.md、header の案、Win32 への写し方、移す app の一覧）、design-reviewer | P2 | cleared | — |
| [ws327-p002](phase002/phase.md) | thread の層（`thread-posix.c`）と host 試験 | P2 | cleared | p001 |
| [ws327-p003](phase003/phase.md) | network の層（`socket-posix.c`）と host 試験、zedBSD の guest で 1 回 | P2 | cleared（2026-10-30 Q1、main merge。host 12 build と zedBSD の guest で thread 27・socket 53 PASS） | p001 |
| [ws327-p004](phase004/phase.md) | app の移行（一括） | — | canceled（p008〜p012 に分けた。Mailer の途中の移行 6bb8c3c8d は p008 が引き継ぐ） | — |
| ws327-p005 | 解説と規約の全文の見直し（一括） | — | canceled（p015・p017 に分けた） | — |
| ws327-p006 | Win32 の実装（一括） | — | canceled（p013・p014 に分けた） | — |
| [ws327-p007](phase007/phase.md) | 委譲の設計（手順の §4.4〜§4.8）: p008〜p017 の各 phase.md（触る file、design の節、移す前後の対応、試験のコマンド、突然変異の確かめ、受け入れ）、Win32 の `thread-win32.c`・`socket-win32.c` の骨格と mingw の build の script、app の移行の試験の道（既存の host 試験の link の直し方を含む）、境界の check の script | P2 | cleared（2026-10-30 Q1、0f60bee4a。phase008〜017、design §13、design-reviewer 21 件を反映） | p003 |
| [ws327-p008](phase008/phase.md) | Mailer の移行の残り（`sync.c`、6bb8c3c8d の `conn.c`・`tls.c`・`imap.c` の確かめ）、`plan/tools/mail/` の host 試験の link と `host-mail-backend.c`・`host-mail-sync.c` | Sonnet | planned | p007 |
| [ws327-p009](phase009/phase.md) | Music・Video Player・media-app の移行、`plan/tools/music/`・`plan/tools/media/` の host 試験の link | Sonnet | planned | p007 |
| [ws327-p010](phase010/phase.md) | Photos（thumbs・media-worker）の移行 | Sonnet | planned | p007 |
| [ws327-p011](phase011/phase.md) | Settings（look・storage-scan・storage-trash）の移行 | Sonnet | planned | p007 |
| [ws327-p012](phase012/phase.md) | IME の辞書の thread、libkeiland の picker-model・audio の移行 | Sonnet | planned | p007 |
| [ws327-p013](phase013/phase.md) | `thread-win32.c` を書き mingw で cross compile（warning 0）。動作確認は不要（決定 3） | Sonnet | cleared（2026-10-30 Q1、main 93e627b96。mingw x86_64・i686 で compile と link PASS・warning 0、sleep は QPC、cpu_count は affinity。Win32 で header が揺れる 5 点は phase013 の結果、p016 で Q1 に一覧） | p007 |
| [ws327-p014](phase014/phase.md) | `socket-win32.c` を書き mingw で cross compile（warning 0）。動作確認は不要（決定 3） | Sonnet | planned | p007 |
| [ws327-p015](phase015/phase.md) | 解説（keiland-programming.md の 8 章、08-worker.c・08-language.c を新しい層に、network の節と 08-network.c） | Sonnet | planned | p007（API は凍結済み。2026-10-30 P2 が p008〜p012 から外した） |
| [ws327-p016](phase016/phase.md) | 受け入れ: OS の境界の check（design §1.3）、全 app の QEMU の回帰（design §6.4）、Win32 で口が成り立たない所の Q1 への報告 | P2 | planned | p008〜p015 |
| [ws327-p017](phase017/phase.md) | 規約の全文の見直し | Sonnet | planned | p016 |

## Resume point

- 2026-10-30 Q1: 作成。
- 2026-10-30 P2: p001 設計（[design.md](design.md)、review 18 件を反映、Q1 の答えは §12）・p002 thread（KL_VERSION 103）・p003 network（104）の
  実装と host 試験。次: p003 の zedBSD の guest での実行、p004 の app の移行（Mailer から）。
- 2026-10-30 P2 ラップアップ（Q1 の指示: Phase を標準廉価実装手順の形に組み直してから再起動）。p003 は guest の実行が残り、p004 は Mailer の
  `conn.c`・`tls.c`・`imap.c`・`mail.h` まで（`sync.c` が残り Mailer は build しない、[phase004](phase004/phase.md)）。guest の image の config は
  `tests/config-amd64-guest.mk`（未 build）。p006（Win32）は未着手。
- 2026-10-30 Q1: ユーザーの指示で Phase を標準廉価実装手順の形に組み直した（上の表）。P2 を再起動し p003 の guest の実行と p007 の委譲の設計から。
- 2026-10-30 P2（q1697-i02）: p003 の guest の実行 PASS（cleared の候補）。p007: p008〜p017 の phase.md、design §13（実装の Phase の共通の約束）、
  Win32 の骨格、試験の道具（`check-layers.py`・`check-base.sh`＋`frozen.sha256`・`build-win32.sh`・`host-apps.sh`・`doc-lines.py`・
  `guest-layers.sh`）。p015 の依存を p007 に（API は凍結済み）。design-reviewer 21 件を反映（high の 1: Mailer が main で壊れないよう
  P2 が `sync.c` を移した。p008 は確かめと host 試験の直し）。実装の担当は ws.md に触らない（Q1 が merge の時に表を直す）。
