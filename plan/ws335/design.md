<!-- awesome-plan project=zedbsd record=ws335-design -->

# WS335 libcrypto-compat と libssl-compat の設計（p001、標準廉価実装手順 §4）

- 作成: 2026-11-01（P6、q1789-i01）。親: [ws.md](ws.md)。手順: [標準廉価実装手順](../../docs/agent/spec-module-delegation.md)。
- 範囲は 2026-11-01 のユーザーの決定（[ws.md](ws.md) の「ユーザーの決定」）による: **libbrowser の `net/tls.c` が使う物と、自前の ssh・sshd（[WS336](../ws336/ws.md)）が使う物の和**だけを、
  OpenSSL 3.5 と互換の API で作る。libcrypto-compat（暗号・証明書・誤りの queue）と、その上の libssl-compat（TLS の client）の 2 つの library。
- 状態: header・.c の骨格・試験の道具を置き、design-reviewer の指摘を反映して凍結した（最初の基点 60760d2a0）。2026-11-01 q1789-i02 で U1〜U6 の答え（全部 A、Windows は DLL を作らず 1 つの .exe への static link）を入れ、
  header と試験の道具を直して凍結し直した: **基点 NEWBASE**。変えた所は §3 の D23〜D27、§4.2〜§4.4、§8、§9、§13。

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
- **WS334（Windows の OmniGrammaton）の設計**（[plan/ws334/design.md](../ws334/design.md) §6.6 S-A、2026-11-01 の形）: 当面は OpenSSL の DLL を実行時に開く（決定 16）。Windows では `SSL_CTX_set_default_verify_paths` を呼ばず、
  Windows の ROOT の store の DER を `d2i_X509`・`SSL_CTX_get_cert_store`・`X509_STORE_add_cert`・`X509_free` で入れ、Disallowed の証明書を検証の callback で鎖の全部から拒む（WS334 p030）。O7（DLL の名前）、O8（`SSL_set_fd` に SOCKET）、O32（`errno` と WinSock）。
  信頼する根は packages の ca-certificates と Windows の store（WS334 の決定 18）。
- **2026-11-01 のユーザーの決定**（[ws.md](ws.md)）: Windows では DLL を作らず、OmniGrammaton は 1 つの .exe（libbrowser も static）。libcrypto-compat・libssl-compat は static library（`.a`）で .exe に link する。残る DLL は OS の物（Vulkan の loader `vulkan-1.dll` など）だけ。WS334 の build の変更は WS334 の担当（P7）が行う。
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

## 3. 範囲と決定（D1〜D28）

