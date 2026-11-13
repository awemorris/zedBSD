<!-- awesome-plan project=zedbsd record=ws335-p041 -->

# ws335-p041: mailer の TLS を libssl-compat・libcrypto-compat に切り替える

Status: in-progress（2026-11-13 Q1、q2048-i01 P2）
Parent: [WS335](../ws.md)

## 目的

2026-11-13 ユーザーの決定: WS335 に mailer の Phase を足し、終わってから WS335 を完了にする。

`userland/desktop/mailer/tls.c` は OpenSSL を名前で開き、自己署名の server の pin に `SSL_get1_peer_certificate` と `X509_digest` を使う。compat にはこの 2 つが無い（[p036](../phase036/phase.md) §4）。

## 範囲

1. libssl-compat に `SSL_get1_peer_certificate`（leaf の DER を SSL に残す）、libcrypto-compat に `X509_digest`（SHA-256）を足す。凍結の header の変更は Q1 が承認した（2026-11-13、check-skeleton の BASE はこの Phase の後に Q1 が移す）。
2. mailer の tls.c に `ML_TLS_COMPAT` で compat の名前だけを開く分岐（libbrowser の WB_TLS_COMPAT と同じ形）。mailer の Makefile の 1 行（`plan/ws335/phase036/mailer-Makefile.diff`）は差分の案としてユーザーへ（当てるまでの確かめは command line の -D で）。
3. 付随: top の Windows 版の入口が `make browser-win32`（ユーザーが mk/foreign.mk に足した）になったので、`plan/ws334/tests/cross-build.sh` の top の入口の段と comment、cockpit の UAT の手順の `make browser-win64-zip` を今に合わせる（cockpit は Q1 が直す。担当は案を返す）。
4. 確かめ: unit・interop の試験（自己署名の pin を含む）、x86_64 の build warning 0、QEMU の guest で mailer が TLS の server（試験の s_server）に pin で繋がる事。
