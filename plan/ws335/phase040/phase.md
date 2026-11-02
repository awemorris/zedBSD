<!-- awesome-plan project=zedbsd record=ws335-p040 -->

# ws335-p040: secp384r1 の group（supported_groups で申し出る、TLS 1.2 の ServerKeyExchange の P-384 の ECDHE、TLS 1.3 の HRR の後の P-384 の key share、共有の秘密の大きさ 48）

Status: 実装済み・試験 PASS（2026-11-02 18:30 P1 q1981-i01。cleared は Q1）。前: in-progress（2026-11-02 17:43 P1）、planned（2026-11-02 P1 が q1971-i01（p033）の中で、Q1 の判断 B に従って作成。依存: p033。p034 の前）
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

2026-11-02（host の `date`、17:45〜18:30）P1（Opus、q1981-i01）。worktree `/home/awe/zedBSD-worktrees/p1-ws335`（main d6a73d451d から）。§3 の header の変更は Q1 が承認済み（ws.md「Q1 の判断（2026-11-02 17:43、p040 の header の変更）」）。

変えた物:

- `sslc-hs.c`: 申し出る group を x25519・secp256r1・secp384r1 に。`sslc_hs_make_p256` を `sslc_hs_make_ec(hs, group)` に（P-256 か P-384 の曲線を init して鍵の対を作る。1 handshake に 1 つ、別の group の 2 回目は EPROTO）。HRR は「offered で share の無い group」（secp256r1 か secp384r1）の鍵の対を作り、その share（65 か 97 byte）だけを送る。`sslc_hs_shared_secret` は group の鍵の対（`ec_group` が一致する事）で計算し、大きさを `*shared_size`（32 か 48）で返す。`SSLC_HS_P256_POINT`・`SSLC_HS_SHARED_SIZE` を除いた。
- `sslc-hs12.c`: ServerKeyExchange の secp384r1 の点は 97 byte の uncompressed（他の大きさは illegal_parameter）、`sslc_hs12_make_ec`、ClientKeyExchange は `ec_public`、premaster の大きさは shared_size。`SSLC_HS12_MESSAGE_MAX` は P-384 の点の分。
- `sslc-hs13.c`: shared の場所は `SSLC_HS_SHARED_MAX`、`sslc_tls13_handshake` に shared_size。
- header（承認済み）: `sslc-hs.h`（`struct sslc_hs` の `ec_group`・`ec_private[CRYC_ECP_MAX_BYTES]`・`ec_public[CRYC_ECP_MAX_POINT]`・`ec_public_size`・`has_ec`、`SSLC_HS_SHARED_MAX`、`sslc_hs_shared_secret` の `size_t *shared_size` と comment、sslc_hs_start の group の文）、`sslc-keysched.h`（shared_size の comment に 48）。`sslc-keysched.c` は comment だけ。
- 試験の道具: `tlspeer.h`（`config.group` に secp384r1、`p384_private`（SHA-384("tlspeer p384")）、P-384 の share と ECDH、ZERO_SHARE の 97 byte）。
- 試験: `test-hs13.c`（ClientHello の期待の supported_groups に 0x0018、CAVP KAS ECC CDH [P-384] COUNT 0 で 48 byte の共有の秘密、P-384 の鍵で P-256 の share を断る、P-384 の不正な点、HRR で secp384r1 の通しの handshake（97 byte の share、2 回目の ClientHello の byte 一致、peer の 48 byte の秘密、鍵の一致、乱数の 4 回目が 48 byte）、送らない group は 0x0019 に）、`test-hs12.c`（3 group × 3 suite × 2 曲線 × 2 client = 36 件、P-384 の点の大きさ 65・96・98 と P-256 に 97 を断る、送らない group は 0x0019）、`test-tlspeer.c`（範囲外の group を 0x0019 に）、`test-api-ssl.c`（P-384 の handshake を 1.3 で 2 件・1.2 で 1 件）、`run-interop.sh`（p033 の known limit 8 件を「繋がらなければならない」に戻し、host の group に P-384（HRR）、全部の system に「1.3 hrr secp384r1」「1.2 secp384r1」「1.2 ECDSA P-384 leaf」）。`run-sites.sh` の `ecc384.badssl.com` は p033 から期待の PASS の一覧にある。

