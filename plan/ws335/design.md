<!-- awesome-plan project=zedbsd record=ws335-design -->

# WS335 libcrypto-compat と libssl-compat の設計（p001、標準廉価実装手順 §4）

- 作成: 2026-11-01（P6、q1789-i01）。親: [ws.md](ws.md)。手順: [標準廉価実装手順](../../docs/agent/spec-module-delegation.md)。
- 範囲は 2026-11-01 のユーザーの決定（[ws.md](ws.md) の「ユーザーの決定」）による: **libbrowser の `net/tls.c` が使う物と、自前の ssh・sshd（[WS336](../ws336/ws.md)）が使う物の和**だけを、
  OpenSSL 3.5 と互換の API で作る。libcrypto-compat（暗号・証明書・誤りの queue）と、その上の libssl-compat（TLS の client）の 2 つの library。
- 状態: header・.c の骨格・試験の道具を置いた。header は WS336 の要る物の一覧（Q1 の中継、2026-11-01）を入れた形。**凍結（基点の SHA）は design-reviewer の指摘の反映の後**（§12）。

## 1. 出典（仕様と試験の data）

仕様は git に入れない（著作物）。p001 で設計の担当の worktree の `build/ws335-specs/`・`build/ws335-vectors/` に取得した。
**消えない共有の置き場所**は Q1 が写す: `/home/awe/zedBSD-claude1/build/sources/specs/tls/`（仕様）と `/home/awe/zedBSD-claude1/build/sources/tls-vectors/`（試験の data）。
取得と照合は [tests/fetch.sh](tests/fetch.sh)（`specs` と `vectors`、SHA-256 を照合、合わなければ `.bad` に残して FAIL）。実装の担当は `WS335_SPECS`・`WS335_VECTORS` に共有の場所を渡す。

| file | 版 | SHA-256（先頭 16 桁） | 印刷の頁 → PDF の頁 |
| --- | --- | --- | --- |
| rfc8446.txt | TLS 1.3（2018-08） | 47871bc8820a2c3b | text |
| rfc8448.txt | TLS 1.3 の trace（2019-01） | 6564d1376d1ec744 | text |
| rfc5246.txt・rfc5288・rfc5289・rfc8422・rfc7905・rfc7627・rfc5746・rfc6066 | TLS 1.2 と ECDHE・GCM・ChaCha の suite・EMS・RI・SNI | 全部は fetch.sh | text |
| rfc8996・rfc9325・rfc9155 | 1.0/1.1 の廃止、BCP 195、1.2 の MD5/SHA-1 の廃止 | fetch.sh | text |
| rfc5280・rfc4158・rfc9525（rfc6125） | X.509 の確かめ、path の組み立て、host 名 | a2f2628c0a83b873 ほか | text |
| rfc3279・rfc4055・rfc5480・rfc5758・rfc8017・rfc7468・rfc4648 | algorithm id、RSA、PEM、base64 | fetch.sh | text |
| rfc2104・rfc4231・rfc5869・rfc6234 | HMAC・HKDF | fetch.sh | text |
| rfc7748・rfc8032・rfc8439・rfc8731 | X25519・Ed25519・ChaCha20/Poly1305・SSH の curve25519 | fetch.sh | text |
| nist-fips-180-4.pdf | SHA-2（2015-08） | 0455b406d89648d2 | +5 |
| nist-fips-197-upd1.pdf | AES（2023-05） | 62c86eb567f13edb | +8 |
| nist-fips-202.pdf | SHA-3（2015-08） | 1592607831ff0908 | +8 |
| nist-fips-203.pdf | ML-KEM（2024-08） | fe1f12f32a7e44ec | +9 |
| nist-fips-186-5.pdf | DSS（ECDSA、2023-02） | fbb9c7c2ba442f03 | +9 |
| nist-sp-800-38d.pdf | GCM（2007-11） | d99f3921ccebca04 | +8 |
| nist-sp-800-56a-r3.pdf | ECC CDH（2018-04） | 6b315e3a91012981 | +11 |
| nist-sp-800-186.pdf | 曲線（2023-02） | 53c7cf3340528aaa | +11 |
| secg-sec1-v2.pdf | SEC 1 v2.0（2009-05） | 54687189af3de645 | +6 |
| itu-t-x690-202102.pdf・itu-t-x680-202102.pdf | DER・ASN.1 | 6ac799a2d1ebed36・b69dc4754f0709a7 | +8・+12 |
| openssl-3.5.8.tar.gz（`build/distfiles/`、既存） | 互換の API の正（`doc/man3/*.pod`・`include/openssl/*.h`） | a8f84a39918ec641 | — |

- 試験の data（`build/ws335-vectors/`、fetch.sh が `rec/*.vec` に変換、[tests/vectors.py](tests/vectors.py)）: Wycheproof（C2SP、commit d17cc89fa5c8、88dcda078c6ff643）、
  x509-limbo（C2SP、commit 1080e565f26e、611e337b9fb477b9、SERVER の 9,792 件）、NIST CAVP（SHA・SHA-3・SHAKE・AES KAT・GCM・ECDSA 186-4・RSA 186-3・ECC CDH）、RFC 8448 の 538 個の値。
- 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像でも照合（WS326 の教訓）。印刷の頁 N は PDF の頁 N + 上の列の数。
- 取得できなかった物: IACR ePrint の 2 本（Boyar–Peralta の S-box の回路 2011/332、Renes–Costello–Batina の完全な加算の式 2015/1060）は server が 403 を返した。
  手順に従い別の経路で取り直していない。設計はこれに依らない（AES の S-box は FIPS 197 の逆元と affine から、楕円曲線の式は SP 800-186 の A.1.1 から実装の担当が導く、§6）。

