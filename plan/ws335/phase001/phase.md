<!-- awesome-plan project=zedbsd record=ws335-p001 -->

# ws335-p001: 設計（libssl-compat、標準廉価実装手順 §4）

Status: in-progress（2026-11-01 01:20 P6 q1789）
Parent: [WS335](../ws.md)

## 範囲

1. 使う側の事実: zedBSD の tree で OpenSSL を使う所（libbrowser `net/tls.c` の SSL_*・X509_*・ERR_*、libpasskey の `crypto-openssl.c`、他）と、
   userland/packages の中で OpenSSL に link する package（openssh・curl など）を読み、互換に要る API の部分集合を決める（file:line）。
2. 範囲: TLS 1.3（RFC 8446）と TLS 1.2（RFC 5246）の client（server は要るか）、暗号の組（AES-GCM・ChaCha20-Poly1305、ECDHE X25519・P-256、署名 ECDSA・RSA-PSS・
   RSA PKCS#1）、X.509 の証明書の連鎖の確かめ（RFC 5280）と host 名の照合（RFC 6125）、ca-certificates の読み、OCSP・CRL の扱い、ALPN・SNI。
   範囲の外と、範囲の外の入力の扱い。
3. 安全: 定数時間の実装（秘密の値で分岐・表引きしない）、乱数（getrandom・BCryptGenRandom）、memory の消去、既知の攻撃（padding oracle など）への備え。
   試験: 各仕様の試験の vector（NIST CAVP、RFC の付録、Wycheproof）、相手を OpenSSL・実の web site にした相互の試験。
4. 置き場所・名前（header の `openssl/ssl.h` 互換と library の名前、関数の接頭辞）、zedBSD と Windows（WS334、llvm-mingw）の両方で build する事。
5. 標準廉価実装手順の §4: 仕様を消えない場所に置く（git の外）、module の分け方、header の書き方、.c の骨格、試験の道具、実装の Phase の表と phase.md、
   design-reviewer、Q1 へ返す物。ユーザーへの問いは AGENTS.md §3 の前置きの形で。
6. code は header・骨格・試験の道具の他は書かない。
