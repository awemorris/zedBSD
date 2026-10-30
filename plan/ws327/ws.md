<!-- awesome-plan project=zedbsd record=ws327 -->

# WS327 libkeiland の thread と network の薄い wrapper（POSIX と Win32 で同じ interface）と app の移行

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-10-30 q1697 P2。p001・p002 cleared、p003 の guest は PASS（P2 の branch、未 merge）、標準廉価実装手順に組み直し）
- 目標: libkeiland に、pthread の類を包む thread の層と、socket の概念を見せる network の層を足し、Keiland の app の thread と socket をそれに移す。POSIX（zedBSD・Linux・FreeBSD）と Win32 で同じ interface で実装できる形。libbrowser は使わない。

## ユーザーの決定（2026-10-30、原文）

1. 「また、libkeilandに、スレッド、ネットワークのごく薄いラッパーを入れたいです。スレッドはpthreadのラッパー的なもので、Win32でも同じインタフェースで使えればOKです。ネットワークはソケットのラッパーで、ソケットという概念を晒してよくて、BSDソケットでもWinSockでも実装できるものとし、複数のソケットからからのpoll/selectをインタフェースで提供します。できそうですか？」
2. 「WS327を立ててOKです。appの対応も入れます。libbrowserは単体のライブラリなので、このラッパーは不使用です。」

## 方針（Q1 の案、2026-10-30。設計で置き換える）

- thread（`kl_thread_*` の類）: 作成・join・detach、mutex・条件変数・once、thread ごとの値、atomic（C11 の <stdatomic.h> で足りれば使う）、名前（WS309 の名前の付いた thread）、優先度と CPU の指定。POSIX は pthread、Win32 は CreateThread・SRWLOCK・CONDITION_VARIABLE。
- network（`kl_socket_*` の類）: socket の作成・connect・bind・listen・accept・send・recv・shutdown・close、非 blocking、address（IPv4・IPv6）、名前の解決（getaddrinfo の包み）、複数の socket の待ち（`kl_socket_poll` の類、BSD は poll か select、WinSock は WSAPoll か select、WSAPoll の connect の失敗の不具合を補う）。handle は不透明な型。TLS は範囲の外。
- WS309・WS314 の方針: socket の待ちは app の network の thread の中で使い、結果は kl_app_post で main に返す。kl_app_dispatch に fd の待ちは戻さない。
- 実装は libkeiland/platform/ の POSIX の層に（WS314 と同じ置き場）。Win32 の実装は interface が移せる事を設計で示す（build は範囲の外、可能なら host の cross の compile だけ）。

## Phase（案）

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws327-p001 | 設計（design.md、keiland_thread.h・keiland_socket.h の案、Win32 への写し方、移す app の一覧）、design-reviewer | planning | — |
| ws327-p002 | thread の層と host 試験（TSan） | planning | p001 |
| ws327-p003 | network の層と host 試験（loopback、IPv4・IPv6、poll） | planning | p001 |
| ws327-p004 | app の移行（Mailer の worker と network、Terminal・Installer・解説の例の thread など、p001 の一覧） | planning | p002・p003 |
| ws327-p005 | 解説（docs/howto/keiland-programming.md の 8 章などを新しい層に）と規約の全文の見直し | planning | p004 |

## Resume point

- 2026-10-30 Q1: 作成。