## 2. 事実（使う側、file:line、2026-11-01 の main）

### 2.1 範囲の中

- **libbrowser `userland/desktop/libbrowser/net/tls.c`**: OpenSSL の header を使わず、`dlopen` で `libcrypto.so`・`libssl.so`（無ければ `.so.3`）を開き（`:45-48`・`:653-656`・`:687-698`）、
  `dlsym` で 25 個の関数を引く（表 `:114-138`）。定数は自前の `#define`（`:51-62`、値は OpenSSL の物）。使い方:
  - context: `TLS_client_method`・`SSL_CTX_new`（`:761`）、`SSL_CTX_ctrl(…, 123 = SET_MIN_PROTO_VERSION, TLS1_2_VERSION)`（`:768`）、`SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL)`（`:775`）、
    `SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF)`（`:776`）、`SSL_CTX_set_default_verify_paths`（`:779`）、`SSL_CTX_load_verify_locations(ctx, file, NULL)`（`:781`、試験の CA）。
  - 接続: `SSL_new`（`:233`・`:376`）、`SSL_set_fd`（`:240`・`:383`）、`SSL_ctrl(55 = SET_TLSEXT_HOSTNAME, 0, host)`（`:825`）、`SSL_set1_host`（`:828`）、
    address の時は `X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), text)`（`:818`）、`SSL_connect`（`:253`・`:415`、非 blocking も）、`SSL_read`（`:286`・`:460`）、
    `SSL_write`（`:329`・`:512`）、`SSL_get_error`（`:293`・`:334`・`:420`・`:467`・`:519`・`:888`）、`SSL_pending`（`:547`）、`SSL_shutdown`・`SSL_free`（`:568-569`）。
  - 誤り: `SSL_get_verify_result`・`X509_verify_cert_error_string`（`:868-870`）、`ERR_get_error`・`ERR_error_string_n`・`ERR_clear_error`（`:872-881`）。`SSL_ERROR_SYSCALL` の後に `errno`（`:302`・`:482`）。
  - thread: context は 1 つを 2 つの thread（page と loader）の接続が共有、接続は 1 thread ずつ（`:24-27`）。
- **WS334（Windows の OmniGrammaton）の設計**（[plan/ws334/design.md](../ws334/design.md) §6.6 S-A）: Windows では `SSL_CTX_set_default_verify_paths` を呼ばず、
  Windows の ROOT の store の DER を `d2i_X509`・`SSL_CTX_get_cert_store`・`X509_STORE_add_cert`・`X509_free` で入れる。O7（DLL の名前）、O8（`SSL_set_fd` に SOCKET）、O32（`errno` と WinSock）。
- **WS336（自前の ssh・sshd）の要る物**（P5 → Q1 の中継、2026-11-01）: `RAND_bytes`、`OPENSSL_cleanse`・`CRYPTO_memcmp`、SHA-256 の `EVP_Digest*`・`EVP_MD_CTX_copy_ex`・`EVP_MD_get_size`、
  Ed25519（raw の鍵、`EVP_DigestSign*`・`EVP_DigestVerify*`）、X25519（raw の鍵、`EVP_PKEY_derive*`、全零の結果は失敗）、ML-KEM-768（`EVP_PKEY_CTX_new_from_name`・keygen・
  `EVP_PKEY_new_raw_public_key_ex`・`EVP_PKEY_get_raw_public_key`・encapsulate・decapsulate）、`EVP_chacha20` の cipher、`EVP_MAC`（POLY1305）。thread の安全は ssh には要らない。

### 2.2 範囲の外（後の候補として名前だけ挙げる、ユーザーの決定 1）

| 使う側 | 何を使うか | 扱い |
| --- | --- | --- |
| mailer `userland/desktop/mailer/tls.c` | libbrowser と同じ 22 個（`:115-136`）＋ `SSL_get1_peer_certificate`（`:679`）・`X509_digest`・`EVP_sha256`・`X509_free`（`:137-139`）、verify の callback（`:576`・`:660`） | 後の候補。足りないのは 3 関数（`SSL_get1_peer_certificate`・`X509_digest`・`EVP_sha256` の SHA-256 は有る）と callback の呼び方（有る） |
| libpasskey `userland/base/libpasskey/crypto-openssl.c`（fidoctl・passkey-fido2、Guardrail の例外） | OpenSSL 3 の EVP（`EVP_PKEY_Q_keygen` `:201`、`OSSL_PARAM` `:305-326`、`EVP_KDF` HKDF `:311`、AES-256-CBC `:277`、`HMAC` `:85`、P-256 の ECDH・ECDSA） | 後の候補（Guardrail の例外の期限「リリースの前」に効く）。OSSL_PARAM と P-256 の EVP_PKEY と AES の逆の暗号が要る |
| curl（`userland/packages/network/curl/Makefile:66-70`） | libssl・libcrypto の広い API | package は本家の OpenSSL のまま |
| OpenSSH（`.../openssh/Makefile:63-100`） | libcrypto（EC_KEY ほか） | WS336 の自前の ssh・sshd が置き換える |
| python3（`.../lang/python3/Makefile:60-126`）・`openssl` の command | ssl・hashlib | package は本家のまま |
| base の `fetch`（`userland/base/fetch/main.c:357`「https is not supported」） | — | 後の候補（libssl-compat が base に入れば https が持てる） |

