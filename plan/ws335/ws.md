<!-- awesome-plan project=zedbsd record=ws335 -->

# WS335 libssl-compat と libcrypto-compat: OpenSSL の libssl・libcrypto と互換の口を持つ、zedBSD の自前の TLS と暗号の library

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG002。
- Status: planning（2026-11-01 01:20 新設、P6 が p001 の設計を q1789 で始めた）
- 目標: OpenSSL の libssl の API（と、それが要る libcrypto の部分）と source の互換を持つ library を、外部の library を取り込まずに自前で実装する。
  zedBSD の program（libbrowser の `net/tls.c` など）と、Windows の OmniGrammaton（[WS334](../ws334/ws.md) の決定 16）が、OpenSSL から link を替えるだけで切り替えられる形にする。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。p001 は設計の担当（Opus）が仕様と module・header・試験の道具を作り、
  p002 以降は実装の担当（Sonnet 5.5 high）が module ごとに実装する。
- 関係: [WS336](../ws336/ws.md)（自前の sshd・ssh、この WS の暗号の口を使う。並べて進め、依存は待ち合わせる）、[WS334](../ws334/ws.md)（決定 16: 当面 OpenSSL、libssl-compat ができたら切り替える）、[WS172](../ws172/ws.md)（passkey の FIDO2 の暗号が OpenSSL の libcrypto を使う例外、
  Guardrail の例外の表「リリースの前に独自実装に置き換える」）。

## ユーザーの決定

- 2026-11-01 ユーザーから、WS を立てて libssl-compat を実装する、標準廉価実装手順を使う、今 P6 で始める、という趣旨の指示を受けた。
- 2026-11-01 ユーザーから、いったん libbrowser に要る範囲だけを実装すればよい、header は他の compat の library と同じ置き方にし、本家（OpenSSL）の header とは分ける、という趣旨の指示を受けた（他の compat は `include/libc/compat/<名前>/`、例 `include/libc/compat/png/png.h`）。
- 2026-11-01 ユーザーから、最終的には OpenSSH も独自の sshd・ssh に置き換えるので、libssl-compat は OpenSSH の libcrypto の使い方に合わせる必要は無く、（後の独自の ssh が使える）口だけ作っておけばよい、という趣旨の指示を受けた（Q1 の解釈: 暗号の部品（hash・MAC・AEAD・鍵の交換・署名・乱数）を内部の口として分けて持ち、OpenSSH 互換の libcrypto の API は作らない）。
- 2026-11-01 ユーザーから、後の独自の sshd・ssh（OpenSSH の置き換え）は、鍵は ed25519 だけ使えればよく、鍵の交換も最新の定番を 1 つ持ち、OpenSSH の sshd と独自の sshd の両方に接続できればよい、という趣旨の指示を受けた（libssl-compat の暗号の口はこれを満たす部品を揃える: Ed25519 の署名と確かめ、鍵の交換の 1 つ（候補 mlkem768x25519-sha256 か curve25519-sha256、設計で案）、それに要る hash と AEAD）。
- 2026-11-01 ユーザーから、libcrypto-compat も作る、範囲は libbrowser と自前の ssh・sshd（[WS336](../ws336/ws.md)）で使う部分だけ、これは WS335 に足す、という趣旨の指示を受けた（前の「OpenSSH の libcrypto の互換は作らない」の読みを改める: OpenSSL の libcrypto と互換の API を、この 2 つが使う部分だけ作る。header は libssl-compat と同じく `include/libc/compat/` の下に本家と分けて置く）。

## Resume point

- p001（設計）を P6 が q1789 で進めている。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（標準廉価実装手順 §4）: 範囲（TLS の版・暗号の組・証明書の確かめ・OpenSSL の API の部分集合）、仕様の入手、module・header・.c の骨格・試験の道具、実装の Phase の表、design-reviewer、ユーザーへの問い | in-progress | — |
