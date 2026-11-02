<!-- awesome-plan project=zedbsd record=ws343-p002 -->

# ws343-p002 libcrypto-compat に passkey が使う OpenSSL の関数を足し、passkey-fido2・fidoctl を link の差し替えだけで OpenSSL から外す

- 親: [ws.md](../ws.md)。Queue: q1990-i01（P2、worktree `/home/awe/zedBSD-worktrees/p2-ws343`、branch `agent/p2-ws343`、基点 main ca8c38f06e、作業の途中で main 8e12d77f2b（P6 の ws336-p021）を早送りで、bba4a9ef4e（P1 の WS335 p034）を merge bf0fa13238 で取り込んだ。p034 の cryc-aes.c の二乗の buffer の消去と自分の逆の暗号は自動の merge で両方残った）。
- Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 22:18 P2。実装と host・build の確かめは全部 PASS。cleared は Q1、QEMU・実機の実行は未実施）

## 目的と範囲

- （2026-11-02 20:46）ユーザーから、passkey-fido2・fidoctl（libpasskey の crypto-openssl.c）が使う OpenSSL の関数を libcrypto-compat に足し、link を差し替えるだけで OpenSSL から外せるようにする、という趣旨の指示を受けた。
- crypto-openssl.c は変えない。変えるのは link・include の道・package の依存だけ。
- 足すのは crypto-openssl.c が使う意味だけ（P-256 の公開鍵を octet string から、DER の ECDSA-SHA256 の確かめ、P-256 の鍵の生成と公開の点の取り出し、ECDH、padding なしの AES-256-CBC、EVP_KDF "HKDF" の SHA-256）。それ以外は OpenSSL と同じ形の誤り。

## Q1 の承認（2026-11-02、q1990 の中で）

- AES の逆の暗号（cryc_aes_decrypt_blocks、FIPS 197 5.3）を cryc-aes.h/.c に足す（cryc-aes は前向きだけで、pin.c が復号を使うため）。SP 800-38A の CBC の復号の値と run-ct の試験を付ける。
- 新しい公開の header（hmac.h・kdf.h・params.h・core_names.h）、evp.h・types.h の拡張、cryc-api.h・cryc-err.h の追加、openssl-names の転送の include の dir、check-skeleton の名前の正規表現（HMAC と OSSL_PARAM_）、passkey-fido2・fidoctl の include・link・package の依存の変更。
- padding を on のままの AES-CBC は誤りを返してよい。

## 作った物

