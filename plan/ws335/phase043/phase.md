<!-- awesome-plan project=zedbsd record=ws335-p043 -->

# ws335-p043: libbrowser の net/tls.c を libssl-compat・libcrypto-compat を直に呼ぶ形に（名前の表・dlopen・WB_TLS_COMPAT・WB_TLS_STATIC を無くす）

Status: in-progress（2026-11-13 実装済み。確かめ 1〜6・9・10 PASS、7・8 は zedBSD の toolchain が worktree に無く未実施。cleared は Q1）
Parent: [WS335](../ws.md)。設計: [p042](../phase042/phase.md)（§1.1・§1.3・§2・§4）。実装の担当: phase-runner-low（Sonnet 5.5 high）、1 回の起動。依存: p042 の Q1 の判定（Q4〜Q7 の答え）と makefile.diff のユーザーの承認。

## 0. 何をするか（要約）

`userland/desktop/libbrowser/net/tls.c` から、関数 pointer の表（`struct tls_library`）・名前の表・`wb_os_library_open` での読み込み・`WB_TLS_COMPAT`・`WB_TLS_STATIC`・OpenSSL だけの道（`OPENSSL_init_ssl`、OS の根の DER の loop、Disallowed の指紋と検証の callback）を外し、`<compat/ssl/ssl.h>` の関数を直に呼ぶ。
外から見える振る舞い（`net/net.h` の `net_tls_*` の約束、誤りの文、errno）は変えない。host の試験の道具を compat を link する形に直す。

## 1. 読む物

- `AGENTS.md` の §4（担当の規則）・§7（禁止）・§9（検証）・§10（コード）。`plan/agents/protocol.md`。
- `plan/coding-style.md`（全文。C を書く前に）。`plan/guardrail.md` の「出典」の節。
- 設計: `plan/ws335/phase042/phase.md` の §1.1・§1.3・§2・§4（この phase.md と食い違えば phase042 を正とし、食い違いを報告する）。
- 仕様の役をする header（**変えない**）: `include/libc/compat/ssl/ssl.h`（各関数の comment が約束）、`include/libc/compat/crypto/x509.h`（`X509_V_OK`・`X509_VERIFY_PARAM_set1_ip_asc`・`X509_verify_cert_error_string`）、`include/libc/compat/crypto/err.h`。
- 今の実装: `userland/desktop/libbrowser/net/tls.c`（1296 行）、`net/tls-library.h`、`net/tls-static-compat.c`（compat の関数と表の member の対応の参考）、`net/net.h:188-200`、`net/http.c:135-145`、`base/os-net.h:165-185`。
- 試験の道具: `plan/ws074/tests/host-build.sh`、`plan/ws334/tests/net-ring-test.sh`、`plan/ws335/tests/run-nettls.sh`、`plan/ws074/tests/run-http-tests.py`・`run-loader-tests.py`（読むだけ）。
- Makefile の案: `plan/ws335/phase042/makefile.diff`（A と C。当てるのは Q1。この Phase では command line の上書きと写しで確かめる）。

## 2. 作る物・変える物

### 2.1 `userland/desktop/libbrowser/net/tls.c`（書き換え）

1. include: `"net/tls-library.h"` を外し、`<compat/ssl/ssl.h>` を足す（`err.h`・`x509.h` は ssl.h が include する）。`base/os-system.h` は `wb_os_data_directory` のため残す。
2. 消す: `TLS_*` の OpenSSL の定数の写し（62-76 行）、`TLS_FINGERPRINT_SIZE`・`TLS_DISTRUSTED_MAX`、`TLS_LIBRARY_NAMES`、`#ifndef WB_TLS_STATIC` の block 全部（`struct tls_symbol`・`tls_symbols[]`・名前の表）、`struct tls_roots`、`tls_library`、`tls_distrusted*` の 3 つ、`tls_bind_library`（2 つとも）・`tls_find`・`tls_find_all`・`tls_refuses_distrusted`・`tls_add_roots_each`・`tls_remember_distrusted`・`tls_verify` と、その prototype。
3. 型: `struct net_tls` の `ssl` を `SSL *`、`tls_context` を `SSL_CTX *` に。`tls_attach`・`tls_set_host`・`tls_fail` の `void *ssl` を `SSL *` に。
4. 呼び出し: `tls_library.xxx(...)` を compat の関数に置き換える。対応は `tls-static-compat.c` の wrapper の中身と同じ（例: `ctx_ctrl(ctx, TLS_CTRL_SET_MIN_PROTO_VERSION, TLS_VERSION_1_2, NULL)` → `SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION)`、`ssl_ctrl(ssl, TLS_CTRL_SET_TLSEXT_HOSTNAME, ...)` → `SSL_set_tlsext_host_name(ssl, host)`、`param_set1_ip_asc(get0_param(ssl), address)` → `X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), address)`）。定数は ssl.h・x509.h の名前（`SSL_VERIFY_PEER`・`SSL_OP_IGNORE_UNEXPECTED_EOF`・`SSL_ERROR_SSL`・`SSL_ERROR_WANT_READ`・`SSL_ERROR_WANT_WRITE`・`SSL_ERROR_SYSCALL`・`SSL_ERROR_ZERO_RETURN`・`X509_V_OK`）。
   `SSL_set_tlsext_host_name` は `(void *)(name)` の cast を macro の中でする（ssl.h）。`const char *host` をそのまま渡してよい。
