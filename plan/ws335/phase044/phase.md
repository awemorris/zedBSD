<!-- awesome-plan project=zedbsd record=ws335-p044 -->

# ws335-p044: mailer の TLS を libssl-compat・libcrypto-compat を直に呼ぶ形に（dlopen・名前の表・ML_TLS_COMPAT を無くす）

Status: planned
Parent: [WS335](../ws.md)。設計: [p042](../phase042/phase.md)（§1.2・§1.3・§2・§4）。実装の担当: phase-runner-low（Sonnet 5.5 high）、1 回の起動。依存: p043 の merge（`plan/tools/tls/host-compat-libs.sh`）。

## 0. 何をするか（要約）

`userland/desktop/mailer/tls.c` から `dlopen`・`dlsym`・関数 pointer の表・名前の表・`ML_TLS_COMPAT`・OpenSSL 1.1 の名前への落ち（`SSL_get_peer_certificate`）を外し、`<compat/ssl/ssl.h>`・`<compat/crypto/evp.h>` の関数を直に呼ぶ。
`mail.h` の `ml_tls_*` の約束（errno・`ML_ERROR_UNTRUSTED`・自己署名の pin・指紋の形）は変えない。host の試験を compat を link する形に直す。

## 1. 読む物

- `AGENTS.md` の §4・§7・§9・§10。`plan/agents/protocol.md`。`plan/coding-style.md`（全文）。`plan/guardrail.md` の「出典」。
- 設計: `plan/ws335/phase042/phase.md` の §1.2・§1.3・§2（特に 4 の mailer）・§4。
- 仕様の役をする header（**変えない**）: `include/libc/compat/ssl/ssl.h`（`SSL_get1_peer_certificate`・`SSL_verify_cb`・`SSL_CTX_set_verify` ほか）、`include/libc/compat/crypto/x509.h`（`X509_digest`・`X509_free`・`X509_V_OK`）、`include/libc/compat/crypto/evp.h`（`EVP_sha256`）、`include/libc/compat/crypto/err.h`。
- 今の実装: `userland/desktop/mailer/tls.c`（804 行）、`mail.h:39-48・189-198`、`sync.c:990-1085`、`imap.c:155-165`、`smtp.c:80-90・125-130`。
- p043 の結果の tls.c（libbrowser）: 同じ置き換えの形の参考。
- 試験: `plan/ws335/tests/run-mailer-tls.sh`・`mailer-tls.c`、`plan/tools/mail/run-host-mail-backend.sh`・`host-mail-backend.c`・`host-mail-sync.c`（読むだけ）・`fake-mail-server.py`（読むだけ）、`plan/tools/tls/host-compat-libs.sh`（p043）。

## 2. 作る物・変える物

### 2.1 `userland/desktop/mailer/tls.c`（書き換え）