試験（各 command に timeout。最後の行）:

| command | 結果 |
| --- | --- |
| `WS335_VECTORS=… sh plan/ws335/tests/run-unit.sh all` | test と ct の 57 本が PASS、`run-unit: PASS`（test-hs13 18663・test-hs12 17959・test-tlspeer 35583・test-api-ssl 5201 checks） |
| `WS335_VECTORS=… sh plan/ws335/tests/run-unit.sh --win32 all` | test と ct の 57 本が PASS（win32）、`run-unit: PASS`（test-hs13 18663・test-hs12 17959・test-tlspeer 35583・test-api-ssl 5576 checks） |
| `sh plan/ws335/tests/run-ct.sh all` | `run-ct: PASS`（ct-ecc は P-256 と P-384 の ECDH を前から含む） |
| `sh plan/ws335/tests/run-interop.sh` | `run-interop: PASS (139 cases, 0 of them known limits)`（1.3・1.2 × suite × {x25519, P-256, P-384} × 4 鍵、1.2 × ECDSA P-384 の leaf の全部、署名 scheme、記録・非 blocking・close の各件） |
| `sh plan/ws335/tests/run-interop.sh --win32` | `run-interop: PASS (47 cases, 0 of them known limits)`（x86_64・aarch64 の .a と .exe が warning 0、Wine の cert.pem と Disallowed の件を含む） |
| `sh plan/ws335/tests/run-limbo.sh` | `limbo-api: PASS (294562 checks)` / `run-limbo: PASS`（p033 と同じ件数、TLS 1.2 の handshake の group の変更の影響なし） |
| `sh plan/ws335/tests/run-sites.sh` | `run-sites: 28 PASS, 1 FAIL, 0 SKIP`（`ecc384.badssl.com` が PASS に変わった。残る FAIL は `sha384.badssl.com` の証明書の期限切れ、site の側） |
| `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p040 build/p040/dynamic/libssl-compat.so` | rc 0、warning 0、check-dynamic-elf 通過。`check-exports.sh` は `public 22, defined 22, cryc_ calls 47` / `PASS` |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: FAIL`。FAILED は凍結の差の 1 つだけ: `userland/base/libssl-compat/sslc-hs.h`（36 行）と `userland/base/libssl-compat/sslc-keysched.h`（3 行）、どちらも §3 の承認済みの変更。他の check（単独の compile・層・名前）は通った。BASE は Q1 が移す |
| わざと壊す（写しから戻し cmp と `git diff --quiet` で確かめた） | (m1) 1.2 の secp384r1 の点の大きさの確かめを外す → test-hs12 FAIL（9 checks）。(m2) secp384r1 の共有の秘密の大きさを 32 にする → test-hs13 FAIL（10）・test-hs12 FAIL（108、peer との鍵の不一致） |

未実施: zedBSD の QEMU の guest（run-guest.sh、今回の依頼に無い。group の変更は host・Wine で確かめた）、実の Windows の機械、aarch64 の zedBSD の .so の build（amd64 だけ）。

p035 へ: 凍結の header のうち承認の範囲の外の comment に「x25519 と secp256r1」の記述が残る: `sslc-tls.h` の file の comment（Groups: x25519 and secp256r1 for key exchange）、`sslc-msg.h:35`（key share の数の comment）。design.md の D8 の文（phase040 §4 の案、Q1 が採用）。

消してほしい path（worktree の中）: `build/p040`、`build/tmp/p040-*`、`build/tmp/ws335-*`（この Phase の run）、`build/amd64/sysroot`（make が作った）。