5. `tls_load_once`: `tls_bind_library` を呼ばず、`tls_make_context` だけ。失敗は今までどおり `tls_loaded = -1` で覚え、`EPROTONOSUPPORT` を返す（理由は `tls_load_reason`）。2 回目以降の失敗の文「cannot load the TLS libraries」は「cannot set up TLS」に（library を読み込む事はもう無い）。
6. `tls_make_context`: `init_ssl` の block を消す。`SSL_CTX_new(TLS_client_method())`、`SSL_CTX_set_min_proto_version`（1 でなければ失敗、文は今のまま）、`tls_add_roots`、`SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, NULL)`、`SSL_CTX_set_options(tls_context, SSL_OP_IGNORE_UNEXPECTED_EOF)`、`tls_add_bundle`、試験の CA の file（今のまま）。
7. `tls_add_roots`: `SSL_CTX_set_default_verify_paths` を呼び、1 でなければ `ERR_clear_error()`・理由「cannot read the system's trusted certificates」・`EPROTO`（今の compat の枝と同じ）。`wb_os_trust_roots` は呼ばない。
8. `tls_add_bundle`・`net_tls_add_ca_file`・`net_tls_open`・`net_tls_start`・`net_tls_handshake`・`net_tls_read`・`net_tls_read_some`・`net_tls_write`・`net_tls_write_some`・`net_tls_pending`・`net_tls_close`・`net_tls_reason`・`tls_attach`・`tls_set_host`・`tls_is_address`・`tls_fail`・`tls_set_reason`: 呼び先の名前と型だけを変え、流れ・誤りの文・errno は変えない。
9. file の先頭の comment を書き直す: 「libssl-compat と libcrypto-compat（zedBSD の TLS の library、OpenSSL 3.5 と同じ API）を直に呼ぶ。zedBSD では libbrowser.so が 2 つの .so を link し、Windows では program に static link する。根は library の default paths（zedBSD は ca-certificates の `/etc/ssl/cert.pem`、Windows は system の ROOT と .exe の隣の cert.pem。system が信用しない証明書は library が鎖のどこでも拒む）、program の束、試験の CA の file。」に、今の 29-44 行の他の約束（TLS 1.2 以上、名前・address の照合、誤りは EPROTO と理由、2 つの thread と lock）を残す。OpenSSL・`dlopen`・macro の説明は消す。開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
10. 「OpenSSL」の語が残る comment（例 `/* One read (OpenSSL takes an int). */`、`/* The bytes OpenSSL holds. */`、`tls_fail` の comment）は「the library」に直す。`tls_fail` の文の「TLS error %d」は今のまま。

### 2.2 他の libbrowser の file（comment だけ）

- `net/net.h:188`: 「TLS for https (tls.c): libssl-compat and libcrypto-compat, linked.」の形に。
- `net/http.c:139-141`: 「EPROTONOSUPPORT … without the OpenSSL package」を「EPROTONOSUPPORT … when TLS cannot be set up」に（意味は §2 の 4: context が作れない）。
- `base/os-net.h:169-180`: 「OpenSSL's SSL_SET_fd」「OpenSSL on Windows」「a library (OpenSSL's SSL_ERROR_SYSCALL)」を「the TLS library's SSL_set_fd」「SSL_ERROR_SYSCALL」に。関数の約束は変えない。
- `base/os-system.h:165` の `wb_os_trust_roots` の comment は p045 の範囲（Q4）。この Phase では触らない。
- `net/tls-library.h`・`net/tls-static.c`・`net/tls-static-compat.c` は**変えず消さない**（削除は差分 C の後に Q1）。書き換えの後の tls.c はどれも include しない。

