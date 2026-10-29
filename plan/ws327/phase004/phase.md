<!-- awesome-plan project=zedbsd record=ws327-p004 -->

# ws327-p004 app の移行（thread・socket を libkeiland の層へ）

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1・§6。Queue: q1697 / q1697-i01（担当 P2、2026-10-30）。
- Status: canceled（2026-10-30 Q1: 標準廉価実装手順に組み直し、p008〜p012 に分けた。ここまでの Mailer の途中の移行（6bb8c3c8d）は p008 が引き継ぐ）
- Disposition: normal

## 到達（2026-10-30）

- Mailer（未 build・未試験）: `conn.c` を `kl_socket_resolve`・`kl_socket_open`・`kl_socket_connect`・`kl_socket_poll`・`kl_socket_receive`・
  `kl_socket_send` に移した（解決の失敗は全て ENOENT のまま、接続は非 blocking で 15 s 待って blocking に戻す）。`mail.h` の `struct ml_conn` は
  `struct kl_socket *socket`、`ml_tls_open(intptr_t socket, ...)`。`imap.c` は `conn.socket != NULL`。`tls.c` は `KL_MUTEX_INIT` と
  `kl_socket_native`。
- **残り（この状態では Mailer は build できない）**: `sync.c`（thread・mutex を `kl_*`、wake の pipe を `kl_socket_waker`、`sync_wait` の `poll` を
  `kl_socket_poll`、`conn.fd` を `conn.socket`）、Mailer の Makefile 3 本（libkeiland の link の確認）、`plan/tools/mail/run-host-*.sh` と
  `host-mail-backend.c`（Q1 が範囲に追加）。その後 design §1.1 の他の app（Music・Video Player・media-app・Photos・Settings・IME・libkeiland の
  picker-model・audio）、§1.3 の境界の check、§6.4 の確かめ。
