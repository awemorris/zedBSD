<!-- awesome-plan project=zedbsd record=ws335-p001 -->

# ws335-p001: 設計（libssl-compat、標準廉価実装手順 §4）

Status: cleared（2026-11-01 05:45 Q1、main 8e44fa17e）
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

## 結果（2026-11-01 P6、q1789-i01）

- 範囲の変更（ユーザーの決定、ws.md）: libbrowser の net/tls.c と WS336 の ssh・sshd が使う物の和だけ、libcrypto-compat と libssl-compat の 2 つ、header は `include/libc/compat/crypto/`・`ssl/`。
- 作った物: [design.md](../design.md)、[user-questions.md](../user-questions.md)（U1〜U6、Q1 への確認 3 件）、header（`userland/base/libcrypto-compat/` 24、`userland/base/libssl-compat/` 8、公開 7）、
  .c の骨格（29＋10、全部 ENOSYS か失敗の値）、試験の道具（[tests/](../tests/): check-skeleton.sh・run-unit.sh・run-ct.sh・fetch.sh・vectors.py・unit/check.h・vecfile.h・derwriter.h・ct.h・test-tools.c）、
  実装の Phase の phase.md（p002〜p035、提案の p036）、ws.md の Phase の表。design-reviewer の記録は [design-review.md](../design-review.md)。
- 仕様と試験の data: P6 の worktree の `build/ws335-specs/`（RFC 33・NIST/SECG/ITU の PDF 11）と `build/ws335-vectors/`（Wycheproof・x509-limbo・CAVP 9 本、`rec/` に 169 個の record file）。SHA-256 は fetch.sh と design.md §1。共有の場所への写しは Q1。
- 確かめ: check-skeleton.sh PASS（cc・clang の -std=c89 -Wpedantic、zedBSD の clang の x86_64 と aarch64、llvm-mingw の x86_64 と aarch64）、run-unit.sh tools PASS（18 checks）、fetch.sh vectors PASS。
- 未実施: zedBSD の .so の build の規則（p033）、QEMU・実機の試験（実装の後）、IACR ePrint の 2 本の取得（server の 403、別の経路で取り直していない）。
- 2026-11-01 q1789-i02（P6）: U1〜U6 の答え（全部 A、Windows は DLL を作らず 1 つの .exe に static link）を入れた。header（cryc.h・cryc-os.h・cryc-chain.h・compat/crypto/x509.h・compat/ssl/ssl.h）、骨格（cryc-os-posix.c・cryc-os-win32.c・cryc-chain.c・api-ssl.c）、
  試験の道具（run-unit.sh `--win32`、check-skeleton.sh の 6・7、unit/test-tools.c の comment）、design.md（D1・D4・D14・D18・D23〜D28、§4.2〜§4.4、§8、§9、§13）、ws.md、user-questions.md、phase.md（全部の実装の Phase、新しい p039）。
  design-reviewer の 2 回目 16 件を全部反映（[design-review.md](../design-review.md)）。**新しい基点 b7dfd3952**。確かめは design-review.md の「2 回目の反映の後の確かめ」。