1. include: `<dlfcn.h>` を外し、`<compat/ssl/ssl.h>` と `<compat/crypto/evp.h>` を足す。
2. 消す: `TLS_LIBRARY_NAMES`、OpenSSL の定数の写し（`TLS_VERIFY_PEER` ほか、compat の名前を使う）、`struct tls_library`・`struct tls_symbol`・名前の表・`tls_symbols[]`・`tls_library`、`tls_open_library`・`tls_find_all`・`tls_find_peer_certificate` と prototype。`TLS_CA_FILES_MAX`・`TLS_DIGEST_BYTES` は残す。
3. 型: `tls_context` を `SSL_CTX *` に。`ml_tls_*` の公開の引数（`void *tls`）は mail.h の約束なので変えず、関数の中で `SSL *` の局所の変数に受ける（`void *` から object pointer への代入は暗黙で、cast は要らない）。`tls_set_host`・`tls_fail`・`tls_decide`・`tls_digest` の引数は `SSL *` に。
4. `tls_verify_later`: `static int tls_verify_later(int preverified, X509_STORE_CTX *store)` にし、`SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, tls_verify_later)` に直に渡す（関数 pointer の型の変換をしない）。中身は今のまま（1 を返す）。
5. `tls_load_locked`: 済み・不可の判定は今のまま。不可の文は `"cannot set up TLS"`（library を読み込む事はもう無い）。`tls_loaded = -1` の後に `tls_make_context()` だけを呼び、失敗は `EPROTONOSUPPORT`（理由は `tls_make_context` が `tls_reason` に書いた物）。
6. `tls_make_context`: `SSL_CTX_new(TLS_client_method())`、`SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION)`（1 でなければ失敗、文は今のまま）、`SSL_CTX_set_verify`（上の 4）、`(void)SSL_CTX_set_options(tls_context, SSL_OP_IGNORE_UNEXPECTED_EOF)`、`(void)SSL_CTX_set_default_verify_paths(tls_context)`（**失敗は黙る、今のまま**）、試験の CA の file（今のまま）。
7. `tls_set_host`: `SSL_set_tlsext_host_name(ssl, host)`（1 でなければ `EPROTO`）、`SSL_set1_host(ssl, host)`（同じ）。
8. `tls_digest`: `SSL_get1_peer_certificate(ssl)`（NULL なら `ENOENT`）、`X509_digest(certificate, EVP_sha256(), bytes, &length)`、`X509_free`、hex の書き方は今のまま。`length` の型は `unsigned int`（x509.h の signature）。
9. `ml_tls_open`・`ml_tls_read`・`ml_tls_write`・`ml_tls_pending`・`ml_tls_close`・`ml_tls_error`・`ml_tls_fingerprint`・`ml_tls_add_ca_file`・`tls_load`・`tls_fail`・`tls_decide`: 呼び先の名前だけを変え、流れ・誤りの文（`(SSL error %d)` を含む）・errno・`SSL_ERROR_SYSCALL` の時に `errno` を返す扱いは変えない（compat の `SSL_get_error` は SYSCALL の時 errno を戻す: ssl.h）。
10. file の先頭の comment を書き直す: 「Mail の TLS: libssl-compat と libcrypto-compat（zedBSD の TLS の library、OpenSSL 3.5 と同じ API）を直に呼ぶ。program は 2 つを link する。」に、今の 20-35 行の約束（TLS 1.2 以上、根と試験の CA、名前の照合、EPROTO と理由、handshake を最後まで進めて後で決める・指紋・pin、connection の thread と lock）を残す。`dlopen`・macro・OpenSSL・「as the browser does (libbrowser's net/tls.c)」の説明は消す。他の comment の「OpenSSL」（`/* One read (OpenSSL takes an int). */` など）は「the library」に。

### 2.2 他の mailer の file（文と comment）

- `mail.h:190`: 「/* The TLS of the OpenSSL package (tls.c). */」→「/* TLS (tls.c): libssl-compat and libcrypto-compat. */」の形に。
- `sync.c:993・1032・1042・1048`: 「OpenSSL's words」→「the TLS library's words」。
- `sync.c:1077`: `"TLS is not available (the OpenSSL package is not installed)"` → `"TLS cannot be set up"`（`EPROTONOSUPPORT` の意味は p042 §2 の 4）。この文は利用者に見える。他の言語の表（`userland/desktop/locale/` など）にこの文があれば、同じ commit で直す（無ければ「無い」と結果に書く: `grep -rn 'OpenSSL package is not installed' userland`）。
- `imap.c:159`・`smtp.c:84`: comment の「OpenSSL's words」→「The TLS library's words」。

### 2.3 試験の道具

1. `plan/ws335/tests/run-mailer-tls.sh`（書き直し）・`mailer-tls.c`（comment と `noload` の mode）:
   - build は 1 つ（`mailer-tls`）: 先に `sh plan/tools/tls/host-compat-libs.sh "$out/compat"`、compile 行に `-I"$out/compat/include"`、link に `"$out/compat/libssl-compat.a" "$out/compat/libcrypto-compat.a" -lpthread`、`-ldl` と `-DML_TLS_COMPAT` を外す。ASan・UBSan の flag は今のまま（compat の .a は sanitizer 無しでよい）。
   - case は今の 8 つ（自己署名 1.3 の pin 無し・その pin・他の pin、1.2 RSA の 3 つ、CA の file あり・無し）を 1 回ずつ。期待は今と同じ（指紋は `openssl x509 -fingerprint -sha256` と一致。host の `openssl` の command は server と証明書を作る道具として使ってよい）。
   - 「library 無しで EPROTONOSUPPORT」の case（6）と ld.so の log の照合（7）を外し、代わりに `nm "$out/mailer-tls" | grep -c ' T SSL_connect$'` が 1（compat が program の中にある）と、`readelf -d` の NEEDED に `libssl`・`libcrypto` が無い事を確かめる。`strings` に `libssl.so`・`libssl-compat.so` が無い事も。
   - `mailer-tls.c` の `noload` の mode と comment の「built once against the host's OpenSSL and once with ML_TLS_COMPAT」を外す。
   - 最後の行 `run-mailer-tls: PASS (8 cases)`。
2. `plan/tools/mail/run-host-mail-backend.sh`: 2 つの compile の前に `sh plan/tools/tls/host-compat-libs.sh "$base/compat"`（`$base` の下、run ごとの dir でなく共有で object を再利用）、各 compile に `-I"$base/compat/include"` と 2 つの .a・`-lpthread`、`-ldl` を外す。先頭の comment の「the host's OpenSSL loaded as Mail loads the package's」を「libssl-compat and libcrypto-compat built for the host and linked」に。

## 3. 確かめの項目

| # | 確かめ | 期待 |
| --- | --- | --- |
| 1 | `grep -n 'dlopen\|dlsym\|dlfcn\|ML_TLS_COMPAT\|tls_library\|tls_symbol\|libssl\.so\|libcrypto\|SSL_get_peer_certificate\|OpenSSL' userland/desktop/mailer/tls.c` | 「OpenSSL 3.5 と同じ API」の 1 か所の他に無い |
| 2 | `grep -rn 'OpenSSL' userland/desktop/mailer` | 1 の 1 か所の他に無い |
| 3 | `sh plan/ws335/tests/run-mailer-tls.sh` | `run-mailer-tls: PASS (8 cases)` |
| 4 | `sh plan/tools/mail/run-host-mail-backend.sh` | 今と同じ最後の行（流す前に今の main で 1 回流し、件数と最後の行を控えて比べる） |
| 5 | zedBSD の build（x86_64、§4） | exit 0、warning 0。`readelf -d build/x86_64/bin/mailer`（出力の場所は `build/x86_64/packages/mailer/` と package.mk の `STANDALONE_OUTPUT_mailer` で確かめる）の NEEDED に `libssl-compat.so`・`libcrypto-compat.so`、`.flags` に `ML_TLS_COMPAT` が無い |
| 6 | mailer の未定義の名前が compat で解ける | `llvm-nm -D --undefined-only` の `SSL_`・`X509_`・`ERR_`・`EVP_`・`TLS_` の名前が全部 2 つの .so の defined にある（件数を書く） |
| 7 | `git diff --check` | 出力無し |

## 4. 確かめのコマンド

1. `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/libcrypto-compat/.stamp build/x86_64/packages/libssl-compat/.stamp`
2. makefile.diff が main に当たっていれば: `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/mailer/.stamp`。
   当たっていなければ差分 B と同じ値を command line で: `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 'STANDALONE_SO_LIST_mailer=libvulkan.so libwayland-client.so libkeiland.so libtruetype.so libssl-compat.so libcrypto-compat.so' build/x86_64/packages/mailer/.stamp`。
   toolchain の用意は p041 の結果と `plan/tools.md` に従う（担当は toolchain を build・install しない）。
3. `git diff --check`。

各コマンドに時間の上限。make は `-j4` まで。大きな build の後は `sync`。

## 5. 注意

- 自己署名の pin が mailer の要。handshake は検証の失敗でも最後まで進み（`tls_verify_later` が 1）、`tls_decide` が `SSL_get_verify_result` で決める。compat の `SSL_get1_peer_certificate` は callback が通した自己署名の鎖でも leaf を返す（ssl.h、p041 で試験済み）。
- `X509_digest` は compat では SHA-256 だけを受ける（x509.h）。`EVP_sha256()` を渡す。
- default verify paths の失敗を黙る扱いは libbrowser（失敗は context の失敗）と違う。mailer は pin で自己署名の server に繋がるので、根の読み違いで全部を止めない今の形を残す（設計の判断、p042 §2 の 4）。
- `ml_tls_open` の `intptr_t socket` を `(int)` にして `SSL_set_fd` に渡す今の形を残す。
- 関数 pointer の型の変換をしない。source の comment に開発の管理情報を書かない。

## 6. 変えてはいけない物

全ての header（compat）、compat の .c、libbrowser（p043・p045）、`mail.h` の関数の宣言と定数（comment の 1 行だけは変える）、Makefile・`mk/`、HAL・toolchain・共有の `build/`、`plan/tools/tls/host-compat-libs.sh`（p043 の物。不具合は Q1 に返す）、§2.3 に名指していない他の WS の file。
header に不備を見つけたら直さずに止め、Q1 に返す。

## 7. 終わりの条件

- §3 の 1〜7 が全部期待どおり。`plan/coding-style.md` の checklist で自分の diff を見直した。
- わざと壊す確かめ（git の外で、戻った事を `git diff` で確かめる）: (a) `tls_decide` で pin を比べない → 「its pin」の case が FAIL、(b) `tls_digest` の hex の上位と下位を入れ替える → 指紋の case が FAIL、(c) `tls_verify_later` が 0 を返す → pin 無しの case の指紋が取れず FAIL。結果を結果の節に。
- commit: `git commit -m WIP -- userland/desktop/mailer/tls.c userland/desktop/mailer/mail.h userland/desktop/mailer/sync.c userland/desktop/mailer/imap.c userland/desktop/mailer/smtp.c plan/ws335/tests/run-mailer-tls.sh plan/ws335/tests/mailer-tls.c plan/tools/mail/run-host-mail-backend.sh plan/ws335/phase044/phase.md plan/ws335/ws.md`（locale の file を直したら足す）。push しない。
- 結果の節（日付は host の `date`）: コマンドと最後の行、件数、自分で決めた点、未実施、消してほしい path。ws.md の行は「実装済み・試験 PASS」。

## 結果

（実装の担当が書く）
