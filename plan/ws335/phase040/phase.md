<!-- awesome-plan project=zedbsd record=ws335-p040 -->

# ws335-p040: secp384r1 の group（supported_groups で申し出る、TLS 1.2 の ServerKeyExchange の P-384 の ECDHE、TLS 1.3 の HRR の後の P-384 の key share、共有の秘密の大きさ 48）

Status: planned（2026-11-02 P1 が q1971-i01（p033）の中で、Q1 の判断 B に従って作成。依存: p033。p034 の前）
Parent: [WS335](../ws.md)。担当: Opus（phase-runner-high を推す。handshake の状態と鍵の大きさに触れる）。

## 0. なぜ

p033 の相互運用（`plan/ws335/tests/run-interop.sh`）で、TLS 1.2 で ECDSA P-384 の leaf を持つ OpenSSL の `s_server` に繋がらなかった（server は `no shared cipher` で handshake_failure）。
RFC 8422 5.1 により、TLS 1.2 の server は client が supported_groups に挙げた曲線の上の ECDSA の証明書しか使えない。libssl-compat は x25519 と secp256r1 だけを挙げる（design.md D8、`sslc-hs.c` の `sslc_hs_offered_groups`）。
TLS 1.3 では P-384 の leaf でも繋がる（署名の scheme で決まる）。実の site でも `ecc384.badssl.com` が handshake_failure になった（run-sites.sh の記録）。
Chrome と Firefox は x25519・secp256r1・secp384r1 を申し出るので、web の client として同じ所に繋がるように secp384r1 を足す（2026-11-02 Q1 の判断 B、ws.md の「Q1 の判断（p033）」）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4・§7・§9・§10、[coding-style.md](../../coding-style.md)。
- [design.md](../design.md) D8・D16・§6、RFC 8446 4.1.4・4.2.7・4.2.8・7.4.2、RFC 8422 5.1・5.4・5.10、SEC 1 2.3.3（`/home/awe/zedBSD-claude1/build/sources/specs/tls/`）。
- `userland/base/libssl-compat/sslc-hs.c`（`sslc_hs_offered_groups`、`SSLC_HS_SHARED_SIZE`、`sslc_hs_shared_secret`、HRR の後の key share の生成 `:1100-1125`）、`sslc-hs12.c`（`SSLC_HS12_SHARED_SIZE`・`SSLC_HS12_P256_POINT`、ServerKeyExchange の確かめ、client の key exchange）、`sslc-hs13.c`（`SSLC_HS13_SHARED_SIZE`）、`sslc-hs.h`（`struct sslc_hs` の `p256_private[32]`・`p256_public[65]`・`has_p256`）、`sslc-keysched.h:118-125`（shared_size の comment）。
- `userland/base/libcrypto-compat/cryc-ecc.h`（`cryc_ecc_ecdh` は曲線を引数に取る。P-384 の群は p010 で実装済み）。
- 試験の道具 `plan/ws335/tests/unit/tlspeer.h`（config.group は x25519 と secp256r1 だけ）。

## 2. 作る物

- 申し出る group を x25519・secp256r1・secp384r1 の順にする（ClientHello の key share は今のとおり x25519 の 1 つ）。
- TLS 1.3: HRR が secp384r1 を指せば P-384 の鍵の対を作り、2 回目の ClientHello の key share に入れる（uncompressed、97 byte）。ServerHello の share の点の確かめ（SP 800-56A 5.6.2.3.3、`cryc_ecc_ecdh` の確かめ）。
- TLS 1.2: ServerKeyExchange の named_curve が secp384r1 なら P-384 の点（97 byte）を確かめ、P-384 の鍵の対で ClientKeyExchange を作る。
- 共有の秘密の大きさを group で変える（x25519・secp256r1 は 32、secp384r1 は 48。RFC 8446 7.4.2 と RFC 8422 5.10: x 座標の全部）。`SSLC_HS_SHARED_SIZE`・`SSLC_HS12_SHARED_SIZE`・`SSLC_HS13_SHARED_SIZE` を 48 に、使う長さを group で決める。秘密の消去は今のとおり全部の大きさで。
- 試験: test-hs13・test-hs12（tlspeer に secp384r1 の server の share を足す: tlspeer.h の config.group と鍵の生成）、test-conn・test-api-ssl の該当の所。
  run-interop.sh の 8 件の「known limit」（1.2 × ECDSA P-384 の leaf: suite 3 × group 2、署名 scheme 2）を「繋がらなければならない」に戻し、`-groups P-384` の 1.3（HRR）と 1.2 の case を足す。
- 結果の節と ws.md の行。

## 3. 凍結の header の変更（Q1 の承認が要る。承認の前は実装しない）

| file | 変更の案 |
| --- | --- |
| `userland/base/libssl-compat/sslc-hs.h` | `struct sslc_hs` の `p256_private[32]`・`p256_public[65]`・`has_p256` を、曲線を問わない形に（案: `ec_group`（uint16_t）・`ec_private[CRYC_ECP_MAX_BYTES]`・`ec_public[CRYC_ECP_MAX_POINT]`・`has_ec`）。`groups[4]` は 3 つが入るので変えない。`sslc_hs_shared_secret` の comment の「32 bytes」を「group の大きさ（32 か 48）、shared は 48 byte の場所」に、引数に `size_t *shared_size` を足す（案）。 |
| `userland/base/libssl-compat/sslc-keysched.h` | `sslc_tls13_handshake` の comment の「32 for x25519, 32 for secp256r1」に「48 for secp384r1」を足す（関数は既に shared_size を取る）。 |
| `plan/ws335/tests/unit/tlspeer.h`（試験の道具、check-skeleton の凍結の外） | `config.group` に `SSLC_GROUP_SECP384R1` を、`p384_private` を足す。 |

check-skeleton の BASE はこの Phase の後に Q1 が移す。

## 4. design.md の D8 の案（p035 か Q1 が design.md に入れる）

> **鍵の交換（TLS）**: x25519・secp256r1・secp384r1 を、この順に supported_groups で申し出る。ClientHello の key share は x25519 の 1 つ。server が secp256r1 か secp384r1 を望めば HelloRetryRequest で応える（TLS 1.3）。TLS 1.2 では ServerKeyExchange の曲線に応じる。secp384r1 を申し出るのは、TLS 1.2 の server が ECDSA の証明書の曲線を client の supported_groups から選ぶため（RFC 8422 5.1。P-384 の leaf の server に繋がる）と、Chrome・Firefox と同じ所に繋がるため（2026-11-02 Q1）。X25519MLKEM768（hybrid）は範囲の外。
> 理由: 1 つの share で handshake の計算が最小。HRR は RFC の必須。選ばなかった案: 2 つの share を常に送る（毎回 P-256 の鍵の生成が要る）、secp384r1 を申し出ない（TLS 1.2 の P-384 の leaf の server に繋がらない、p033 で観測）。

## 5. 試験と受け入れ

- `run-unit.sh all`・`run-unit.sh --win32 all`・`run-ct.sh all`（P-384 の ECDH を ct-ecc に）・check-skeleton（Q1 が移した BASE で）が PASS。
- `run-interop.sh` が known limit 0 で PASS（1.2 × ECDSA P-384 の 8 件と `-groups P-384` の case を含む）、`run-interop.sh --win32` も PASS。
- run-sites.sh の `ecc384.badssl.com` が繋がる（記録。外部に依る）。
- わざと壊す確かめ: P-384 の点の確かめを外す・共有の秘密の長さを 32 に戻す、で試験が FAIL。

## 6. 結果

（未着手）