### 2.3 試験の道具

1. **新しい** `plan/tools/tls/host-compat-libs.sh`（POSIX sh、`set -eu`、先頭の comment に使い方と出力、`Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib`）:
   - 使い方: `sh plan/tools/tls/host-compat-libs.sh <outdir>`。環境の `HOST_COMPAT_CFLAGS` を compile の flag に足す（既定は空。sanitizer を渡す口）。
   - 作る物: `<outdir>/include/compat`（`$(pwd)/include/libc/compat` への symlink、`ln -sfn`）、`<outdir>/obj/<library>-<name>.o`、`<outdir>/libcrypto-compat.a`・`<outdir>/libssl-compat.a`。
   - source の一覧: `userland/base/libcrypto-compat/sources.mk` の common と POSIX（`grep -v '^#' … | grep -o 'userland/base/libcrypto-compat/[a-z0-9-]*\.c' | grep -v -- '-win32\.c'`）、`userland/base/libssl-compat/sources.mk`（run-nettls.sh の今の 2 行と同じ）。
   - compile: `cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fPIC -I<outdir>/include -I. $HOST_COMPAT_CFLAGS -c`（run-nettls.sh の今の flag と同じ。`-fPIC` は libbrowser.so 等の shared object に入れられるように）。object が source・`<outdir>/flags`（flag の文字列、変わった時だけ書き直す）より新しく、`find userland/base/libcrypto-compat userland/base/libssl-compat include/libc/compat -name '*.h' -newer <object>` が空なら compile しない（host-build.sh の engine の loop と同じ考え）。各 compile に `timeout 300`。
   - archive: 決まった名前 `<outdir>/libX.a.new` に `printf '!<arch>\n' > <outdir>/libX.a.new` で空の archive の見出しを書き（中身の無い file に GNU ar は追記できない: `file format not recognized`）、`ar rcs <outdir>/libX.a.new <objects>`、`mv -f <outdir>/libX.a.new <outdir>/libX.a`。**rm を使わない**。mktemp は使わない（失敗の時に 0600 の file が残る）。
   - 出力の最後の行: `host-compat-libs: <outdir> (<N> crypto, <M> ssl sources)`。失敗は stderr に理由、exit 1。
   - 使う側の link: `-I<outdir>/include` と `<outdir>/libssl-compat.a <outdir>/libcrypto-compat.a -lpthread`（この順）。`-Iinclude/libc` は使わない（host の libc の header を隠す）。
2. `plan/ws074/tests/host-build.sh`: 画像の compat の library の loop（`userland/desktop/libjpeg-compat/*.c …`）の後に、compat の TLS の loop を足す: 同じ source の一覧（上の 2 つの sources.mk から）を `$out/obj/<library>-<name>.o`（`library` は `libcrypto-compat`・`libssl-compat`）に `"$cc" $flags -c` で compile し、`engine` と `objects` に足す（画像の library と同じ形。`$base/include/compat` は既にある）。**`-Wno-error` は付けない**。warning が出たら止めて、file と文を Q1 に返す（compat の source はこの Phase の範囲の外）。
   libbrowser.so の link 行の `-ldl` は残す（`paint/vulkan.c` が使う）。header の comment に「the TLS library (libcrypto-compat, libssl-compat)」を 1 行足す。