## 3. 範囲と決定（D1〜D20）

| # | 決定 | 理由 | 選ばなかった案 |
| --- | --- | --- | --- |
| D1 | **2 つの library**: `libcrypto-compat`（`userland/base/libcrypto-compat/`）と `libssl-compat`（`userland/base/libssl-compat/`）。zedBSD は `/lib/libcrypto-compat.so`・`/lib/libssl-compat.so`（SONAME も同じ）、Windows は `libcrypto-compat.dll`・`libssl-compat.dll`（＋ import library `.dll.a` と静的な `.a`） | ユーザーの決定（libssl-compat の上下の 2 つ）。本家の `libssl.so`・`libcrypto.so`（package の `/usr/lib`）と名前が衝突しない。base に置くのは base の program（後の fetch・libpasskey・WS336 の ssh）が使えるため | 本家の名前で入れる（`/lib` が `/usr/lib` より先に探されるので、本家に link した curl・python が壊れる）。1 つの library（ユーザーの決定と違う） |
| D2 | **header は `include/libc/compat/crypto/`（types・err・x509・evp・rand・crypto）と `include/libc/compat/ssl/ssl.h`**。program は `<compat/crypto/evp.h>`・`<compat/ssl/ssl.h>` と書く | ユーザーの決定（他の compat と同じ `include/libc/compat/<名前>/`、本家と分ける）。png・zlib の前例（`#include <compat/png/png.h>`、`libbrowser/image/png.c:15`） | `include/libc/compat/openssl/`（P5 の案。1 つの dir に 2 つの library の header が混ざり、`<名前>` が library の名前でなくなる） |
| D3 | 公開の名前・型の tag・数値・戻り値の約束は **OpenSSL 3.5 の物**（値は block ごとに「Values referenced: OpenSSL 3.5.8 include/openssl/…」）。中身の code・comment・構造は写さない。内部の名前は独自（`cryc_`・`sslc_`） | 互換が目的。libz-compat（`include/libc/compat/zlib/zlib.h`）の前例と同じ扱い | OpenSSL の内部の名前（`ossl_`・`tls13_`）に似せる（Guardrail に反する） |
| D4 | **内部の口**: libcrypto-compat は内部の module の関数（`cryc_*`）を、libssl-compat のためだけの private な version node（`LIBCRYPTO_COMPAT_PRIVATE`、約束なし）で export する。libssl-compat は TLS の内部で `cryc_*`（hash・AEAD・鍵の交換・X.509・誤りの queue）を直に呼ぶ | 誤りの queue と trust store を 1 つにする（program から見て OpenSSL と同じく 1 つの ERR の queue）。TLS の record に EVP の層を挟まない | libssl-compat に暗号を静的に複写（queue が 2 つになる）。libssl-compat も公開の EVP だけを使う（AEAD・HKDF・X.509 の EVP が要り範囲が膨らむ） |
| D5 | **TLS は client だけ**。server は作らない | 使う側（net/tls.c）は client だけ。相手の試験は host の OpenSSL の `s_server` で足りる | server も（試験のためだけの大きな code） |
| D6 | **版**: TLS 1.3（RFC 8446）と TLS 1.2（RFC 5246）。1.0・1.1 は無し（RFC 8996）。1.3 を offer した時の 1.2 への downgrade の印を確かめる（4.1.3） | 1.2 だけの server が今も多い。net/tls.c は 1.2 以上を求める（`:768`） | 1.3 だけ（届かない site が残る） |
| D7 | **suite**: 1.3 の 3 つ（AES-128-GCM・AES-256-GCM・ChaCha20-Poly1305）、1.2 は ECDHE-ECDSA/RSA × AES-GCM/ChaCha20-Poly1305 の 6 つ。CBC・RSA の鍵の輸送・DHE・static ECDH は無し | AEAD だけなので padding oracle（Lucky13・POODLE）の類が成り立たない（RFC 9325 4.2 の推し） | CBC の suite（古い server の互換、定数時間の MAC-then-encrypt が要る） |
| D8 | **鍵の交換（TLS）**: x25519 と secp256r1。ClientHello は x25519 の share 1 つ、server が P-256 を望めば HelloRetryRequest で応える。X25519MLKEM768（hybrid）は範囲の外 | 1 つの share で handshake の計算が最小。HRR は RFC の必須 | 2 つの share を常に送る（毎回 P-256 の鍵の生成が要る） |
| D9 | **署名（TLS と証明書）**: ECDSA P-256・P-384（SHA-256/384/512）、RSA-PSS（rsae、SHA-256/384/512）、RSA PKCS#1 v1.5（1.2 と証明書だけ）。Ed25519・P-521・SHA-1 は証明書の署名として不可。RSA は 2048〜8192 bit | web PKI の現実（Let's Encrypt の E5/E6 は P-384） | SHA-1 の中間（RFC 9155 で廃止） |
| D10 | **SNI** は送る（host 名の時だけ、address では送らない、RFC 6066 3）。**ALPN は範囲の外**（net/tls.c が使わない） | ユーザーの決定 1 | ALPN（HTTP/2 を作る時の後の候補） |
| D11 | **証明書の確かめ**: RFC 5280 6.1 の path validation（policy の処理は無し、critical の policy の拡張は認識して通す）、path は RFC 4158 の流儀の深さ優先で組み、仕事の量（署名の確かめ 64 回）と深さ（10）を限る。name constraints（dNSName・iPAddress・directoryName・rfc822Name）を適用。trust anchor の有効期限は確かめる（OpenSSL と同じ） | web PKI の client の profile。x509-limbo で照合 | policy tree の全部（web PKI で使われず大きい） |
| D12 | **host 名の照合** は RFC 9525（6125 の後継）: SAN の dNSName だけ、wildcard は最も左の label の全体だけ、**CN への fallback は無し**。address は SAN の iPAddress | RFC 9525 6.3。CA/B の証明書は全部 SAN を持つ。試験の CA（`plan/tools/mail/run-host-mail-backend.sh:29`、`plan/ws074/tests/make-test-ca.sh`）も SAN を持つ | OpenSSL の既定（SAN の DNS が無い時は CN を見る） |
| D13 | **失効（OCSP・CRL）・session の再開・0-RTT・PSK・client 証明書・renegotiation・post-handshake auth・compression・heartbeat・record_size_limit・ECH は範囲の外**。NewSessionTicket は読んで捨てる、1.2 の HelloRequest には warning の no_renegotiation | OpenSSL の既定も失効を見ない。範囲を net/tls.c に限る決定 | resumption（毎回 full handshake の費用。HTTP の keep-alive が補う。後の候補） |
| D14 | **trust store**: zedBSD は `/etc/ssl/cert.pem`（ca-certificates の package、`userland/packages/security/ca-certificates/Makefile:10`）。Windows は `SSL_CTX_set_default_verify_paths` が Windows の ROOT（current user と local machine）から Disallowed を除いた物と DLL の隣の `cert.pem` を読む（WS334 の決定 18）。`SSL_CTX_get_cert_store`＋`X509_STORE_add_cert` の道（WS334 S-A）も持つ | 使う側が両方の OS で同じ呼び方にできる | Windows の検証（`CertGetCertificateChain`）を使う（挙動が OS で分かれる） |
| D15 | **乱数** は毎回 OS から（zedBSD `getrandom`、Windows `BCryptGenRandom`）。user 空間の DRBG は持たない | 単純で fork に安全。失敗は誤り（fallback なし） | DRBG（速いが fork・再 seed の扱いが要る） |
| D16 | **定数時間**: 秘密の値で分岐・表引き・除算をしない。AES は bitsliced（S-box は GF(2^8) の逆元と affine を論理演算で）、GHASH は SP 800-38D の Algorithm 1 を mask で、P-256 の scalar 倍は固定 window と全部の entry を読む選択、X25519 は ladder と cswap、ML-KEM は Barrett/Montgomery の還元と mask の比較 | 時間の side channel を消す。表の AES（T-table）は cache の攻撃に弱い | AES-NI・PMULL の hardware の道（速いが intrinsics と CPU の検出が要る。後の「速さ」の候補） |
| D17 | **C は ANSI C（C89）＋`<stdint.h>`**、64 bit の定数は 32 bit の半分 2 つから。OS に依る code は `cryc-os-posix.c`・`cryc-os-win32.c` だけ | 規約（[coding-style.md](../coding-style.md) §1）。zedBSD・Linux の host・llvm-mingw（x86_64・aarch64）の全部で同じ source | — |
| D18 | **OS の差**: socket は zedBSD で `read`/`write`、Windows で `recv`/`send`（`SSL_set_fd` の int を SOCKET として、OpenSSL の流儀）、`WSAGetLastError` を errno に写す（WS334 O32）。thread ごとの値は pthread の key と Windows の FLS | WS334 の O8・O32 と合わせる | — |
| D19 | **WS336 の鍵の交換**: mlkem768x25519-sha256 を支持（P5 の案）。ML-KEM-768（FIPS 203）と SHA-3/SHAKE（FIPS 202）を libcrypto-compat に入れる（実装の Phase 3 つ） | in-tree の OpenSSH 10.5p1 の既定、OpenSSH 10.1 以降は PQ でない kex に警告。X25519 は TLS で作るので hybrid の残りは ML-KEM だけ。ML-KEM は後の TLS の X25519MLKEM768 にも使える | curve25519-sha256 だけ（RFC 8731、費用は 0 だが OpenSSH が警告を出す）。決めるのはユーザー（U3） |
| D20 | **WS336 の cipher**: chacha20-poly1305@openssh.com（`EVP_chacha20` の 64 bit の counter と nonce、`EVP_MAC` の Poly1305）。ML-KEM の ek の取り出しは `EVP_PKEY_get_raw_public_key`（OpenSSL 3.5 の `doc/man3/EVP_PKEY_new.pod:194-200` が ML-KEM-768 に効くと書く）、`OSSL_PARAM`・`core_names.h` は作らない | 要る関数を最小に。`OSSL_PARAM` の struct は型として完全に置くが、配列の引数は NULL だけ | aes256-gcm@openssh（AES-GCM は内部に有るので後で安く足せる） |