| 所 | 中身 |
| --- | --- |
| `userland/base/libcrypto-compat/cryc-aes.h`・`.c` | 逆の暗号 cryc_aes_decrypt_blocks（bitsliced、定数時間）。InvSubBytes は逆の affine（式 5.16、d = {05}）と前の逆元の回路、InvShiftRows は slice の置換、InvMixColumns は circ(05,00,04,00) を掛けてから MixColumns（積が circ(0e,0b,0d,09) になる）。S-box の逆元の回路は aes_invert_slices に分けて前向きと共有 |
| `api-evp.c` | EVP_aes_256_cbc、CBC の EVP_CipherInit_ex・EVP_CipherUpdate（16 byte 未満の残りを持ち越す）、EVP_CipherFinal_ex（残りがあれば DATA_NOT_MULTIPLE_OF_BLOCK_LENGTH）、EVP_CIPHER_CTX_set_padding。padding on は UNSUPPORTED。EVP_Cipher は ChaCha20 だけ |
| `api-pkey.c` | EC の P-256 の鍵（CRYC_PKEY_EC_P256）: EVP_PKEY_Q_keygen("EC", 群の名前)、EVP_PKEY_CTX_new_from_name("EC")、EVP_PKEY_fromdata_init・EVP_PKEY_fromdata（group と pub だけ、65 byte の非圧縮の点、曲線の上）、EVP_PKEY_public_check、EVP_PKEY_get_octet_string_param（pub・encoded-pub-key）、EVP_PKEY_verify_init・EVP_PKEY_CTX_set_signature_md（SHA-256 だけ）・EVP_PKEY_verify（strict DER）、EVP_PKEY_derive の ECDH。EC の鍵の raw の形は断る |
| `api-kdf.c`（新） | HMAC（SHA-256 だけ、md NULL の static の出力は持たない）、EVP_KDF_fetch("HKDF")・EVP_KDF_CTX_new・_free・EVP_KDF_derive（digest・key・salt・info、info は連結して 32768 byte まで、他の名前は UNSUPPORTED） |
| `api-param.c`（新） | OSSL_PARAM_construct_utf8_string・_octet_string・_end、内部の名前の比較（cryc_param_name_equal・cryc_param_string_equal・cryc_param_p256_name） |
| `exports.map`・`sources.mk` | 20 個の公開の名前、api-kdf.c・api-param.c |
| `openssl-names/openssl/*.h`（新、10 個） | `<openssl/X.h>` を `<compat/crypto/X.h>` に導く転送の header（core_names・crypto・err・evp・hmac・kdf・params・rand・types・x509）。crypto-openssl.c を変えずに build するための include の道 |
| `platform/common/userland-dynamic.mk` | fidoctl・passkey-fido2 の規則だけ（下の hunk） |
| `userland/base/fidoctl/Makefile`・`passkey-fido2/Makefile` | 依存を security/openssl から base/libcrypto-compat に（Makefile の REQUIRE の解決が、選んだ config に libcrypto-compat を足す。config/ci は変えていない） |
| `plan/ws335/tests/unit/test-api-passkey.c`・`ct-api-passkey.c`（新）、`check-skeleton.sh` | 単体と定数時間の試験、名前の正規表現 |
| `plan/ws343/tests/run-passkey-compare.sh`・`passkey-compare.c`・`ct-matrix.sh`・`config-amd64-compat.mk`（新） | host の比較、cc/clang × -O2/-O3 の定数時間、OpenSSL 無しの build の config |

## header の差（WS335 の check-skeleton の BASE を移すため）

`BASE=a37f972c7a sh plan/ws335/tests/check-skeleton.sh` は 5 だけが次の 20 file の差で FAIL（BASE 無しは PASS）:

- 変えた: `include/libc/compat/crypto/evp.h`、`include/libc/compat/crypto/types.h`、`userland/base/libcrypto-compat/cryc-aes.h`、`cryc-api.h`、`cryc-err.h`（EVP_R の 138・150 を足しただけ。文の表 cryc-err.c は変えていない: test-err が 138 を文の無い reason として試すため）。
- 足した: `include/libc/compat/crypto/hmac.h`・`kdf.h`・`params.h`・`core_names.h`、`userland/base/libcrypto-compat/openssl-names/openssl/*.h`（10 個）。
- 試験の道具: `plan/ws335/tests/check-skeleton.sh`（crypto_names に `OSSL_PARAM_` と `HMAC$`）、足した `unit/test-api-passkey.c`・`unit/ct-api-passkey.c`。
- 試験の file（test-api-passkey.c・ct-api-passkey.c）は check-skeleton の 5 の対象に入っていない。
- 基点の候補: この Phase の最後の commit（Q1 が merge の後に決める）。

## userland-dynamic.mk の hunk（P6 の ws336-p021 の上、merge のため）

- `DYNAMIC_FIDOCTL_SSL := userland/base/libcrypto-compat/openssl-names`（前は staged の OpenSSL の `/usr`）。
- fidoctl の object と `DYNAMIC_PASSKEY_FIDO2_SSL_OBJS`（P6 の filter-out はそのまま）: `DYNAMIC_CPPFLAGS += -I$(DYNAMIC_FIDOCTL_SSL)`、`$(ZEDBSD_OPENSSL_STAGED)` の依存を外した。
- 2 つの program: 依存 `$(ZEDBSD_OPENSSL_LIBCRYPTO)` → `$(DYNAMIC_DIR)/libcrypto-compat.so`、`-L$(DYNAMIC_FIDOCTL_SSL)/lib` を外し、`-l:libcrypto.so` → `-l:libcrypto-compat.so`、check の `--needed libcrypto.so` → `--needed libcrypto-compat.so`。comment を 3 か所。