3. `plan/ws334/tests/net-ring-test.sh`: `sh plan/tools/tls/host-compat-libs.sh build/tmp/ws334-net-ring/compat` を先に流し（失敗は FAIL）、compile 行に `-Ibuild/tmp/ws334-net-ring/compat/include`、link に 2 つの .a と `-lpthread`。tls-static の 2 つを除く `grep -v` は、file が消えるまで残す（comment を「the Windows build's old bindings, until they are removed」に）。
4. `plan/ws335/tests/run-nettls.sh`（書き直し）: host-build.sh を 1 回（`BROWSER_HOST_BUILD=build/ws335-nettls-host`、CC の wrapper 無し）。確かめ:
   - `readelf -d build/ws335-nettls-host/plain/libbrowser.so` の NEEDED に `libssl`・`libcrypto` で始まる物が無い（compat は中に入っている）。
   - `nm -D --undefined-only` の出力に `SSL_`・`X509_`・`ERR_`・`TLS_client_method` が無い。`nm -D --defined-only` にも無い（exports.map が隠す）。
   - `strings` に `libssl.so`・`libcrypto.so`・`libssl-compat.so` が無い（名前で開く物が無い）。
   - run-http-tests.py・run-loader-tests.py を今の引数で 1 回ずつ、`LD_DEBUG=libs` の log で `libssl`・`libcrypto` の init が 0 回。
   - 件数: http 15、loader 11（p036 の基準）。最後の行 `run-nettls: PASS (http 15 cases, loader 11 cases, libssl-compat and libcrypto-compat linked)`。
   OpenSSL の run と compat の .so の build と `LD_LIBRARY_PATH` の wrapper は外す。出力は今と同じ `build/tmp/ws335-nettls`（`fresh_out`）。

## 3. 確かめの項目

| # | 確かめ | 期待 |
| --- | --- | --- |
| 1 | `grep -n 'WB_TLS\|tls_library\|tls-library\|wb_os_library\|wb_os_trust_roots\|OPENSSL\|"libssl\|"libcrypto\|libssl\.so\|libcrypto\.so' userland/desktop/libbrowser/net/tls.c`（名前で開く物と表。comment の「libssl-compat and libcrypto-compat」は当たらない） | 何も出ない |
| 2 | `grep -rn 'OpenSSL' userland/desktop/libbrowser/net/tls.c userland/desktop/libbrowser/net/net.h userland/desktop/libbrowser/net/http.c userland/desktop/libbrowser/base/os-net.h` | 「OpenSSL 3.5 と同じ API」の 1 か所の他に無い |
| 3 | `sh plan/tools/tls/host-compat-libs.sh build/tmp/p043/compat` を 2 回 | 2 回目は compile 0（object を作り直さない）、最後の行が `host-compat-libs: …`、warning 0 |
| 4 | `sh plan/ws335/tests/run-nettls.sh` | `run-nettls: PASS (http 15 cases, loader 11 cases, …)` |
| 5 | `sh plan/ws334/tests/net-ring-test.sh plain` | `net-ring-test: PASS`（前提: host-build.sh plain が `build/ws074-host/include` を作っている事。無ければ `sh plan/ws074/tests/host-build.sh plain` を先に） |
| 6 | `BROWSER_HOST_BUILD=build/tmp/p043/asan-host sh plan/ws074/tests/host-build.sh asan`、続けてその browser で `run-http-tests.py` を 1 回（引数は run-nettls.sh と同じ、`ASAN_OPTIONS=detect_stack_use_after_return=0`） | build が通る（warning 0）、http 15 件が PASS、ASan・UBSan の報告 0（compat の object も asan の flag で compile される） |
| 7 | zedBSD の build（x86_64、§4 の 1・2） | exit 0、warning 0、`readelf -d build/x86_64/dynamic/libbrowser.so` の NEEDED に `libssl-compat.so`・`libcrypto-compat.so`、`.flags` に `WB_TLS_COMPAT` が無い |
| 8 | zedBSD の libbrowser.so の未定義の名前が compat で解ける | `llvm-nm -D --undefined-only build/x86_64/dynamic/libbrowser.so` の `SSL_`・`X509_`・`ERR_`・`TLS_` の名前が全部 `llvm-nm -D --defined-only` の libssl-compat.so か libcrypto-compat.so にある（script で突き合わせ、件数を書く） |
| 9 | Windows の core の compile（§4 の 3） | exit 0、warning 0。`net/tls.o` の `llvm-nm` に `U SSL_connect`、`tls_static_library` の名前が無い |
| 10 | `git diff --check` | 出力無し |

## 4. 確かめのコマンド