| # | 決定 | 理由 | 選ばなかった案 |
| --- | --- | --- | --- |
| D1 | **2 つの library**: `libcrypto-compat`（`userland/base/libcrypto-compat/`）と `libssl-compat`（`userland/base/libssl-compat/`）。zedBSD は `/lib/libcrypto-compat.so`・`/lib/libssl-compat.so`（SONAME も同じ）、Windows は static library の `libcrypto-compat.a`・`libssl-compat.a` だけ（D23） | ユーザーの決定（libssl-compat の上下の 2 つ）。本家の `libssl.so`・`libcrypto.so`（package の `/usr/lib`）と名前が衝突しない。base に置くのは base の program（後の fetch・libpasskey・WS336 の ssh）が使えるため | 本家の名前で入れる（`/lib` が `/usr/lib` より先に探されるので、本家に link した curl・python が壊れる）。1 つの library（ユーザーの決定と違う） |
| D2 | **header は `include/libc/compat/crypto/`（types・err・x509・evp・rand・crypto）と `include/libc/compat/ssl/ssl.h`**。program は `<compat/crypto/evp.h>`・`<compat/ssl/ssl.h>` と書く | ユーザーの決定（他の compat と同じ `include/libc/compat/<名前>/`、本家と分ける）。png・zlib の前例（`#include <compat/png/png.h>`、`libbrowser/image/png.c:15`） | `include/libc/compat/openssl/`（P5 の案。1 つの dir に 2 つの library の header が混ざり、`<名前>` が library の名前でなくなる） |
| D3 | 公開の名前・型の tag・数値・戻り値の約束は **OpenSSL 3.5 の物**（値は block ごとに「Values referenced: OpenSSL 3.5.8 include/openssl/…」）。中身の code・comment・構造は写さない。内部の名前は独自（`cryc_`・`sslc_`） | 互換が目的。libz-compat（`include/libc/compat/zlib/zlib.h`）の前例と同じ扱い | OpenSSL の内部の名前（`ossl_`・`tls13_`）に似せる（Guardrail に反する） |
| D4 | **内部の口**: libcrypto-compat は内部の module の関数（`cryc_*`）を、libssl-compat のためだけの private な version node（`LIBCRYPTO_COMPAT_PRIVATE`、約束なし）で export する。libssl-compat は TLS の内部で `cryc_*`（hash・AEAD・鍵の交換・X.509・誤りの queue）を直に呼ぶ。Windows では 2 つの .a が同じ .exe に入るので version node は無く、名前の約束（D25）で守る | 誤りの queue と trust store を 1 つにする（program から見て OpenSSL と同じく 1 つの ERR の queue）。TLS の record に EVP の層を挟まない | libssl-compat に暗号を静的に複写（queue が 2 つになる）。libssl-compat も公開の EVP だけを使う（AEAD・HKDF・X.509 の EVP が要り範囲が膨らむ） |
| D5 | **TLS は client だけ**。server は作らない | 使う側（net/tls.c）は client だけ。相手の試験は host の OpenSSL の `s_server` で足りる | server も（試験のためだけの大きな code） |
| D6 | **版**: TLS 1.3（RFC 8446）と TLS 1.2（RFC 5246）。1.0・1.1 は無し（RFC 8996）。1.3 を offer した時の 1.2 への downgrade の印を確かめる（4.1.3） | 1.2 だけの server が今も多い。net/tls.c は 1.2 以上を求める（`:768`） | 1.3 だけ（届かない site が残る） |
| D7 | **suite**: 1.3 の 3 つ（AES-128-GCM・AES-256-GCM・ChaCha20-Poly1305）、1.2 は ECDHE-ECDSA/RSA × AES-GCM/ChaCha20-Poly1305 の 6 つ。CBC・RSA の鍵の輸送・DHE・static ECDH は無し | AEAD だけなので padding oracle（Lucky13・POODLE）の類が成り立たない（RFC 9325 4.2 の推し） | CBC の suite（古い server の互換、定数時間の MAC-then-encrypt が要る） |
| D8 | **鍵の交換（TLS）**: x25519 と secp256r1。ClientHello は x25519 の share 1 つ、server が P-256 を望めば HelloRetryRequest で応える。X25519MLKEM768（hybrid）は範囲の外 | 1 つの share で handshake の計算が最小。HRR は RFC の必須 | 2 つの share を常に送る（毎回 P-256 の鍵の生成が要る） |
| D9 | **署名（TLS と証明書）**: ECDSA P-256・P-384（SHA-256/384/512）、RSA-PSS（rsae、SHA-256/384/512）、RSA PKCS#1 v1.5（1.2 と証明書だけ）。Ed25519・P-521・SHA-1 は証明書の署名として不可。RSA は 2048〜8192 bit | web PKI の現実（Let's Encrypt の E5/E6 は P-384） | SHA-1 の中間（RFC 9155 で廃止） |
| D10 | **SNI** は送る（host 名の時だけ、address では送らない、RFC 6066 3）。**ALPN は範囲の外**（net/tls.c が使わない） | ユーザーの決定 1 | ALPN（HTTP/2 を作る時の後の候補） |
| D11 | **証明書の確かめ**: RFC 5280 6.1 の path validation（policy の処理は無し、critical の policy の拡張は認識して通す）、path は RFC 4158 の流儀の深さ優先で組み、仕事の量（署名の確かめ 64 回）と深さ（10）を限る。name constraints（dNSName・iPAddress・directoryName・rfc822Name）を適用。trust anchor の有効期限は確かめる（OpenSSL と同じ） | web PKI の client の profile。x509-limbo で照合 | policy tree の全部（web PKI で使われず大きい） |
| D12 | **host 名の照合** は RFC 9525（6125 の後継）: SAN の dNSName だけ、wildcard は最も左の label の全体だけ、**CN への fallback は無し**。address は SAN の iPAddress | RFC 9525 6.3。CA/B の証明書は全部 SAN を持つ。試験の CA（`plan/tools/mail/run-host-mail-backend.sh:29`、`plan/ws074/tests/make-test-ca.sh`）も SAN を持つ | OpenSSL の既定（SAN の DNS が無い時は CN を見る） |
| D13 | **失効（OCSP・CRL）・session の再開・0-RTT・PSK・client 証明書・renegotiation・post-handshake auth・compression・heartbeat・record_size_limit・ECH は範囲の外**。NewSessionTicket は読んで捨てる、1.2 の HelloRequest には warning の no_renegotiation | OpenSSL の既定も失効を見ない。範囲を net/tls.c に限る決定 | resumption（毎回 full handshake の費用。HTTP の keep-alive が補う。後の候補） |
| D14 | **trust store**: zedBSD は `/etc/ssl/cert.pem`（ca-certificates の package、`userland/packages/security/ca-certificates/Makefile:10`）。Windows は `SSL_CTX_set_default_verify_paths` が Windows の ROOT（current user と local machine）から Disallowed を除いた物と、**実行された .exe の隣**の `cert.pem` を読む（U5 の A、WS334 の決定 18）。Disallowed の証明書は store に「信頼しない」印として残り、鎖のどこに出ても拒む（D24）。`SSL_CTX_get_cert_store`＋`X509_STORE_add_cert` の道（WS334 S-A）も持つ | 使う側が両方の OS で同じ呼び方にできる | Windows の検証（`CertGetCertificateChain`）を使う（挙動が OS で分かれる） |
| D15 | **乱数** は毎回 OS から（zedBSD `getrandom`、Windows `BCryptGenRandom`）。user 空間の DRBG は持たない | 単純で fork に安全。失敗は誤り（fallback なし） | DRBG（速いが fork・再 seed の扱いが要る） |
| D16 | **定数時間**: 秘密の値で分岐・表引き・除算をしない。AES は bitsliced（S-box は GF(2^8) の逆元と affine を論理演算で）、GHASH は SP 800-38D の Algorithm 1 を mask で、P-256 の scalar 倍は固定 window と全部の entry を読む選択、X25519 は ladder と cswap、ML-KEM は Barrett/Montgomery の還元と mask の比較 | 時間の side channel を消す。表の AES（T-table）は cache の攻撃に弱い | AES-NI・PMULL の hardware の道（速いが intrinsics と CPU の検出が要る。後の「速さ」の候補） |
| D17 | **C は ANSI C（C89）＋`<stdint.h>`**、64 bit の定数は 32 bit の半分 2 つから。OS に依る code は `cryc-os-posix.c`・`cryc-os-win32.c` だけ | 規約（[coding-style.md](../coding-style.md) §1）。zedBSD・Linux の host・llvm-mingw（x86_64・aarch64）の全部で同じ source | — |
| D18 | **OS の差**: socket は zedBSD で `read`/`write`、Windows で `recv`/`send`（`SSL_set_fd` の int を SOCKET として、OpenSSL の流儀）、`WSAGetLastError` を errno に写す（WS334 O32）。thread ごとの値は pthread の key と Windows の FLS（FlsFree しない: static library は .exe の中で process の終わりまで map されている） | WS334 の O8・O32 と合わせる。static link なので errno・heap は program と同じ UCRT の物（DLL の境の確保と解放の問題が無い） | — |
| D19 | **WS336 の鍵の交換**: mlkem768x25519-sha256 を支持（P5 の案）。ML-KEM-768（FIPS 203）と SHA-3/SHAKE（FIPS 202）を libcrypto-compat に入れる（実装の Phase 3 つ） | in-tree の OpenSSH 10.5p1 の既定、OpenSSH 10.1 以降は PQ でない kex に警告。X25519 は TLS で作るので hybrid の残りは ML-KEM だけ。ML-KEM は後の TLS の X25519MLKEM768 にも使える | curve25519-sha256 だけ（RFC 8731、費用は 0 だが OpenSSH が警告を出す）。決めるのはユーザー（U3） |
| D20 | **WS336 の cipher**: chacha20-poly1305@openssh.com（`EVP_chacha20` の 64 bit の counter と nonce、`EVP_MAC` の Poly1305）。ML-KEM の ek の取り出しは `EVP_PKEY_get_raw_public_key`（OpenSSL 3.5 の `doc/man3/EVP_PKEY_new.pod:194-200` が ML-KEM-768 に効くと書く）、`OSSL_PARAM`・`core_names.h` は作らない | 要る関数を最小に。`OSSL_PARAM` の struct は型として完全に置くが、配列の引数は NULL だけ | aes256-gcm@openssh（AES-GCM は内部に有るので後で安く足せる） |
| D21 | **read ahead をしない**: 接続は今の record の残りだけを transport に求める（OpenSSL の既定の read_ahead off と同じ） | libbrowser は `SSL_pending()==0` の時に socket を poll する（`net/http.c:768-783`、`net/tls.c:541-553`、`net/loader.c:1838`・`:1861`）。先読みすると待ちが timeout まで止まる | 先読み（system call が減るが、SSL_pending と poll の約束が崩れる） |
| D22 | **handshake の単体試験の相手は試験の道具の scripted な server**（p037 の `tlspeer.h`）。RFC 8448 の値は record・鍵の schedule・message の parse（p026〜p028）だけに使う | RFC 8448 の ClientHello は libssl-compat の送らない拡張を含み、server の鍵は RSA-1024（CRYC_RSA_MIN_BITS 未満）なので、全体の trace は一致しえない（design-review 1） | 試験のための hook を handshake の config に足す（本番の code に試験の道を残す） |
| D23 | **Windows は static library だけ**: `libcrypto-compat.a`・`libssl-compat.a` を、使う program の .exe に link する（OmniGrammaton は 1 つの .exe）。DLL・import library・`.def`・`DllMain`・export の macro は無い。link の順は `libssl-compat.a libcrypto-compat.a -lbcrypt -lcrypt32 -lws2_32`。1 つの process に 1 組（.exe の中）なので、誤りの queue と trust store は program に 1 つ（OpenSSL と同じ見え方） | ユーザーの決定（2026-11-01、DLL を作らず 1 つの .exe）。配る file が減り、DLL の探索の順の弱さ（同じ directory に置かれた偽の DLL）が無い | DLL を `LoadLibrary` で開く（前の U2 の A の Windows の部分。ユーザーが替えた）。DLL を import library で link（同じ理由） |
| D24 | **Windows の Disallowed は鎖の全部で拒む**: `cryc_os_system_roots` が Disallowed の証明書を `add_distrusted` に渡し、store は DER の SHA-256 の印だけを持つ（`cryc_store_add_distrusted`）。完全な path の確かめの最初に（署名の前、仕事に数えない）leaf から上へ印を探し、あれば一番下の深さで `CRYC_VERIFY_REJECTED`（`X509_V_ERR_CERT_REJECTED` 28）、探索は次の path へ。ROOT の列挙の時には Disallowed と照らさない（同じ根が ROOT と cert.pem のどちらから来ても同じ REJECTED になる）。ROOT のうち拡張鍵用途の property が serverAuth を持たない物（「全部の目的で無効」を含む）は入れない | WS334 の OpenSSL の形（p030）は検証の callback で鎖の全部の証明書を Disallowed と照らす。compat に切り替えた時にこの守りを失わない（Disallowed には盗まれた中間の CA が入る）。ROOT から除くだけでは、`cert.pem` の同じ根や、信頼する根の下の Disallowed の中間を通してしまう | ROOT から除くだけ（前の形）。OpenSSL と同じく callback に毎回の証明書を渡す（`X509_STORE_CTX_get_current_cert` などの API が増え、net/tls.c に Windows だけの code が残る） |
| D25 | **名前の約束**: static でない関数・object の名前は、公開の部分集合の名前か `cryc_`（libcrypto-compat）・`sslc_`（libssl-compat）で始まる物だけ（cryc.h の冒頭）。check-skeleton の 7 が `nm` で照合する | Windows の .a は program の名前の空間を共有する（version script が無い）。補助の名前（例 `hex_encode`）が program や他の static library と衝突しない | Windows で `-fvisibility` や objcopy で名前を隠す（COFF の static library では効かない・build が複雑） |
| D26 | **Windows の build の規則は使う側に置く**: WS335 は `sources.mk`（変数だけ）を渡し、使う側（OmniGrammaton の `apps/browser-win32/Makefile`）がそれを include して `lib<名前>.a` を作る（png・jpeg・gif・z の compat と同じ形）。WS335 の試験の Windows の build は試験の script（run-unit.sh `--win32`、check-skeleton の 6、run-interop.sh `--win32`）の中 | WS334 の toolchain の探し方（`WIN32_CC`・`LLVM_MINGW`）と flag を 2 か所に持たない。前例と同じ | WS335 に `make libssl-compat-win64` の target（toolchain の探し方の重複、一番上の Makefile の変更） |
| D28 | **OpenSSL だけの関数は compat に作らない**: WS334 p030 が OpenSSL のために net/tls.c に足す `OPENSSL_init_ssl`・`X509_digest`・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error` と証明書ごとの検証の callback は、compat では要らない（D14・D24 が同じ守りを持つ）。net/tls.c の側で「OpenSSL だけ」の印を付けて compat では引かず使わない（§4.2） | compat の範囲を net/tls.c の今の要る物に保つ（ユーザーの決定 2）。callback に証明書ごとの X509_STORE_CTX を渡すと path の確かめの中身が API に出る | 4 つを compat に足す（`OPENSSL_init_ssl` の 1 を返すだけの物は安いが、残りの 3 つと callback の意味が大きい） |
| D27 | **libcrypto-compat.so を先に作る Phase（p039）**: zedBSD の .so の規則・exports・package の登録だけを、p023・p024 の後に。libssl-compat.so は p033 | WS336 の zedBSD の Phase（p021〜p024）が TLS の全部と統合を待たずに .so へ link できる（2026-11-01 Q1 の決め、P5 の案 B） | p033 で両方（WS336 が TLS の完成まで待つ） |

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

### 4.2 net/tls.c の本家と compat の切り替え（U2 の答え: A、Windows は static link）

net/tls.c は今 header を使わず `dlopen` で名前を開く（§2.1。WS334 p022 の後は `wb_os_library_open`）。

- **zedBSD（A）**: libbrowser の Makefile が `-DWB_TLS_COMPAT` を付けた build では、開く名前を `libcrypto-compat.so`・`libssl-compat.so` にする。関数の名前・signature・定数は同じなので、それ以外の code は変わらない。
  zedBSD の base の build は compat、Linux の host の試験（WS074 の host-build、本家の OpenSSL が入っている）は既定で本家、`-DWB_TLS_COMPAT` と `LD_LIBRARY_PATH` で compat も試せる。
- **Windows（D23）**: 何も開かない。net/tls.c の関数の表を、static link した関数で満たす（Windows の build だけが compile する小さな file が `<compat/ssl/ssl.h>` を include し、表の member と同じ型の 1 行の wrapper を表に入れる。関数 pointer の型の変換をしない）。
  library が無い時の EPROTONOSUPPORT の道は Windows では通らない。
- compat の下での net/tls.c の変更（D28。WS334 p030 の後の net/tls.c に対して。「開く名前だけ」では済まない）:
  1. 関数の表の各行に「OpenSSL だけ」の印を足し、`WB_TLS_COMPAT` の build ではその行を引かない（`OPENSSL_init_ssl`・`X509_digest`・`EVP_sha256`（指紋のため）・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error`）。引く名前は全部 §5 の部分集合にある。
  2. compat では `OPENSSL_init_ssl` を呼ばない（compat は設定の file を読まない）。
  3. compat では OS に依らず `SSL_CTX_set_default_verify_paths` を呼び、`wb_os_trust_roots` の DER を入れる loop と、Disallowed の指紋の一覧と、証明書ごとの検証の callback を使わない（`SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL)`、今の zedBSD の形）。
  4. Windows の static の build は 1 の表を link した関数の wrapper で満たす（上の Windows の項）。
  Q1 への頼み: WS334 p030 を始める前に、p030 の担当（P7）に 1 の印（OpenSSL だけの行が無くても TLS が動く形）を入れてもらうと、p036 の変更は 2・3 の `#ifdef` だけになる。
