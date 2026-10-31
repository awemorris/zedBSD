<!-- awesome-plan project=zedbsd record=ws334-p030 -->

# ws334-p030 統合: OpenSSL の Windows の build と信頼する根（packages の ca-certificates ＋ Windows の store）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §6.6（S-A）、決定 16・18。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p005（`wb_os_trust_roots`）・p022（tls.c の library の読み込み）。
- 担当の想定: Opus（phase-runner）を 1 回。使う約束は `base/os-system.h` の `wb_os_trust_roots`。

## 中身

1. **OpenSSL の cross build**: `userland/packages/security/openssl/Makefile` の版と SHA-256（3.5.8）の tarball を同じ手順で取得・照合し（共有の `build/distfiles` に既にあれば読むだけ。無ければ自分の `build/` に取り、共有の場所への写しは Q1 に頼む）、`plan/ws334/tests/openssl-windows.sh`（置き場は design §6.6 の案のうち script の方に決めた: package の Makefile は zedBSD の package の規則に乗っているため、Windows の build は WS334 の script に置く）で llvm-mingw により `./Configure mingw64`（x86_64）と `mingw-arm64`（aarch64。OpenSSL 3.5 の Configurations の target の名前を `Configurations/10-main.conf` で確かめる。無ければ止めて Q1 に）、`--prefix=C:/Windows/System32/OmniGrammaton-no-prefix`（利用者が作れない path。OpenSSL が prefix から modules・engines の directory を決めるため、利用者が `C:\` の下に作れる folder を指さない）・`--openssldir=C:/Windows/System32/OmniGrammaton-no-config`（利用者が書けず存在しない path。design §6.6 の弱さの回避）、`no-tests no-docs`、共有の DLL。DLL の名前を確かめる（`libssl-3-x64.dll`・`libcrypto-3-x64.dll`、arm64 は `-arm64`）。出力は `$(BROWSER_WIN32_BUILD)/openssl/`。`apps/browser-win32/Makefile` に `openssl` の target（script を呼ぶ）。
2. **tls.c の根**（`net/tls.c`）:
   - 設定の自動の読みを切る: `OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL)` を library の読み込みの直後に（symbol の表に足す）。zedBSD でも同じ（zedBSD の package の openssl.cnf に頼る所が無いことを確かめる。あれば止めて Q1 に）。
   - context を作る時: `wb_os_trust_roots` を呼び、distrusted 0 の DER を `d2i_X509` → `X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), x)` → `X509_free`、distrusted 1 の物の SHA-256 の指紋（`X509_digest`、EVP_sha256）を一覧に持ち、検証の callback（`SSL_CTX_set_verify` の callback）で鎖の中の証明書がその一覧にあれば拒む。symbol の表に `SSL_CTX_get_cert_store`・`d2i_X509`・`X509_STORE_add_cert`・`X509_free`・`X509_digest`・`EVP_sha256`・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error` を足す。
   - packages の ca-certificates（決定 18）: 同梱の `share\certs\ca-certificates.pem`（`userland/packages/` の ca-certificates の package の Mozilla の束、MPL-2.0。p031 が同梱）を `SSL_CTX_load_verify_locations(ctx, path, NULL)` で読む。path は `wb_os_data_directory` + "/certs/ca-certificates.pem"。無ければ読まずに続ける。
   - `SSL_CTX_set_default_verify_paths` は posix では今のまま呼び、Windows では呼ばない: 「呼ぶか」を `wb_os_trust_roots` の結果で決める（POSIX は each を呼ばずに 0 を返すので「system の file に任せる」= 呼ぶ、Windows は 1 つ以上を渡す = 呼ばない）。`#ifdef` を書かない。
   - `browser_add_ca_file`（今の公開の関数）は今のまま足す。
3. 誤り: DLL が無い時は今の EPROTONOSUPPORT（design §3.7）。

## 試験（受け入れ）

- `openssl-windows.sh` が x86_64 と aarch64 の DLL を作り、`strings` に `--openssldir` の path が出て、`OPENSSL_CONF` を書ける場所に置いても読まない（Wine で `OPENSSL_CONF=<作った file>` を置き、file に不正な行を書いても TLS の接続が失敗しない）。
- Wine: p022 の fetch の program を https に: host の `openssl s_server`（試験の CA と server の証明書を `build/tmp/` に作る）へ、(a) 試験の CA を `browser_add_ca_file` で足すと接続でき、(b) 足さないと検証の失敗、(c) 試験の CA を Wine の ROOT の store に入れる（`wine certutil` が無ければ試験の program が `CertAddEncodedCertificateToStore` で current user の ROOT に足す）と足さずに接続でき、(d) 同じ CA を Disallowed に入れると拒む。外の network は使わない。
- zedBSD・Linux の host の TLS の試験（WS074 の https の試験、`plan/tools.md` で探す）の件数が同じ。`cross-build.sh` PASS。`git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。`userland/packages/security/openssl/` の zedBSD の規則（読むだけ）。HAL・toolchain・共有の build/（共有の distfiles は読むだけ）。rm をしない。push しない。
- commit、SHA と結果を Q1 に、結果の節と ws.md の行。