1. compat を先に: `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/libcrypto-compat/.stamp build/x86_64/packages/libssl-compat/.stamp`
2. libbrowser（差分 A と同じ値を command line で。makefile.diff が main に当たった後なら上書き無し）:
   `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 'STANDALONE_CFLAGS_libbrowser=$(STANDALONE_SHLIB_CFLAGS)' 'STANDALONE_SO_LIST_libbrowser=libtruetype.so libvulkan.so libmedia.so libjpeg-compat.so libpng-compat.so libz-compat.so libgif-compat.so libwebp-compat.so libssl-compat.so libcrypto-compat.so' build/x86_64/packages/libbrowser/.stamp`
   toolchain の用意は p041 の結果と `plan/tools.md` に従う（担当は toolchain を build・install しない。無ければ Q1 に返す）。
3. Windows: `mkdir -p build/tmp/p043/tree/apps/browser-win32 && cp apps/browser-win32/Makefile build/tmp/p043/tree/apps/browser-win32/Makefile && git apply -v --include=build/tmp/p043/tree/apps/browser-win32/Makefile --directory=build/tmp/p043/tree plan/ws335/phase042/makefile.diff`（`--include` は `--directory` を付けた後の path と照合される。`apps/...` と書くと全部 Skipped のまま exit 0 になる）、`grep -c 'tls-static\|WB_TLS_STATIC' build/tmp/p043/tree/apps/browser-win32/Makefile` が 0 を確かめ（0 でなければ当たっていない。`-v` の出力を見て直す）、
   `make -f build/tmp/p043/tree/apps/browser-win32/Makefile WIN32_ARCH=x86_64 BROWSER_WIN32_BUILD=build/tmp/p043/win64 -j4 libbrowser-core`（llvm-mingw の場所は `plan/ws334/tests/cross-build.sh` が渡す `LLVM_MINGW` と同じ）。差分 C は p045 の merge で当たるので、この Phase では写しで確かめる。
4. `git diff --check`。

各コマンドに時間の上限（make は `timeout 3600`、試験は script の上限）。make は `-j4` まで。大きな build の後は `sync`。

## 5. 注意

- **振る舞いを変えない**: 誤りの文・errno・thread の扱い・`INT_MAX` で切る長さは今と同じ。WS074 の http・loader の試験の件数が p036 と同じ（15・11）である事が回帰の基準。
- `SSL_CTX_set_default_verify_paths` の失敗で context を失敗させるのは p036 の決定（system の根の読み違いを黙って通さない）。zedBSD の compat は file が無い時 1 を返す（ssl.h の comment）。
- `SSL_get_error` の SYSCALL の時、compat は errno を戻す（ssl.h）。今の code は `wb_os_socket_last_error()` を読むので、それを残す（Windows の socket の誤りは errno ではない）。
- compat の `SSL_set1_host` は IP の文字列も受けるが、今の code は IP の時 `X509_VERIFY_PARAM_set1_ip_asc` を使い SNI を送らない。この形を残す（SNI に IP を送らない、RFC 6066 3）。
- 関数 pointer の型の変換をしない。`(void *)` への cast で警告を消さない。
- host（Linux）の compat の default paths は `/etc/ssl/cert.pem` だけで、Debian の host の根を読まない（p042 の問い Q9）。試験は local の CA だけを使うので影響しない。Q9 が A なら host-build.sh の comment に「公開の site には `--ca-file /etc/ssl/certs/ca-certificates.crt`」を 1 行書く。
- zedBSD の package の compile の flag には `-Wall` が無いので、型の誤りは host の build（`-Werror`）で捕まえる。
- host-build.sh は多くの WS の試験が使う。flag・出力の置き場所・`obj/` の名前の形を変えない（compat の object を足すだけ）。
- source の comment に開発の管理情報を書かない（AGENTS.md §10）。

## 6. 変えてはいけない物

全ての header（`include/libc/compat/**`、`userland/base/lib*-compat/*.h`）、compat の .c、`net/tls-library.h`・`tls-static.c`・`tls-static-compat.c`（消すのは Q1）、`base/os-system.h`・`os-system-*.c`（p045）、mailer（p044）、Makefile・`mk/`（案は makefile.diff、当てるのは Q1）、HAL・toolchain・共有の `build/`、他の WS の file のうち §2.3 に名指していない物。
header に不備を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。phase.md と header・設計が食い違えば header と設計を正とし、報告する。

## 7. 終わりの条件