- 切り替えの変更は net/tls.c（WS074 の所有）と WS334 の build に入るので、**切り替え自体は WS335 の最後の提案の Phase（p036、WS074・WS334 との調整の後）**。WS334 の build は WS334 の担当（P7）が直す。

### 4.3 build

- zedBSD: `platform/common/userland-dynamic.mk` に `libcrypto-compat.so`（`--version-script=userland/base/libcrypto-compat/exports.map`、公開の名前は `LIBCRYPTO_COMPAT` の node、`cryc_*` は `LIBCRYPTO_COMPAT_PRIVATE`）と
  `libssl-compat.so`（`--needed libcrypto-compat.so`）の規則、`userland/base/*/Makefile` に package の登録（既定 n）。**p001 では登録しない**: 骨格の .so を image に入れず、共有の
  `userland-dynamic.mk` に並行の Phase が触らないため。登録は libcrypto-compat が p039（D27）、libssl-compat が p033。p001 の build の確かめは check-skeleton.sh（host の cc・clang、zedBSD の clang で x86_64 と aarch64、llvm-mingw で x86_64 と aarch64 の compile・static link・名前の範囲）。
- **source の一覧**（`sources.mk`、変数だけ。zedBSD・Linux・Windows の build が読む。libz-compat の `sources.mk` と同じ形）:

  | 変数 | 中身 | 作る Phase |
  | --- | --- | --- |
  | `LIBCRYPTO_COMPAT_SOURCES` | `userland/base/libcrypto-compat/*.c` のうち OS に依らない全部（`cryc-os-*.c` を除く） | p039 |
  | `LIBCRYPTO_COMPAT_POSIX_SOURCES` | `userland/base/libcrypto-compat/cryc-os-posix.c` | p039 |
  | `LIBCRYPTO_COMPAT_WIN32_SOURCES` | `userland/base/libcrypto-compat/cryc-os-win32.c` | p039 |
  | `LIBSSL_COMPAT_SOURCES` | `userland/base/libssl-compat/*.c`（OS に依る file は無い） | p033 |