## 4. 構成

```
libcrypto-compat（userland/base/libcrypto-compat/、公開: include/libc/compat/crypto/*.h）
  公開の API の層   api-err.c（ERR_・RAND_bytes・OPENSSL_cleanse・CRYPTO_memcmp）
                    api-x509.c（d2i_X509・X509_free・X509_STORE_add_cert・X509_VERIFY_PARAM_set1_*・X509_verify_cert_error_string、内部の store の生成）
                    api-evp.c（EVP_sha256・EVP_Digest*・EVP_chacha20・EVP_Cipher*・EVP_MAC*）
                    api-pkey.c（EVP_PKEY: Ed25519・X25519・ML-KEM-768、DigestSign/Verify、derive、encapsulate）
                    cryc-api.h（公開の型の中身、libssl-compat も読む）
  証明書            cryc-chain（store と path validation）← cryc-name・cryc-sigver ← cryc-x509 ← cryc-der・cryc-pem
  公開鍵            cryc-ecc ← cryc-ecp ← cryc-bn、cryc-rsa ← cryc-bn、cryc-ed25519・cryc-x25519 ← cryc-f25519、cryc-mlkem ← cryc-sha3
  対称              cryc-gcm ← cryc-aes、cryc-chacha、cryc-hash ← cryc-sha2
  土台              cryc.h（約束・定数時間の補助・byte 順）、cryc-os（posix・win32）、cryc-err（thread ごとの queue）
libssl-compat（userland/base/libssl-compat/、公開: include/libc/compat/ssl/ssl.h）
  api-ssl.c ← sslc-conn ← sslc-hs（hs.c・hs13.c・hs12.c）← sslc-msg・sslc-keysched ← sslc-record ← sslc-tls・sslc-buf
  （下は libcrypto-compat の cryc_* を private な version node で呼ぶ）
```