- §3 の 1〜10 が全部期待どおり（出力を自分で読む）。`plan/coding-style.md` の checklist で自分の diff を見直した。
- わざと壊す確かめ（git の外で、終わったら `git diff` で戻った事を確かめる）: (a) `SSL_CTX_set_verify` の mode を `SSL_VERIFY_NONE` に → run-http-tests の誤った証明書の case が FAIL、(b) `tls_set_host` で `SSL_set1_host` を呼ばない → 名前の違う証明書の case が FAIL（試験の server は SNI を見ないので、SNI を外す壊しは観測できない）、(c) `tls_add_roots` の失敗を無視 → 変化が無い事を記録（default paths が成功する host では観測できない）。結果を結果の節に。
- commit: `git add plan/tools/tls/host-compat-libs.sh` の後、`git commit -m WIP -- userland/desktop/libbrowser/net/tls.c userland/desktop/libbrowser/net/net.h userland/desktop/libbrowser/net/http.c userland/desktop/libbrowser/base/os-net.h plan/tools/tls/host-compat-libs.sh plan/ws074/tests/host-build.sh plan/ws334/tests/net-ring-test.sh plan/ws335/tests/run-nettls.sh plan/ws335/phase043/phase.md plan/ws335/ws.md`。push しない。
- 結果の節（日付は host の `date`）: 実行したコマンドと最後の行、件数、自分で決めた点、未実施、消してほしい path（`build/tmp/p043`、使われなくなった `build/ws335-nettls-host-compat`（compat の wrapper の build）など、理由付き）。ws.md の行は「実装済み・試験 PASS」（cleared は Q1）。
- Q1 への報告: commit の SHA、上の要約。

## 結果（2026-11-13 Sonnet q2053-i01、branch `agent/s-ws335-p043`、基点 main 13e62a764b）

### 変えた物

- `userland/desktop/libbrowser/net/tls.c`: 1296 行 → 818 行。表・名前の表・`wb_os_library_open`・`WB_TLS_COMPAT`・`WB_TLS_STATIC`・OpenSSL の定数の写し・`init_ssl`・OS の根の loop・Disallowed の指紋と検証の callback を外し、`<compat/ssl/ssl.h>` の関数を直に呼ぶ。`struct net_tls` の `ssl` は `SSL *`、context は `SSL_CTX *`。`tls_add_roots` は `SSL_CTX_set_default_verify_paths` だけ（1 でなければ `ERR_clear_error`・「cannot read the system's trusted certificates」・EPROTO）。誤りの文・errno・`INT_MAX` で切る長さ・`wb_os_socket_last_error()`・IP の時 SNI を送らない形は今のまま。
- comment だけ: `net/net.h`・`net/http.c`・`base/os-net.h`（OpenSSL の語を「the TLS library」等に）。
- 新: `plan/tools/tls/host-compat-libs.sh`。変更: `plan/ws074/tests/host-build.sh`（compat の TLS の loop、`-Wno-error` 無し、host の根の comment）、`plan/ws334/tests/net-ring-test.sh`（compat を先に build して link、tsan の時は HOST_COMPAT_CFLAGS に sanitizer）、`plan/ws335/tests/run-nettls.sh`（書き直し）。
- 変えていない: header、`net/tls-library.h`・`tls-static.c`・`tls-static-compat.c`、Makefile・`mk/`。

### 確かめ