## 確かめ（2026-11-02、P2）

| command | 結果 |
| --- | --- |
| `WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors sh plan/ws335/tests/run-unit.sh api-passkey`（と `--win32`） | PASS 24754 checks（posix・Wine）。FIPS 197 C.1〜C.3 の逆、CAVP ECB の KAT（128・192・256 の復号と暗号）、CAVP CBC 256 の 4 file、SP 800-38A F.2.5・F.2.6 を 1〜64 byte の刻みで、RFC 4231 の 6 件、Wycheproof hmac_sha256 87、RFC 5869 A.1〜A.3、Wycheproof hkdf_sha256 86、CAVP PKV の P-256 12、Wycheproof ecdsa_secp256r1_sha256（valid 348・invalid 614 の判定）、Wycheproof ecdh_secp256r1_ecpoint（330 一致・25 拒否）、Q_keygen の 2 つの鍵の ECDH、誤りの経路 |
| `run-unit.sh aes gcm api-evp api-pkey err hash ecc`、`--win32 aes api-evp api-pkey` | 全部 PASS（前と同じ数） |
| `sh plan/ws335/tests/run-ct.sh api-passkey aes gcm`、`sh plan/ws343/tests/ct-matrix.sh` | PASS。ct-api-passkey（逆の暗号、CBC の両方向、HMAC、HKDF、ECDH の EVP の層）と ct-aes を cc・clang × -O2・-O3。ECDH の判定（scalar の範囲）は ct-api-pkey の X25519 と同じく公開にしてから derive（算術は ct-ecc） |
| `sh plan/ws335/tests/check-skeleton.sh`（BASE 無し） | PASS（header の単独の compile、zedBSD の clang、llvm-mingw の 2 arch、名前の範囲） |
| `sh plan/ws343/tests/run-passkey-compare.sh` | PASS。変えていない crypto-openssl.c を host の OpenSSL 3.5.6 と libcrypto-compat（ASan・UBSan）で build、1485 の pk_crypto_* の呼び出しの答えと出力が一致、compat の ECDH を既知の秘密鍵の相手で OpenSSL の導出が 50/50 確かめた、compat の build は host の OpenSSL の header を読まない |
| `make ZEDBSD_CONFIG=plan/ws343/tests/config-amd64-compat.mk BUILD=build/ws343-p002 build/ws343-p002/bin/fidoctl build/ws343-p002/bin/passkey-fido2` | rc 0、warning 0、OpenSSL は stage も build もされない、check-dynamic-elf PASS、NEEDED は 2 つとも libcrypto-compat.so と libc.so |
| 同じく aarch64（config-rpi4 から OpenSSL・curl・OpenSSH・Python を外した config、BUILD=build/ws343-p002a） | rc 0、warning 0 |
| `sh plan/ws335/tests/check-exports.sh build/ws343-p002/dynamic/libcrypto-compat.so` | PASS（public 78、cryc_ 190） |
| merge bf0fa13238 の後に `run-unit.sh all` と `run-unit.sh --win32 all` | 両方 PASS（posix 59 本、Wine 59 本、FAIL 0）。上の試験も merge の後に全部流し直して PASS |

## 未実施

- QEMU・実機での passkey-fido2・fidoctl の実行（割り当て無し。WS172 の loopback の security key の image の試験が候補）。
- riscv64・armv7 の build。

## 残り

- OpenSSL と違う所（header に書いた）: EVP_PKEY_verify は strict DER でない・r か s が 0・負・32 byte を越える署名を -1（OpenSSL は後の 3 つを 0）、HMAC の md NULL を断る、EVP_PKEY_fromdata は既にある *ppkey と知らない parameter を断る、AES-CBC は padding off だけ。crypto-openssl.c の答えはどれも同じ（host の比較）。
- crypto-openssl.c と crypto.h の comment は「OpenSSL の libcrypto」のまま（変えない指示）。Guardrail の例外（WS172）の扱いは Q1。