- 依存は一方向。libcrypto-compat は libssl-compat の名前を一切含まない（check-skeleton.sh の 4 が照合）。
- header の数: libcrypto-compat 24（内部）＋6（公開）、libssl-compat 8（内部）＋1（公開）。.c は 29＋10。

### 4.1 WS336（ssh）に向く口（ssh-facing surface）

自前の ssh・sshd は **公開の API だけ**を使う（`<compat/crypto/evp.h>`・`rand.h`・`crypto.h`、link は libcrypto-compat）: 
SHA-256（`EVP_sha256`・`EVP_Digest*`・`EVP_MD_CTX_copy_ex`）、Ed25519（`EVP_PKEY_ED25519` の raw の鍵、`EVP_DigestSign`・`EVP_DigestVerify`）、X25519（`EVP_PKEY_X25519`、`EVP_PKEY_derive`）、
ML-KEM-768（`"ML-KEM-768"`、keygen・encapsulate・decapsulate）、ChaCha20（`EVP_chacha20`、IV の 16 byte = 状態の語 12〜15、64 bit の counter）、Poly1305（`EVP_MAC_fetch("POLY1305")`）、
`RAND_bytes`・`OPENSSL_cleanse`・`CRYPTO_memcmp`。中の module は `cryc-sha2`・`cryc-ed25519`・`cryc-x25519`・`cryc-f25519`・`cryc-mlkem`・`cryc-sha3`・`cryc-chacha`（`cryc_chacha20_xor64`）・`cryc-os`。
primitive の header に TLS の型は無い（TLS の型は libssl-compat の `sslc-*.h` だけ）。

### 4.2 net/tls.c の本家と compat の切り替え（build の時）

net/tls.c は今 header を使わず `dlopen` で名前を開く（§2.1）。**推し（U2 の案 A）**: libbrowser の Makefile が `-DWB_TLS_COMPAT` を付けた build では、開く名前を
`libcrypto-compat.so`・`libssl-compat.so`（Windows は `libcrypto-compat.dll`・`libssl-compat.dll`）にする。関数の名前・signature・定数は同じなので、それ以外の code は変わらない。
zedBSD の base の build は compat、Linux の host の試験（WS074 の host-build、本家の OpenSSL が入っている）は既定で本家、`-DWB_TLS_COMPAT` と `LD_LIBRARY_PATH` で compat も試せる。
案 B: `dlopen` をやめ、`#ifdef WB_TLS_COMPAT` で `<compat/ssl/ssl.h>` か `<openssl/ssl.h>` を include して直に link する（libbrowser が base の libssl-compat に依存する。package の無い時の EPROTONOSUPPORT の道が消える）。
切り替えの変更は net/tls.c（WS074 の所有）に入るので、**切り替え自体は WS335 の最後の提案の Phase（p036、ユーザーの承認と WS074・WS334 との調整の後）**。

### 4.3 build

- zedBSD: `platform/common/userland-dynamic.mk` に `libcrypto-compat.so`（`--version-script=userland/base/libcrypto-compat/exports.map`、公開の名前は `LIBCRYPTO_COMPAT` の node、`cryc_*` は `LIBCRYPTO_COMPAT_PRIVATE`）と
  `libssl-compat.so`（`--needed libcrypto-compat.so`）の規則、`userland/base/*/Makefile` に package の登録（既定 n）。**p001 では登録しない**: 骨格の .so を image に入れず、共有の
  `userland-dynamic.mk` に並行の Phase が触らないため。登録は統合の Phase（p033）。p001 の build の確かめは check-skeleton.sh（host の cc・clang、zedBSD の clang で x86_64 と aarch64 の header、llvm-mingw で x86_64 と aarch64）。
- Windows: `userland/base/libcrypto-compat/sources.mk`・`userland/base/libssl-compat/sources.mk`（変数だけ）を p033 で作り、WS334 の `apps/browser-win32/Makefile` と単独の `make libssl-compat-win64`（x86_64）・`-winarm64` が使う。
  link: libcrypto-compat は `-lbcrypt -lcrypt32 -lws2_32`。UCRT。`.def` は exports.map から。

## 5. 公開の API の部分集合（全部、OpenSSL 3.5 の signature）

