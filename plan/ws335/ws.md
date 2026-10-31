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
- 2026-11-01 P6: 設計を [design.md](design.md)、問い U1〜U6 を [user-questions.md](user-questions.md) に書いた。header（libcrypto-compat 24＋公開 6、libssl-compat 8＋公開 1）・.c の骨格（29＋10）・試験の道具（[tests/](tests/)）を置いた。
  実装の Phase p002〜p035（と提案の p036）の phase.md を置いた。header の凍結（基点の SHA）は design-reviewer の反映の後。仕様と試験の data は P6 の worktree の `build/ws335-specs/`・`build/ws335-vectors/`（Q1 が共有の場所へ写す、user-questions.md の Q1 への確認 3）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（標準廉価実装手順 §4）: 範囲（TLS の版・暗号の組・証明書の確かめ・OpenSSL の API の部分集合）、仕様の入手、module・header・.c の骨格・試験の道具、実装の Phase の表、design-reviewer、ユーザーへの問い | in-progress | — |
| [p002](phase002/phase.md) | 土台（約束・定数時間の補助・byte 順）、SHA-2、hash の選択・HMAC・HKDF（test-util、test-sha2、test-hash） | planned | p001 |
| [p003](phase003/phase.md) | SHA-3・SHAKE（Keccak）（test-sha3） | planned | p002 |
| [p004](phase004/phase.md) | AES（bitsliced、定数時間）と counter mode（test-aes） | planned | p002 |
| [p005](phase005/phase.md) | AES-GCM（test-gcm） | planned | p004 |
| [p006](phase006/phase.md) | ChaCha20・Poly1305・AEAD（と 64 bit の counter の ChaCha20）（test-chacha） | planned | p002 |
| [p007](phase007/phase.md) | 固定幅の整数と Montgomery の算術（test-bn） | planned | p002 |
| [p008](phase008/phase.md) | GF(2^255-19) と X25519（test-f25519、test-x25519） | planned | p002 |
| [p009](phase009/phase.md) | Ed25519（test-ed25519） | planned | p008 |
| [p010](phase010/phase.md) | P-256・P-384 の群（test-ecp） | planned | p007 |
| [p011](phase011/phase.md) | ECC: 鍵の生成・ECDH・ECDSA の確かめ（test-ecc） | planned | p010 |
| [p012](phase012/phase.md) | RSA の署名の確かめ（PKCS#1 v1.5・PSS）（test-rsa） | planned | p007、p002 |
| [p013](phase013/phase.md) | ML-KEM の多項式・符号化・sampling（test-mlkem-poly） | planned | p003 |
| [p014](phase014/phase.md) | K-PKE と ML-KEM-768（test-mlkem） | planned | p013 |
| [p015](phase015/phase.md) | OS の層（POSIX と Windows）（test-os） | planned | p002 |
| [p016](phase016/phase.md) | DER の reader と PEM・base64（test-der、test-pem） | planned | p002 |
| [p017](phase017/phase.md) | X.509 の parse（証明書・公開鍵・署名の algorithm）（test-x509） | planned | p016 |
| [p018](phase018/phase.md) | 署名の確かめ（algorithm ごと）（test-sigver） | planned | p017、p011、p012 |
| [p019](phase019/phase.md) | 名前（Name の比較・host と address の照合・name constraints）（test-name） | planned | p017 |
| [p020](phase020/phase.md) | trust store と path の検証（test-chain） | planned | p018、p019、p015 |
| [p021](phase021/phase.md) | 誤りの queue と ERR_・RAND_bytes・OPENSSL_cleanse・CRYPTO_memcmp（test-err） | planned | p015 |
| [p022](phase022/phase.md) | 公開の X509（d2i_X509 ほか）と内部の store の生成（test-api-x509） | planned | p020、p021 |
| [p023](phase023/phase.md) | 公開の EVP の digest・ChaCha20・Poly1305（test-api-evp） | planned | p021、p006、p002 |
| [p024](phase024/phase.md) | 公開の EVP_PKEY（Ed25519・X25519・ML-KEM-768）（test-api-pkey） | planned | p021、p009、p008、p014 |
| [p025](phase025/phase.md) | TLS の定数・suite の表・presentation language の reader/writer（test-tls、test-buf） | planned | p002 |
| [p026](phase026/phase.md) | record の保護（test-record） | planned | p025、p005、p006 |
| [p027](phase027/phase.md) | transcript と鍵の schedule（1.3 と 1.2）（test-keysched） | planned | p026 |
| [p028](phase028/phase.md) | handshake の message の encode と syntax の確かめ（test-msg） | planned | p025、p020 |
| [p029](phase029/phase.md) | handshake の共通と TLS 1.3（test-hs13） | planned | p027、p028、p020、p011、p008 |
| [p030](phase030/phase.md) | TLS 1.2 の handshake（test-hs12） | planned | p029 |
| [p031](phase031/phase.md) | 接続（record の I/O・再組み立て・alert・close）（test-conn） | planned | p030、p015 |
| [p032](phase032/phase.md) | 公開の SSL・SSL_CTX（test-api-ssl） | planned | p031、p022 |
| [p033](phase033/phase.md) | 統合: zedBSD の .so と exports、Windows の DLL、相互接続（OpenSSL s_server）・x509-limbo・RFC 8448・net/tls.c の host 試験（Opus） | planned | p032、p023、p024 |
| [p034](phase034/phase.md) | 安全の review と hardening: 定数時間・fuzz・checklist（Opus） | planned | p033 |
| [p035](phase035/phase.md) | 規約の全文の見直しと header の持ち越しの直し | planned | p034 |
| [p036](phase036/phase.md) | 提案（要承認 U2）: net/tls.c の libssl-compat への切り替え | planning | p033 |
