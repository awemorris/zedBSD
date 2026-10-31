<!-- awesome-plan project=zedbsd record=ws335-p036 -->

# ws335-p036（提案、ユーザーの承認が要る）: libbrowser の net/tls.c の libssl-compat への切り替え

Status: planning（2026-11-01 p001 が提案。承認の問いは [user-questions.md](../user-questions.md) U2。依存: p033）
Parent: [WS335](../ws.md)。担当: 未定（WS074 の担当と調整）。

- 案 A（推し）: libbrowser の Makefile が zedBSD の build で `-DWB_TLS_COMPAT` を付け、net/tls.c の `TLS_LIBSSL`・`TLS_LIBCRYPTO`（`net/tls.c:45-48`）を `libssl-compat.so`・`libcrypto-compat.so` にする（Windows は WS334 の O7 の名前を `libssl-compat.dll`・`libcrypto-compat.dll` に）。
  `#ifdef` は名前の 4 行だけ。dlopen と関数の表と定数は変えない。libbrowser の package の依存に `base/libssl-compat` を足す。
- 受け入れ: WS074 の host の試験（本家と compat の両方）、QEMU の zedBSD でブラウザが https の頁を開く（Q1 の割当、画面の PNG）。
- 調整: net/tls.c は WS074 の所有、Windows の名前は WS334 の p004・p013。WS334 の S-A（OpenSSL の Windows の build）は不要になる（U5 の答えによる）。

## 結果

（未着手）