| header | 関数・定数 | 使う側 |
| --- | --- | --- |
| compat/ssl/ssl.h | `TLS_client_method`、`SSL_CTX_new`/`free`/`set_verify`/`load_verify_locations`/`set_default_verify_paths`/`set_options`/`ctrl`/`get_cert_store`、`SSL_new`/`free`/`set_fd`/`ctrl`/`set1_host`/`get0_param`/`connect`/`read`/`write`/`get_error`/`pending`/`shutdown`/`get_verify_result`、macro `SSL_set_tlsext_host_name`・`SSL_CTX_set_min/max_proto_version`、定数（`SSL_ERROR_*`・`SSL_VERIFY_*`・`SSL_CTRL_*`・`TLS1_2/3_VERSION`・`SSL_OP_*`・`SSL_R_*`） | net/tls.c（＋WS334） |
| compat/crypto/err.h | `ERR_get_error`・`ERR_peek_error`・`ERR_error_string_n`・`ERR_clear_error`、`ERR_GET_LIB`・`ERR_GET_REASON`、`ERR_LIB_*` | net/tls.c |
| compat/crypto/x509.h | `d2i_X509`・`X509_free`・`X509_STORE_add_cert`・`X509_VERIFY_PARAM_set1_host`・`X509_VERIFY_PARAM_set1_ip_asc`・`X509_verify_cert_error_string`、`X509_V_*` | net/tls.c（＋WS334） |
| compat/crypto/evp.h | §4.1 の 45 関数と `EVP_PKEY_X25519`・`EVP_PKEY_ED25519`・`EVP_MAX_MD_SIZE` | WS336 |
| compat/crypto/rand.h・crypto.h | `RAND_bytes`、`OPENSSL_cleanse`・`CRYPTO_memcmp` | WS336 |
| compat/crypto/types.h | 型（OpenSSL の tag）、`OSSL_PARAM`（完全な struct、引数は NULL だけ） | 全部 |

- 範囲の外の引数（別の ctrl、ENGINE・OSSL_LIB_CTX・propq が NULL でない、別の algorithm）は失敗（誤りを queue）か無視（未知の `SSL_OP_` bit は記録だけ）。各関数の comment に書いた。
- 振る舞いの詳細の正は OpenSSL 3.5.8 の `doc/man3/*.pod`（data として読む、文を写さない）。header の comment が食い違う時は header が正で、実装の担当は報告する。

## 6. 安全の設計

- **秘密の定義と定数時間**: `cryc.h` の冒頭（秘密の一覧、分岐・index・除算の禁止、`cryc_ct_*`）。各 header が何が秘密かを書く。確かめは (1) review、(2) [tests/run-ct.sh](tests/run-ct.sh)（Valgrind memcheck で秘密を undefined にして分岐・address への使用を検出、ctgrind の考え方）を AES・GCM・ChaCha20/Poly1305・HMAC/HKDF・X25519・P-256 の scalar 倍と ECDH・Ed25519 の署名・ML-KEM の decaps・record の open・tag と Finished の比較に。
- **消去**: 秘密を持つ struct は `_clear`・`_free`・`_final` が `cryc_wipe`。local の配列も return の前に。失敗した出力も消す（AEAD の open の失敗は平文の場所を 0 に）。公開の `EVP_*_free`・`SSL_free` も消す。
- **乱数**: `cryc_os_random`（§3 D15）。鍵の生成・ClientHello の random・session id・ML-KEM の m・d・z。失敗は EIO → handshake の失敗（internal_error）。
- **record 層と padding oracle**: AEAD だけ（D7）。tag の失敗は区別しない EBADMSG → bad_record_mac。1.3 の内側の padding の除去は長さだけに依る。sequence の wrap は close。
- **handshake**: downgrade の印、HRR の 2 回目の拒否、unsolicited な拡張の拒否、ServerKeyExchange と CertificateVerify の署名の範囲（offer した scheme・鍵との一致）、Finished の定数時間の比較、x25519 の全零・P-256 の点の検証（SP 800-56A 5.6.2.3.3）、ChangeCipherSpec の制限、進まない record の数の上限（DoS）。
- **parser**: DER は strict、長さは残りと上限で照合、path 構築の仕事の上限、handshake message の上限 100 KiB、PEM と file の上限。全部 ASan・UBSan の単体試験と、統合の後の fuzz（libFuzzer、DER・X.509・PEM・ServerHello・Certificate・record の open・ML-KEM の decaps）。
- **ML-KEM**: decaps の暗黙の拒否（FIPS 203 Algorithm 18）、ek の modulus check（7.2）、dk の hash check（7.3）。
- **review**: 暗号の code は追加の review が要る。p034「安全の review」（Opus、敵対的、上の確かめを全部流し、fuzz を回し、不足を直す）を統合の後、規約の見直しの前に置く。

## 7. 所有・寿命・並行

- 公開の object（`SSL_CTX`・`SSL`・`X509`・`X509_STORE`・`EVP_PKEY` ほか）は heap。参照の数は `cryc_os_atomic_add`。`SSL` は `SSL_CTX` の参照、`SSL_CTX` は `X509_STORE` の参照を持つ。
- `SSL_CTX` の設定と store は mutex の下（net/tls.c は 1 つの context を 2 つの thread で使う）。store の entry は動かない pointer（成長しても中身は動かない、`cryc-chain.h`）。
- `SSL` の中の `sslc_conn` は 1 thread ずつ。handshake の状態は `sslc_hs`（peer の証明書は複写を持つ）。誤りの queue は thread ごと（`cryc_os_thread_*`）。
- 内部の module は global の状態を持たない（`cryc-err` の thread ごとの queue と `cryc-os` の key を除く）。

## 8. 試験の計画