- **Windows**（D23・D26）: 成果物は `libcrypto-compat.a`（`LIBCRYPTO_COMPAT_SOURCES` ＋ `LIBCRYPTO_COMPAT_WIN32_SOURCES`）と `libssl-compat.a`（`LIBSSL_COMPAT_SOURCES`）だけ。作るのは使う側の build（OmniGrammaton では
  `apps/browser-win32/Makefile` の `WIN32_STATIC_LIBRARY`。置き場所はその build の object の dir、`build/browser-win64/obj/libcrypto-compat.a` の類。配る zip には入らない: .exe の中に入る）。
  compile の約束: llvm-mingw（x86_64・aarch64、UCRT）、warning 0（check-skeleton の 3 が `-std=c89 -Wall -Wextra -Werror -Wpedantic` に `-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE` を付けて 2 arch で、6 が使う側と同じ `-std=gnu11 -O2` で macro 無しに x86_64 で確かめる。W の API を名指しで呼ぶ）、include は `<compat/...>` の header の dir だけ（WS334 は `include/libc/compat/*/*.h` を写した dir）。内部の header は相対の名前で読む（libssl-compat は `"../libcrypto-compat/cryc-*.h"`）ので、source の tree の形のまま compile する。
  link: `libssl-compat.a libcrypto-compat.a -lbcrypt -lcrypt32 -lws2_32`。pthread は使わない（FLS・SRWLOCK・InitOnce）。program は `SSL_set_fd` の前に `WSAStartup` を呼ぶ（OpenSSL と同じ。ssl.h）。
