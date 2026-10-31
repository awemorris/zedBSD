<!-- awesome-plan project=zedbsd record=ws335-p036 -->

# ws335-p036（提案）: libbrowser の net/tls.c の libssl-compat への切り替え

Status: planning（2026-11-01 p001 が提案。U2 は同日 A で答えを受けた。Windows は DLL を作らず、1 つの .exe への static link（[ws.md](../ws.md) のユーザーの決定）。起動は Q1 が WS074・WS334 と調整してから。依存: p033、WS334 の p022（net/tls.c の library の読み込みを `wb_os_library_open` に）・p030（OpenSSL の Windows の根の扱い））
Parent: [WS335](../ws.md)。担当: 未定（net/tls.c は WS074 の所有。Windows の build は WS334（P7）の側が直す）。

## 1. 形（[design.md](../design.md) §4.2・§4.4）

- **zedBSD（U2 の A）**: libbrowser の Makefile が zedBSD の build で `-DWB_TLS_COMPAT` を付け、net/tls.c が開く名前を `libssl-compat.so`・`libcrypto-compat.so` にする（名前の `#ifdef` だけ。読み込みは WS334 p022 の後の `wb_os_library_open`）。Linux の host の試験は既定で本家、`-DWB_TLS_COMPAT` で compat も。libbrowser の package の依存に `base/libssl-compat` を足す。
- **Windows（static link）**: 読み込みを行わない。net/tls.c の関数の表（`struct tls_library`）を、static link した関数で満たす小さな file（例 `net/tls-static.c`、Windows の build だけが compile する。`<compat/ssl/ssl.h>` を include し、表の member と同じ型の 1 行の wrapper（`static void *tls_static_ctx_new(const void *method) { return SSL_CTX_new(method); }` の類）を置いて表に入れる。関数 pointer の型を変換しない）。net/tls.c の他の code は変えない。library が無い時の EPROTONOSUPPORT の道は Windows では通らない。
- **compat で使う関数**は design.md §5 の部分集合だけ。WS334 p030 が OpenSSL のために足す物（`OPENSSL_init_ssl`、`X509_digest`、`EVP_sha256` での指紋、`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error`、検証の callback での Disallowed の照合、`wb_os_trust_roots` の DER を `X509_STORE_add_cert` で入れる loop）は compat では使わない: compat の `SSL_CTX_set_default_verify_paths` が Windows でも ROOT（Disallowed を除く）と .exe の隣の `cert.pem` を読み、Disallowed の証明書を全部の鎖で拒む（D14・D24）。compat では net/tls.c は Windows でも `SSL_CTX_set_default_verify_paths` を呼ぶ。
- **根の file**: packages の ca-certificates の束（WS334 の決定 18）を `cert.pem` の名前で `OmniGrammaton.exe` の隣に置く（U5 の A）。`share/certs/ca-certificates.pem` を `SSL_CTX_load_verify_locations` で読む今の WS334 の形も compat で動く（どちらにするかは WS334 の担当が決める。両方なら同じ証明書は store で重ならない）。

## 2. 受け入れ

- zedBSD: WS074 の host の試験（本家と compat の両方）、QEMU の zedBSD でブラウザが https の頁を開く（Q1 の割当、画面の PNG）。compat の表の名前が全部 libssl-compat.so・libcrypto-compat.so の export にある事（`check-exports.sh` の一覧と照合）。
- Windows: WS334 の Wine の https の試験（WS334 p030 の (a)〜(d) と同じ組: 試験の CA を足す・足さない・ROOT に入れる・Disallowed に入れる）が compat の static link で PASS。`OmniGrammaton.exe` の import に OpenSSL の DLL が無い事（`llvm-objdump -p`）。

## 3. 調整

- net/tls.c は WS074 の所有。Windows の build（`apps/browser-win32/Makefile` の 2 つの `sources.mk` の include、static library の追加、.exe の link の `-lbcrypt -lcrypt32 -lws2_32`、OpenSSL の DLL と `openssl` の target と許諾の文の同梱を外す事）は WS334 の担当（P7）が WS334 の Phase で直す。この Phase は WS334 の file を変えない。
- WS335 が渡す物は [design.md](../design.md) §4.4 の一覧。

## 結果

（未着手）
