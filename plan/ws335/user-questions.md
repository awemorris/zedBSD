<!-- awesome-plan project=zedbsd record=ws335-questions -->

# WS335 ユーザーへの問い（p001 の設計から、2026-11-01）

## 前置き

- **WS335**（libcrypto-compat と libssl-compat: OpenSSL と同じ名前の関数を持つ、zedBSD の自前の暗号と TLS の library）。
  WS の種類: library の新規の実装（外部の標準に沿うので標準廉価実装手順。設計は Opus、実装は Sonnet が module ごとに）。
- 今の状態: p001（設計）を P6 が進め、header・骨格・試験の道具・実装の Phase の案（p002〜p036）を置いた。実装はまだ。設計は [design.md](design.md)。
- 決まっていない事: 下の U1〜U6。どれも推しの案で設計を書いてあり、違う答えなら header と Phase を直す。
- 用語:
  - **TLS**: https の暗号の通信の規格。1.3 が今の版、1.2 が一つ前。
  - **OCSP・CRL**: 証明書が取り消されていないかを確かめる仕組み。
  - **session の再開・0-RTT**: 2 回目の接続を速くする仕組み（0-RTT は最初の往復の前に data を送る）。
  - **ALPN**: 接続の時に HTTP/2 などの protocol を決める拡張。
  - **CN**: 証明書の「一般名」。古くは host 名をここに書いた。今は SAN（別名の欄）に書く決まり（RFC 9525）。
  - **ML-KEM**: 量子計算機に強い鍵の交換（FIPS 203）。OpenSSH 10 の既定は X25519 と ML-KEM-768 を組み合わせた mlkem768x25519-sha256。
  - **trust store**: 信頼する根の証明書の集まり。

## U1 範囲の外の一覧

TLS の client として、次を作らない（net/tls.c が使わない、または OpenSSL の既定でも働いていない）: 失効の確かめ（OCSP・CRL）、session の再開と 0-RTT、ALPN、TLS の server、client の証明書、
CBC の暗号（古い暗号の組）、TLS 1.0・1.1、P-521・Ed25519・SHA-1 で署名された証明書（Ed25519 は ssh 用には作る）、量子耐性の TLS の鍵の交換（X25519MLKEM768）。後で要れば別の Phase か WS で足す。

- **A（推し）**: この一覧のとおり範囲の外にする。
- B: このうち（ ）は範囲に入れる。

## U2 libbrowser の net/tls.c を本家の OpenSSL と libssl-compat のどちらで動かすかの切り替え

net/tls.c は今、OpenSSL の library を名前で開く（dlopen）。

- **A（推し）**: build の印（`-DWB_TLS_COMPAT`）で開く名前だけを `libssl-compat.so`・`libcrypto-compat.so`（Windows は `.dll`）に替える。zedBSD の build は compat、Linux の host の試験は本家のまま（compat も試せる）。
  切り替えの変更は WS335 の最後の Phase（p036）として、WS074（ブラウザ）と WS334（Windows 版）と調整して行う。
- B: dlopen をやめ、`<compat/ssl/ssl.h>` を include して直に link する（変更が大きく、package の無い時の扱いが変わる）。
- C: 切り替えは WS335 では行わず、WS074・WS334 の側の Phase にする。

## U3 自前の ssh・sshd（WS336）の鍵の交換

- **A（推し）**: mlkem768x25519-sha256（OpenSSH 10 の既定。OpenSSH 10.1 以降は量子耐性でない鍵の交換に警告を出す）。libcrypto-compat に SHA-3 と ML-KEM-768 を作る（実装の Phase が 3 つ増える）。
- B: curve25519-sha256 だけ（追加の費用は無いが、OpenSSH が警告を出す）。

## U4 library と header の名前・置き場所

- **A（推し）**: `/lib/libcrypto-compat.so`・`/lib/libssl-compat.so`（base、source は `userland/base/libcrypto-compat/`・`userland/base/libssl-compat/`）。header は
  `include/libc/compat/crypto/`（err・x509・evp・rand・crypto・types）と `include/libc/compat/ssl/ssl.h`。program は `#include <compat/ssl/ssl.h>` と書く。
- B: header を `include/libc/compat/openssl/` の 1 つにまとめる（WS336 の担当の案。2 つの library の header が 1 つの dir に混ざる）。
- C: 置き場所を desktop にする（base の program が使えなくなる）。

## U5 Windows で信頼する根の証明書

- **A（推し）**: libssl-compat が、Windows の根の store（利用者と機械の両方、「信頼しない」store の物を除く）と、DLL の隣の `cert.pem`（ca-certificates と同じ物）を読む。ブラウザは zedBSD と同じ呼び方のまま。
- B: library は読まず、ブラウザ（WS334）が Windows の store を読んで渡す（WS334 の設計の今の形）。

## U6 証明書の host 名の照合で CN を見るか

- **A（推し）**: 見ない（RFC 9525。今の公開の証明書は全部 SAN を持ち、zedBSD の試験の CA も SAN を付けている）。
- B: OpenSSL の既定と同じく、SAN に host 名が無い時は CN を見る。

## Q1 への確認（ユーザーに聞く前の技術の判断）

1. 公開の関数・型の名前と数値を OpenSSL 3.5 と同じにし、値は block ごとに「Values referenced: OpenSSL 3.5.8 include/openssl/…」で出典を書く（libz-compat の zlib と同じ扱い）。`OSSL_PARAM` の struct の配置も同じ（型を完全にするため）。Guardrail の出典の規則に合うかの確認。
2. IACR ePrint の 2 本の PDF の取得が server の 403 で止まった。別の経路で取り直していない。設計はこれに依らない（design.md §1）。
3. 仕様と試験の data を共有の `/home/awe/zedBSD-claude1/build/sources/specs/tls/` と `/home/awe/zedBSD-claude1/build/sources/tls-vectors/` に写す事（p001 の担当は main の checkout に書かない）。元は `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/` と `.../build/ws335-vectors/`。