- zedBSD の 2 つの .so は `-z nodelete` で link する（thread ごとの値の destroy の code を、dlclose の後も map のままにする。cryc-os.h）。

### 4.4 WS335 が使う側（OmniGrammaton の Windows の build）に渡す物

WS334 の file は WS334 の担当が直す。WS335 が渡し、約束する物は次だけ。

1. `userland/base/libcrypto-compat/sources.mk`・`userland/base/libssl-compat/sources.mk`（§4.3 の変数）と、§4.3 の compile と link の約束。
2. 公開の header（`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/ssl.h`）。export・import・visibility の macro は無い（cryc.h の冒頭）。
3. 名前の約束（D25）: .a が定義する外の名前は、公開の部分集合の名前と `cryc_`・`sslc_` だけ（check-skeleton の 7）。program の名前と衝突しない。
4. 根の扱い: `SSL_CTX_set_default_verify_paths` が Windows の ROOT（Disallowed を除く）と、実行された .exe の隣の `cert.pem`（packages の ca-certificates の束を、この名前で .exe の隣に置く）を読み、Disallowed の証明書は鎖のどこでも拒む（D14・D24）。
   OpenSSL の時の `OPENSSL_init_ssl`・`X509_digest`・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error` と検証の callback での Disallowed の照合は要らない（compat はこれらの関数を持たない）。
5. 試験の道具: `run-unit.sh --win32`（.exe に static link して Wine で。flag は `-std=c89 -O1` と UBSan の trap）と run-interop.sh `--win32`（p033）。使う側の flag（`-std=gnu11 -O2`）での compile は check-skeleton の 6 が確かめるが、その flag での実行の試験は使う側の試験（WS334 の Wine の https）に残る。
6. 切り替えの時に WS334 の側で要る事（参考。決めるのは WS334 の担当）: 2 つの `sources.mk` の include と static library の追加、.exe の link への `-lbcrypt -lcrypt32 -lws2_32`、OpenSSL の DLL と `openssl` の target と Apache-2.0 の許諾の文の同梱を外す、
   ca-certificates の束を `cert.pem` として .exe の隣に（今の `share/certs/ca-certificates.pem` の読み込みを残してもよい）、net/tls.c の変更（§4.2 の 1〜4、p036）、`WSAStartup`（今の os-net の層が呼んでいる事の確かめ）。zlib の許諾の文（WS335 の source は zedBSD の Zlib）を同梱の許諾の一覧に足す。

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
| 統合（p033） | (1) RFC 8448 の値（record・鍵・message）は p026〜p028 で照合済み、(2) x509-limbo の全件は p038、(3) host の OpenSSL `s_server` との相互（1.2・1.3 × suite × x25519/P-256（HRR）× ECDSA/RSA × 断片化・非 blocking）、(4) net/tls.c の host の試験（`plan/ws074/tests/host-loader.c` の系）を `-DWB_TLS_COMPAT` 相当（試験用の名前の symlink）で compat に通す、(5) 実の site の一覧（Let's Encrypt の RSA と ECDSA、P-384 の中間、Google・Cloudflare・GitHub ほか 20、host から） | host |
| 統合（p033） | zedBSD の image で `fetch`…は無いので試験の program（`plan/ws335/tests/tls-client.c`）を QEMU の guest で走らせ、host の `s_server` に繋ぐ（SSH で操作、log で判定しない） | QEMU（Q1 の割当） |
| 各 Phase | `run-unit.sh --win32 <名前>`: 同じ単体試験を llvm-mingw の x86_64 で 2 つの static library に link した .exe にし、Wine で走らせる（UBSan の trap。LLP64 の `long` の誤り、Windows の OS の層） | host の Wine |
| 骨格 | check-skeleton の 6・7: x86_64・aarch64 の static library と、全部の object を取り込む .exe の link、`nm` で名前の範囲 | cross |
| 統合（p039） | zedBSD の `libcrypto-compat.so` の build・exports の照合（check-exports.sh）、QEMU の guest で `crypto-smoke`（SHA-256・X25519・Ed25519・RAND の公開の API） | host・QEMU（Q1 の割当） |
| 統合（p033） | Windows の 2 arch で 2 つの .a と tls-client.exe を link、x86_64 は Wine で host の `s_server` に繋ぐ（Disallowed と .exe の隣の cert.pem も）。実の Windows の機械での実行は WS334 の試験の機械が揃ってから（未実施として残す） | cross・host の Wine |
| 安全（p034） | 定数時間の全部、fuzz（各 1 時間、夜はユーザーに確かめてから）、review の checklist | host |

## 9. 実装の Phase（p002〜p039）

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
| p020 | store と path validation（cryc-chain.h） | test-chain（x509-limbo の rfc5280・pathlen・webpki・pathological の約 190 件、試験の CA） | p018・p019・p015 | Sonnet |
| p021 | 誤りの queue と ERR・RAND・cleanse・memcmp（cryc-err.h、api-err.c） | test-err（thread ごと、満杯、文字列の形） | p015 | Sonnet |
| p022 | 公開の X509（api-x509.c） | test-api-x509 | p020・p021 | Sonnet |
| p023 | 公開の EVP の digest・cipher・MAC（api-evp.c） | test-api-evp（OpenSSL の man の約束、ssh の chacha20-poly1305@openssh の packet の例は PROTOCOL.chacha20poly1305 から手で） | p021・p006・p002 | Sonnet |
| p024 | 公開の EVP_PKEY（api-pkey.c） | test-api-pkey（RFC 8032・7748 の値、Wycheproof mlkem を EVP 経由で） | p021・p009・p008・p014 | Sonnet |
| p025 | TLS の定数・suite の表・buf（sslc-tls.h・sslc-buf.h） | test-tls・test-buf | p002 | Sonnet |
| p026 | record（sslc-record.h） | test-record（RFC 8448 の暗号化された record を開く・同じ bytes に閉じる、壊した tag・長さ） | p025・p005・p006 | Sonnet |
| p027 | 鍵の schedule と transcript（sslc-keysched.h） | test-keysched（RFC 8448 の全部の secret・key・iv、HRR の section 5、1.2 の PRF は HMAC の合成を式どおりに） | p025・p002 | Sonnet |
| p028 | handshake の message（sslc-msg.h） | test-msg（RFC 8448 の ServerHello・EE・Certificate・CV・Finished の parse、壊した拡張の alert） | p025 | Sonnet |
| p029 | handshake の共通と TLS 1.3（sslc-hs.c・sslc-hs13.c） | test-hs13（tlspeer で 1-RTT・HRR・壊した message） | p027・p028・p020・p011・p008・p037 | Sonnet（重い。Q1 が Opus に替えてもよい） |
| p030 | TLS 1.2（sslc-hs12.c） | test-hs12（tlspeer の 1.2 の flight、EMS・RI・downgrade の印） | p029・p037 | Sonnet |
| p031 | 接続（sslc-conn.h） | test-conn（memory の pipe と tlspeer、1 byte ずつ・EAGAIN・alert・close_notify・EOF・先読みしない事・key change の境） | p030・p015・p037 | Sonnet |
| p032 | 公開の SSL（api-ssl.c、sslc-api.h） | test-api-ssl（SSL_get_error の表、ctrl、非 blocking の繰り返し） | p031・p022 | Sonnet |
| p039 | zedBSD の libcrypto-compat.so（規則・exports・package と image の登録、`sources.mk`）、TLS は無し（D27） | check-exports・crypto-smoke（QEMU） | p023・p024 | Sonnet |
| p033 | 統合: zedBSD の libssl-compat.so と exports、Windows の static library の一覧（libssl-compat の `sources.mk`）、§8 の統合の試験 | §8 | p032・p023・p024・p039 | Opus |
| p034 | 安全の review と hardening | §6・§8 | p033 | Opus |
| p035 | 規約の全文の見直しと header の持ち越しの直し | style-check、全試験が前と同じ | p034 | conformance-reviewer |
| p037 | 試験の道具: tlspeer.h（scripted な TLS 1.3/1.2 server、試験の側の ECDSA の署名器、ClientHello の parser） | test-tlspeer | p025・p026・p027・p011・p008 | Opus |
| p038 | x509-limbo の SERVER 全 9,792 件の照合と違いの理由の分類 | test-limbo | p020 | Opus |
| p036（提案。U2 は A で答え済み、起動は調整の後） | net/tls.c の切り替え（§4.2）: zedBSD は開く名前、Windows は static link の表。WS074・WS334 と調整 | WS074 の host 試験、QEMU の browser の https、WS334 の Wine の https | p033、WS334 の p022・p030 | 未定 |

- 起動の順（依存が満ちた物から、Q1 の目安の 3〜5 ずつ並べる）:
  1. p002
  2. p003・p004・p006・p007・p008・p015・p016・p025（p002 の後）
  3. p005（p004）・p009（p008）・p010・p012（p007）・p013（p003）・p017（p016）・p021（p015）・p027・p028（p025）
  4. p011（p010）・p014（p013）・p019（p017）・p023（p021・p006）・p026（p025・p005・p006）
  5. p018（p017・p011・p012）・p024（p021・p009・p008・p014）・p037（p026・p027・p011・p008）
  6. p020（p018・p019・p015）・p039（p023・p024。WS336 の zedBSD の Phase が待つので、6 の中で先に）
  7. p022・p038（p020）・p029（p027・p028・p020・p011・p008・p037）
  8. p030 → p031 → p032 → p033 → p034 → p035、提案の p036 は p033 と WS334 の p022・p030 の後に、WS074・WS334 と調整して
- 規模の目安: 各 Phase は Sonnet の 1 回の起動（header 1〜3 個、仕様の 1〜2 節）。p029・p030・p033・p034 は重い。
- q1789-i02 で module の順は変わらない（依存の module は同じ）。増えたのは p039（6 の段）と、p015（Windows の Disallowed と .exe の隣の file、Wine での試験）・p020（信頼しない印）・p021（Wine での thread ごとの queue）・p022（REJECTED の写し）の中身。各 Phase の確かめに `run-unit.sh --win32` が増えた。

## 10. 範囲の外の入力の扱い

- TLS: 範囲の外の版・suite・group・scheme を server が選べば、その version に合う alert（protocol_version・illegal_parameter・handshake_failure）で止まる。未知の拡張は unsupported_extension。
- 証明書: 範囲の外の鍵・署名（SHA-1・Ed25519・P-521・RSA-PSS の鍵）を path に要すれば `X509_V_ERR_UNSUPPORTED_SIGNATURE_ALGORITHM`・`EE_KEY_TOO_SMALL` ほか。trust store には読めて載る（使えないだけ）。
- 公開の API: 範囲の外の引数は失敗（0・NULL）と誤りの queue（`ERR_R_UNSUPPORTED` ほか）。

## 11. ユーザーと Q1 への問い

[user-questions.md](user-questions.md)（U1〜U6）。

## 12. design-reviewer

[design-review.md](design-review.md)（1 回目の 27 件と、q1789-i02 の変更の 2 回目の指摘の全部と、反映か見送りの理由）。header と試験の道具の凍結（基点の SHA）は反映の後の commit で、ws.md と各 phase.md の `BASE=` に書く。

## 13. q1789-i02 の変更（2026-11-01、U1〜U6 の答えの反映）

| 所 | 変えた事 |
| --- | --- |
| header | `cryc.h`（2 つの build の形、Windows は .exe への static library、名前の約束、export の macro が無い事）、`cryc-os.h`（`cert.pem` は .exe の directory、`cryc_os_system_roots` に `add_distrusted`・`CertOpenStore` の 4 つの store・拡張鍵用途、file は binary で読む、key を消さない理由と `-z nodelete`）、`compat/ssl/ssl.h` の `SSL_set_fd`（`WSAStartup`）、`cryc-chain.h`（信頼しない印 `cryc_store_add_distrusted`、`CRYC_VERIFY_REJECTED`、path の確かめの規則）、`compat/crypto/x509.h`（`X509_V_ERR_CERT_REJECTED` 28）、`compat/ssl/ssl.h`（`SSL_CTX_set_default_verify_paths` の Windows の文） |
| 骨格 | `cryc-os-posix.c`・`cryc-os-win32.c`（引数）、`cryc-chain.c`（`cryc_store_add_distrusted`）、`api-ssl.c`（comment） |
| 試験の道具 | `run-unit.sh --win32`（試験ごとの dir、Wine の prefix の扱い）、`check-skeleton.sh` の 6（static link と gnu11 の compile）・7（名前の範囲、nm の失敗と空を FAIL に）と llvm-mingw の flag、`unit/test-tools.c` の comment |
| Phase | p015・p020・p021・p022・p033・p036 の中身、新しい p039、全部の実装の Phase の基点・`--win32` の command・Windows の注意 |
| module の順 | 変わらない（p039 が 6 の段に入る） |