| # | コマンド | 結果 |
| --- | --- | --- |
| 1 | 確かめ 1 の grep | 何も出ない |
| 2 | 確かめ 2 の grep | `tls.c:10`（「OpenSSL 3.5's」）の 1 か所だけ |
| 3 | `sh plan/tools/tls/host-compat-libs.sh build/tmp/p043/compat` を 2 回 | 1 回目 4.9 s で compile（`-Werror`、warning 0）、2 回目 0.4 s で object の mtime が変わらず compile 0。最後の行 `host-compat-libs: build/tmp/p043/compat (30 crypto, 10 ssl sources)`。archive は 30・10 個 |
| 4 | `sh plan/ws335/tests/run-nettls.sh`（最終の tree で 1 回、その前に 1 回） | `libbrowser.so: 0 libssl/libcrypto NEEDED, 0 imported and 0 exported TLS names, 0 library names in strings`、`http-tests 15/15`、`loader-tests 11/11`、`119 libraries started, 0 of them libssl or libcrypto`、`run-nettls: PASS (http 15 cases, loader 11 cases, libssl-compat and libcrypto-compat linked)` |
| 5 | `sh plan/ws334/tests/net-ring-test.sh plain` | `net-ring-test: PASS`（11 s。`build/ws074-host` は無いまま、zlib の header は libz-compat の source の-I で足りた）。tsan は未実施 |
| 6 | `BROWSER_HOST_BUILD=build/tmp/p043/asan-host sh plan/ws074/tests/host-build.sh asan`、続けて `ASAN_OPTIONS=detect_stack_use_after_return=0` で run-http-tests.py | build 通る（`-Werror`）、`http-tests 15/15`、ASan・UBSan の報告 0（grep で数えた） |
| 7 | zedBSD の build（x86_64） | **未実施**: worktree に `build/llvm`・sysroot が無い（`build/x86_64` も無い）。toolchain の build・install は担当がしない規則なので Q1 に返す |
| 8 | libbrowser.so の未定義の名前が compat で解ける | **未実施**（7 に依る）。代わりの確かめ（zedBSD の確かめではない）: host の `libbrowser_net_tls.o` が参照する `SSL_`・`X509_`・`ERR_`・`TLS_client_method` の 25 名の全部が `libssl-compat`・`libcrypto-compat` の `exports.map` の global にある（`grep -w`、欠け 0） |
| 9 | Windows の core（確かめ 4 の 3 の手順、`LLVM_MINGW=$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64`、`-j3`） | 差分 A・C を写しに当てて `git apply -v` が `Applied patch ... cleanly`、写しの `grep -c 'tls-static\|WB_TLS_STATIC'` は 0。`libbrowser-core` は exit 0（2 回目の実行も exit 0）、`warning:`・`error:` 0 行。`llvm-nm net/tls.o` に `U SSL_connect`・`U SSL_CTX_new`・`U X509_VERIFY_PARAM_set1_ip_asc`・`U ERR_*`、`tls_static_library`・`tls_library` は無い |
| 10 | `git diff --check` | 出力なし |

### わざと壊す確かめ（tls.c を壊して host build し run-http-tests。終わりに戻して `git diff` が空である事を確かめた）