| 段 | 何で | どこで |
| --- | --- | --- |
| 単体（各 Phase） | [run-unit.sh](tests/run-unit.sh)（ASan・UBSan、`-std=c89`）。期待の値は仕様の表・付録と公式の vector（CAVP・Wycheproof・RFC の付録・RFC 8448・x509-limbo）と手の計算だけ。他の実装の出力は使わない。逆向きの独立の道具: [derwriter.h](tests/unit/derwriter.h)（DER の組み立て）、record・handshake の相手の bytes は RFC 8448 の値 | host |
| 定数時間 | [run-ct.sh](tests/run-ct.sh)（`ct-<名前>.c`） | host |
| 骨格 | [check-skeleton.sh](tests/check-skeleton.sh)（header の単独の compile、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、層の照合、`BASE=`） | host |
| 統合（p033） | (1) RFC 8448 の trace を handshake に流し bytes 一致、(2) x509-limbo の SERVER 9,792 件（期待と一致の数、違いは理由の表）、(3) host の OpenSSL `s_server` との相互（1.2・1.3 × suite × x25519/P-256（HRR）× ECDSA/RSA × 断片化・非 blocking）、(4) net/tls.c の host の試験（`plan/ws074/tests/host-loader.c` の系）を `-DWB_TLS_COMPAT` 相当（試験用の名前の symlink）で compat に通す、(5) 実の site の一覧（Let's Encrypt の RSA と ECDSA、P-384 の中間、Google・Cloudflare・GitHub ほか 20、host から） | host |
| 統合（p033） | zedBSD の image で `fetch`…は無いので試験の program（`plan/ws335/tests/tls-client.c`）を QEMU の guest で走らせ、host の `s_server` に繋ぐ（SSH で操作、log で判定しない） | QEMU（Q1 の割当） |
| 統合（p033） | Windows の build（llvm-mingw x86_64・aarch64）で `.dll` と試験の `.exe` を作る。実行は WS334 の Windows の試験の機械が揃ってから（未実施として残す） | cross |
| 安全（p034） | 定数時間の全部、fuzz（各 1 時間、夜はユーザーに確かめてから）、review の checklist | host |

## 9. 実装の Phase（p002〜p036）

各 phase.md が手順（読む物・作る物・試験・注意・変えてはいけない物・確かめの command・終わりの条件）。依存は main に merge され cleared の物。LW は目安。

