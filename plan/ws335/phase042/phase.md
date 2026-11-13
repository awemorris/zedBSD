<!-- awesome-plan project=zedbsd record=ws335-p042 -->

# ws335-p042: TLS を libssl-compat・libcrypto-compat を前提に書き換える設計（mailer・libbrowser・browser-win32）と OpenSSL・OpenSSH を外部の package にする

Status: planned
Parent: [WS335](../ws.md)

## 目的

2026-11-13 ユーザーから、次の趣旨の決定を受けた（要点、原文ではない）:

1. mailer の TLS は選べる機能ではなく、compat の TLS（libssl-compat・libcrypto-compat）を前提に書き換える。`-DML_TLS_COMPAT` は要らない形にし、source でもその macro を使わない。Makefile もその前提に。
2. libbrowser と browser-win32 も同じく compat の TLS を前提にする。`WB_` で始まる compat の macro（`WB_TLS_COMPAT`）を使う今の実装（名前で開く表・`tls-static-compat.c` の表）を改める。
3. OpenSSL・OpenSSH は、利用者が明示的に指定しない限り build に入らない、完全に外部の package として扱う。計画書だけでなく source tree にも明記する。

（ユーザーは「libtls-compat」と呼んだ。Q1 は、今ある libssl-compat・libcrypto-compat の 2 つ（WS335 の成果）の事と読む。新しい library の名前が要ると判断すれば、案として返す。）

## 進め方

標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。この Phase は設計の担当（Opus）が行い、実装は Sonnet 5.5 high の Phase（p043 以降）に渡す。

## 範囲（設計の担当）

1. 今の形を調べる: `userland/desktop/mailer/tls.c`、`userland/desktop/libbrowser/net/tls.c`・`net/tls-library.h`・`net/tls-static-compat.c`、`userland/desktop/libbrowser/Makefile`、`apps/browser-win32/Makefile`、`userland/desktop/mailer/Makefile`、試験（`plan/ws335/tests/` の config-p036.mk・mailer-tls.c・run-mailer-*.sh・run-nettls.sh、`plan/ws334/tests/`）。dlopen の名前の表・OpenSSL への落ち・macro の分岐の全部を一覧にする。
2. あるべき形を決める: compat の header（`include/libc/compat/ssl/`・`compat/crypto/`）を直に include し、zedBSD は `libssl-compat.so`・`libcrypto-compat.so` に link（package の依存にも書く）、Windows は `libssl-compat.a`・`libcrypto-compat.a` を static link。OpenSSL を開く道・名前の表・`WB_TLS_COMPAT`・`ML_TLS_COMPAT` を無くす。host（Linux）での unit・試験の build の扱いも決める（host の compat の .so を使う等）。
3. OpenSSL・OpenSSH の扱い: `userland/packages/security/openssl`・`userland/packages/network/openssh` の Makefile の先頭（と要れば README）に、明示的に指定しない限り build に入らない外部の package である事を書く案。既定は既に n。それに依る package（curl・python3 など）と、`config/ci/*.mk` の一覧に openssl・openssh・curl が明示されている事の扱いは、Q1 へ問いとして返す（config-amd64-server.mk はユーザーの物）。
4. 変える Makefile・`mk/` の差分の案を全部 1 つの file に（`plan/ws335/phase042/makefile.diff`）。Makefile は Claude が当てない。Q1 がユーザーに見せて承認の後に当てる。
5. 実装の Phase（p043〜）の phase.md を置く: Sonnet が迷わず書けるよう、file ごとの変え方・残す振る舞い（自己署名の pin・誤りの文・証明書の検証・default verify paths・Windows の cert.pem）・確かめのコマンドを書く。Phase の分け方は担当が決める（例: libbrowser、mailer、browser-win32 と試験の道具）。
6. design-reviewer を 1 回起動し、指摘を反映する。

## 受け入れ

- 一覧（今の macro・表・落ちの全部）、あるべき形、Makefile の差分の案、p043〜の phase.md、design-reviewer の結果が `plan/ws335/phase042/` にある。
- source は変えない（試験の道具の下書きは可）。Makefile・`mk/` は案だけ。
- Q1 への問い（CI の config の扱いなど）が一覧になっている。