| 壊し方 | 結果 |
| --- | --- |
| (a) `SSL_CTX_set_verify` を `SSL_VERIFY_NONE` に | `FAIL untrusted`・`FAIL wrong-name`、13/15 |
| (b) `tls_set_host` の `SSL_set1_host` の呼びと検査を消す | **15/15（観測できない）**。理由: http の試験は全て `127.0.0.1`（IP）に繋ぎ、`tls_set_host` の IP の枝（`X509_VERIFY_PARAM_set1_ip_asc`）を通り、名前の枝（SNI と `SSL_set1_host`）を通らない。phase.md の「(b) で名前の違う証明書の case が FAIL」は成り立たない |
| (b') 代わりに IP の枝の `X509_VERIFY_PARAM_set1_ip_asc` を消す | `FAIL wrong-name`、14/15 |
| (c) `tls_add_roots` の失敗を無視 | 15/15（変化なし。host で default paths が成功するため観測できない） |

名前の枝（DNS 名の host、SNI と `SSL_set1_host`）は host の試験で通らない。host の試験に名前で繋ぐ case（server が 127.0.0.1 のまま、`/etc/hosts` に頼らず `--resolve` 相当）が無いため。p046 の guest か別の試験で確かめる事を勧める。

### 自分で決めた点

- `tls_add_roots` を関数のまま残した（default paths の失敗を context の失敗にする理由の comment を持たせる）。
- 2 回目以降の失敗の文は「cannot set up TLS」、lock の失敗は「cannot lock the setup of TLS (error %d)」。
- `net-ring-test.sh` の tsan は compat の object も `-fsanitize=thread` で作る（HOST_COMPAT_CFLAGS に渡す。out の directory が variant ごとなので混ざらない）。
- Q9（Q1 の判断: A）の 1 行を host-build.sh の先頭の comment に。

### 食い違い（phase042 と phase.md）

phase.md と phase042 の食い違いは見つからなかった。phase.md の (b) の記述だけ上のとおり成り立たない。

### 未実施

zedBSD の x86_64 の build と `readelf`・`llvm-nm` の確かめ（7・8）、net-ring-test の tsan、`plan/ws074/tests/guest-build.sh`（旧い道具）、Windows の `tls-wine.sh`・`tls-wine-compat.sh`（p045）、arm64・riscv64、QEMU・実機。

### 消してほしい path（Q1 の pipeline で）

- `build/tmp/p043`（asan-host・compat・win64・tree・break の log と script を含む。checks の出力の置き場。Q1 が見た後に）
- `build/ws335-nettls-host`（run-nettls の host build の置き場。続けて使うなら残す）
- 古い `build/ws335-nettls-host-compat` はこの worktree には作っていない（main の checkout の分は Q1 の判断）。
- `plan/ws335/tests/` の compat の wrapper の build に頼る他の道具は無い（`run-nettls.sh` が唯一だった）。

## 追記（2026-11-13 Sonnet q2053-i02、基点 4b6d77f136）: 確かめ 7・8（zedBSD x86_64 の build）

makefile.diff の A・B・D〜G は当たった後。この worktree の `build/` に toolchain を入れた（`build/llvm`、Q1 の許可）。source は変えていない。

| コマンド | 結果 |
| --- | --- |
| `make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` | exit 0（3.6 s、cache `zedbsd-llvm-23.1.0-zedbsd11-x86_64-linux.tar.gz` を取得して展開） |
| `make -j4 CONFIGMK=config/config.x86_64.mk sysroot` | exit 0（19 s、`build/x86_64/sysroot/smoke.elf` まで link。`-Werror` 付きで build、warning 無し） |
| §4 の 1: `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/libcrypto-compat/.stamp build/x86_64/packages/libssl-compat/.stamp`（`-j4`） | exit 0、`CC` 40 件、`warning` 0 行 |
| §4 の 2: libbrowser（`STANDALONE_CFLAGS_libbrowser`・`STANDALONE_SO_LIST_libbrowser` は command line で上書き。Makefile・`mk/` には無かった。`mk/userland.mk` の依存には libssl-compat・libcrypto-compat が入っている） | exit 0（6 s）、`CC` 377 件、`SHLIB libbrowser.so`、`warning` 0 行 |
| mailer: `make -j4 CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/mailer/.stamp` | exit 0、`LD bin/mailer`、`warning` 0 行 |

確かめ 7: `llvm-readelf -d build/x86_64/dynamic/libbrowser.so` の NEEDED は libtruetype・libvulkan・libmedia・libjpeg-compat・libpng-compat・libz-compat・libgif-compat・libwebp-compat・**libssl-compat.so・libcrypto-compat.so**・libc・libm。OpenSSL の名前（libssl.so・libcrypto.so）は無い。`strings` で `openssl`（大小無視）・`libssl.so`・`libcrypto.so`・`WB_TLS` は 0 件。`build/x86_64/packages/libbrowser/.flags` に `WB_TLS` は 0 件。

確かめ 8: `llvm-nm -D --undefined-only` の `SSL_`・`X509_`・`ERR_`・`TLS_` の名前は 25 個（ERR_clear_error ERR_error_string_n ERR_get_error SSL_CTX_ctrl SSL_CTX_load_verify_locations SSL_CTX_new SSL_CTX_set_default_verify_paths SSL_CTX_set_options SSL_CTX_set_verify SSL_connect SSL_ctrl SSL_free SSL_get0_param SSL_get_error SSL_get_verify_result SSL_new SSL_pending SSL_read SSL_set1_host SSL_set_fd SSL_shutdown SSL_write TLS_client_method X509_VERIFY_PARAM_set1_ip_asc X509_verify_cert_error_string）。`libssl-compat.so`・`libcrypto-compat.so` の `--defined-only`（T・D・B・R・W）の合計 292 個と `comm -23` で突き合わせ、解けない名前は 0。この 25 個の他に OpenSSL 系（`ssl`・`SSL`・`EVP_`・`CRYPTO` を含む）の未定義の名前も無い。host の `tls.o` での 25 名の確かめと同じ集合。

未実施: arm64・riscv64 の build、QEMU・実機での TLS の実行（p046）、disk image。sub-make が -j16 を強いる事は無かった（`-j4` で通った）。

消してほしい path: 無し（この worktree の `build/llvm`・`build/x86_64` は残す。出力 log は scratchpad の外の `/tmp/claude-1000/*.log` のみ、Q1 が不要なら消す）。