| Phase | module（header） | 単体試験と vector | 依存 | 担当 |
| --- | --- | --- | --- | --- |
| p002 | 土台・SHA-2・hash/HMAC/HKDF（cryc.h・cryc-sha2.h・cryc-hash.h） | test-util・test-sha2（CAVP SHA byte）・test-hash（RFC 4231・RFC 5869 付録 A・Wycheproof hmac/hkdf） | — | Sonnet |
| p003 | SHA-3・SHAKE（cryc-sha3.h） | test-sha3（CAVP SHA-3・SHAKE） | p002 | Sonnet |
| p004 | AES（cryc-aes.h） | test-aes（FIPS 197 付録 C、CAVP KAT_AES の ECB 相当、S-box の 256 個を Table 4） | p002 | Sonnet |
| p005 | GCM（cryc-gcm.h） | test-gcm（CAVP gcmEncryptExtIV・gcmDecrypt、Wycheproof aes_gcm） | p004 | Sonnet |
| p006 | ChaCha20・Poly1305（cryc-chacha.h） | test-chacha（RFC 8439 2.3.2〜2.8.2 と付録 A、Wycheproof chacha20_poly1305） | p002 | Sonnet |
| p007 | bignum（cryc-bn.h） | test-bn（手の計算の値、P-256 の p と n での Montgomery の性質、乱数の入力では 2 通りの計算の一致（(a·b)·c と a·(b·c)、Montgomery の積と from_mont の往復）） | p002 | Sonnet |
| p008 | f25519・X25519（cryc-f25519.h・cryc-x25519.h） | test-x25519（RFC 7748 5.2・6.1、Wycheproof x25519） | p002 | Sonnet |
| p009 | Ed25519（cryc-ed25519.h） | test-ed25519（RFC 8032 7.1、Wycheproof ed25519） | p008 | Sonnet |
| p010 | P-256・P-384 の群（cryc-ecp.h） | test-ecp（SP 800-186 の G・n G = ∞、CAVP PKV、affine の参照の式との一致を試験の側に独立に） | p007 | Sonnet |
| p011 | ECC: keygen・ECDH・ECDSA verify（cryc-ecc.h） | test-ecc（CAVP ecccdh・SigVer、Wycheproof ecdsa・ecdh） | p010 | Sonnet |
| p012 | RSA verify（cryc-rsa.h） | test-rsa（CAVP SigVer15・SigVerPSS、Wycheproof rsa_signature・rsa_pss） | p007・p002 | Sonnet |
| p013 | ML-KEM の多項式（cryc-mlkem.h の poly、cryc-mlkem-poly.c） | test-mlkem-poly（NTT の往復、zeta の表を FIPS 203 付録 A と 2 通りで照合、ByteEncode/Decode の往復、Compress の式） | p003 | Sonnet |
| p014 | ML-KEM-768（cryc-mlkem.c） | test-mlkem（Wycheproof mlkem_768 の keygen_seed・encaps・decaps・test） | p013 | Sonnet |
| p015 | OS（cryc-os.h、posix と win32） | test-os（posix: 乱数・時刻・mutex・thread の値・file・socketpair の読み書き）、win32 は compile と link まで | p002 | Sonnet |
| p016 | DER・PEM（cryc-der.h・cryc-pem.h） | test-der（derwriter で正と壊れた入力、X.690 の例）・test-pem（RFC 7468・4648 の例、ca-certificates の bundle を全部読む） | p002 | Sonnet |
| p017 | X.509 の parse（cryc-x509.h） | test-x509（ca-certificates の全部、x509-limbo の証明書の parse、derwriter の壊れた拡張） | p016 | Sonnet |
| p018 | 署名の確かめ（cryc-sigver.h） | test-sigver（ca-certificates の自己署名の確かめ、Wycheproof の DER の ECDSA） | p017・p011・p012 | Sonnet |
| p019 | 名前（cryc-name.h） | test-name（RFC 9525 の例、name constraints の表、x509-limbo の名前の件） | p017 | Sonnet |
| p020 | store と path validation（cryc-chain.h） | test-chain（x509-limbo の SERVER 件の期待との一致の数を記録、OpenSSL で作った試験の CA） | p018・p019・p015 | Sonnet |
| p021 | 誤りの queue と ERR・RAND・cleanse・memcmp（cryc-err.h、api-err.c） | test-err（thread ごと、満杯、文字列の形） | p015 | Sonnet |
| p022 | 公開の X509（api-x509.c） | test-api-x509 | p020・p021 | Sonnet |
| p023 | 公開の EVP の digest・cipher・MAC（api-evp.c） | test-api-evp（OpenSSL の man の約束、ssh の chacha20-poly1305@openssh の packet の例は PROTOCOL.chacha20poly1305 から手で） | p021・p006・p002 | Sonnet |
| p024 | 公開の EVP_PKEY（api-pkey.c） | test-api-pkey（RFC 8032・7748 の値、Wycheproof mlkem を EVP 経由で） | p021・p009・p008・p014 | Sonnet |
| p025 | TLS の定数・suite の表・buf（sslc-tls.h・sslc-buf.h） | test-tls・test-buf | p002 | Sonnet |
| p026 | record（sslc-record.h） | test-record（RFC 8448 の暗号化された record を開く・同じ bytes に閉じる、壊した tag・長さ） | p025・p005・p006 | Sonnet |
| p027 | 鍵の schedule と transcript（sslc-keysched.h） | test-keysched（RFC 8448 の全部の secret・key・iv、HRR の section 5、1.2 の PRF は HMAC の合成を式どおりに） | p026 | Sonnet |
| p028 | handshake の message（sslc-msg.h） | test-msg（RFC 8448 の ClientHello・ServerHello・EE・Certificate・CV・Finished の bytes、壊した拡張の alert） | p025・p020 | Sonnet |
| p029 | handshake の共通と TLS 1.3（sslc-hs.c・sslc-hs13.c） | test-hs13（RFC 8448 section 3 を固定の乱数で流し、送る bytes と鍵が一致、HRR は section 5） | p027・p028・p020・p011・p008 | Sonnet（重い。Q1 が Opus に替えてもよい） |
| p030 | TLS 1.2（sslc-hs12.c） | test-hs12（RFC 5246・8422 の式から試験の側で作った server の message、EMS・RI・downgrade の印） | p029 | Sonnet |
| p031 | 接続（sslc-conn.h） | test-conn（memory の pipe の transport で 1 byte ずつ・EAGAIN・alert・close_notify・EOF） | p030・p015 | Sonnet |
| p032 | 公開の SSL（api-ssl.c、sslc-api.h） | test-api-ssl（SSL_get_error の表、ctrl、非 blocking の繰り返し） | p031・p022 | Sonnet |
| p033 | 統合: zedBSD の .so と exports、Windows の DLL、§8 の統合の試験 | §8 | p032・p023・p024 | Opus |
| p034 | 安全の review と hardening | §6・§8 | p033 | Opus |
| p035 | 規約の全文の見直しと header の持ち越しの直し | style-check、全試験が前と同じ | p034 | conformance-reviewer |
| p036（提案、要承認） | net/tls.c の切り替え（§4.2）と Windows の名前、WS074・WS334 と調整 | WS074 の host 試験、QEMU の browser の https | p033 | 未定 |

- 並べられる組（依存が満ちた順）: p002 の後に p003・p004・p006・p007・p008・p015・p016・p025 を並べて（Q1 の目安の 3〜5 ずつ）。
- 規模の目安: 各 Phase は Sonnet の 1 回の起動（header 1〜3 個、仕様の 1〜2 節）。p029・p030・p033・p034 は重い。

## 10. 範囲の外の入力の扱い

- TLS: 範囲の外の版・suite・group・scheme を server が選べば、その version に合う alert（protocol_version・illegal_parameter・handshake_failure）で止まる。未知の拡張は unsupported_extension。
- 証明書: 範囲の外の鍵・署名（SHA-1・Ed25519・P-521・RSA-PSS の鍵）を path に要すれば `X509_V_ERR_UNSUPPORTED_SIGNATURE_ALGORITHM`・`EE_KEY_TOO_SMALL` ほか。trust store には読めて載る（使えないだけ）。
- 公開の API: 範囲の外の引数は失敗（0・NULL）と誤りの queue（`ERR_R_UNSUPPORTED` ほか）。

## 11. ユーザーと Q1 への問い

[user-questions.md](user-questions.md)（U1〜U6）。

## 12. design-reviewer

[design-review.md](design-review.md)（指摘の全部と、反映か見送りの理由）。header の凍結（基点の SHA）は反映の後の commit。
