<!-- awesome-plan project=zedbsd record=ws335-p045 -->

# ws335-p045: Windows（OmniGrammaton）の TLS を表の無い net/tls.c で確かめ、使われなくなった wb_os_trust_roots を消す

Status: planned
Parent: [WS335](../ws.md)。設計: [p042](../phase042/phase.md)（§1.1・§1.3・§2 の 3・§4 の C・問い Q4）。実装の担当: phase-runner-low（Sonnet 5.5 high）、1 回の起動。依存: p043 の merge（と makefile.diff の C。当たっていなければ写しで）。p044 と並べてよい。

## 0. 何をするか（要約）

p043 の net/tls.c（`#ifdef` 無し、compat を直に呼ぶ）で、Windows の program（OmniGrammaton.exe と試験の tls-tool.exe）が build でき、Wine の https の試験の組（a〜g）が通る事を確かめ、Windows の試験の道具を表の無い形に直す。
問い Q4 の答えが A（消す）なら、どこからも使われなくなった `wb_os_trust_roots` を OS の層と WS334 の単体試験から外す。答えが B なら §2.2 を飛ばし、os-system.h の comment の「OpenSSL」だけを直す。

## 1. 読む物

- `AGENTS.md` の §4・§7・§9・§10。`plan/agents/protocol.md`。`plan/coding-style.md`（全文）。
- 設計: `plan/ws335/phase042/phase.md` の §1.1・§1.3・§2・§4。p043 の phase.md の結果の節。p036 の phase.md（Windows の試験の流し方: `tls-wine.sh`・`cross-build.sh`・llvm-mingw・Wine）。
- `include/libc/compat/ssl/ssl.h` の `SSL_CTX_set_default_verify_paths` の comment（Windows の ROOT・Disallowed・.exe の隣の cert.pem）。
- `apps/browser-win32/Makefile`（読むだけ）、`plan/ws335/phase042/makefile.diff` の C。
- 試験: `plan/ws334/tests/tls-wine.sh`・`tls-main.c`・`cross-build.sh`・`run-unit.sh`・`unit/test-os-system.c`。
- OS の層: `userland/desktop/libbrowser/base/os-system.h:30-60・150-172`、`os-system-posix.c`（`wb_os_trust_roots` の周り）、`os-system-win32.c`（同、`CertOpenStore` などの include と link の flag）。

## 2. 作る物・変える物

### 2.1 Windows の試験の道具

1. `plan/ws334/tests/tls-wine.sh`:
   - 1 の build の確かめの「`net/tls-static-compat.o` が build にある」を、「`$build/obj/userland/desktop/libbrowser/net/tls-static-compat.o` も `tls-static.o` も無い」と「`llvm-nm $build/obj/userland/desktop/libbrowser/net/tls.o` に `U SSL_connect`・`U SSL_CTX_set_default_verify_paths` があり `tls_static_library` が無い」に替える（tls.c が compat を直に呼ぶ）。
     注意: build の dir を前の run と共有していると、消えた source の古い object が残りうる。`$build` が前の run の物なら、確かめの前に Q1 に消してもらうか、新しい `BROWSER_WIN32_BUILD` で流す（担当は rm しない）。
   - 2 の「OpenSSL's own functions」の確かめから ` X509_digest$` を外す。p041 で libcrypto-compat が `X509_digest` を持ち（`api-x509.c`）、.a の object ごと link されるので、今の確かめは compat の名前を OpenSSL の物と誤って数える。残す名前は `OPENSSL_init_ssl`・`X509_STORE_CTX_set_error`（compat に無い）。
   - 先頭の comment の「net/tls-static-compat.c fills tls.c's table」を「net/tls.c calls libssl-compat and libcrypto-compat, linked into the program」に。
   - 試験の a〜g（a: 試験の CA で繋がる、b: CA 無しで断る、c: 名前の違い、d: Disallowed の証明書を断る（`certificate rejected`）、e: share/certs の束、f: OPENSSL_CONF を読まない、g: .exe の隣の cert.pem）は変えない。
2. `plan/ws334/tests/cross-build.sh`: tls-static に触れる所があれば同じく直す（`grep -n 'tls-static\|WB_TLS' plan/ws334/tests/cross-build.sh`、無ければ変えない）。
3. `plan/ws334/tests/net-ring-test.sh`: p043 が残した tls-static の 2 つを除く `grep -v` は、Q1 が file を消した後なら外す（消えていなければ残し、結果に書く）。

### 2.2 `wb_os_trust_roots` を消す（Q4 が A の時だけ）

1. `userland/desktop/libbrowser/base/os-system.h`: `wb_os_trust_roots` の宣言と comment、それだけが使う型（`wb_os_trust_each` と、その comment。30-60 行の辺り）を外す。他の宣言は変えない。
2. `os-system-posix.c`・`os-system-win32.c`: 実装を外す。それだけが使う include・static の補助関数・定数（Windows の `<wincrypt.h>`・`CertOpenStore` の周りなど）も外す。`wb_os_library_*`・`wb_os_data_directory` は残す。
   Windows の link の flag（`-lcrypt32`）は compat の cryc-os-win32 も使うので、Makefile の側は変えない（Makefile は変えてはいけない）。
3. `plan/ws334/tests/unit/test-os-system.c`: `wb_os_trust_roots` の試験（`struct trust_context`・`trust_each`・その試験の関数と呼び出し、file の先頭の comment の「trusted」の語）を外す。他の case は変えない。
4. `grep -rn 'wb_os_trust\|trust_each\|trust_context' userland plan/ws334 plan/ws074 plan/ws335/tests plan/tools` が 0 件。

Q4 が B の時: `os-system.h:165-166` の「(OpenSSL reads the system's file itself)」を「(the TLS library reads the system's file itself)」に直すだけ。

## 3. 確かめの項目

| # | 確かめ | 期待 |
| --- | --- | --- |
| 1 | `sh plan/ws334/tests/tls-wine.sh`（makefile.diff の C が main に当たった後の素の Makefile。当たっていなければ下の §4 の 2） | `tls-wine: PASS`（a〜g、import に ssl・crypto の DLL 無し、`SSL_connect` と `sslc_` がある） |
| 2 | `WIN32_ARCH="x86_64 aarch64" sh plan/ws334/tests/cross-build.sh` | 今の main と同じ結果（p041 の時点では zip の許諾の 5 file の欠けだけで FAIL。流す前に今の main で 1 回流して比べる）。warning 0、OmniGrammaton.exe の import に ssl・crypto の DLL 無し |
| 3 | `sh plan/ws334/tests/run-unit.sh`（`os-system` の単体試験を含む。posix と Wine） | PASS（Q4 が A なら trust の case が無い） |
| 4 | `sh plan/ws074/tests/host-build.sh plain` | build が通る（POSIX の os-system の変更の確かめ） |
| 5 | `git diff --check` | 出力無し |

## 4. 確かめのコマンド

1. 素の Makefile（C が当たった後）: 上の表のとおり。
2. C が当たっていない時: p043 の phase.md §4 の 3 と同じに写しを作り、`tls-wine.sh` が使う make の `-f` を写しに替えて流す（`tls-wine.sh` に Makefile の path を渡す変数が無ければ、script の写しを `build/tmp/p045/` に置いて 1 行だけ替える。commit しない）。
3. llvm-mingw・Wine の場所は `cross-build.sh`・`tls-wine.sh` が使う物（p036 の結果）。各コマンドに時間の上限、`-j4` まで、大きな build の後に `sync`。

## 5. 注意

- Windows の根は compat の `SSL_CTX_set_default_verify_paths` が ROOT（Disallowed を除く）と .exe の隣の cert.pem を読み、Disallowed を鎖のどこでも拒む。tls.c は Windows で何もしない（d の `certificate rejected` は compat が出す）。
- WSAStartup は os-net の層が呼ぶ（p036 で確かめ済み）。変えない。
- `wb_os_trust_roots` を消す時、Windows の file の他の関数が使う include を消さない（compile して確かめる）。

## 6. 変えてはいけない物

全ての header（compat）、compat の .c、`net/tls.c`（p043 の物。不具合は Q1 に返す）、mailer、`apps/browser-win32/Makefile` と全ての Makefile・`mk/`、HAL・toolchain・共有の `build/`、§2 に名指していない他の WS の file。

## 7. 終わりの条件

- §3 の 1〜5 が全部期待どおり（出力を自分で読む）。規約の checklist で diff を見直した。
- わざと壊す確かめ（git の外で）: tls-wine.sh の d（Disallowed）が、`SSL_CTX_set_default_verify_paths` を呼ばない tls.c の写しで FAIL する事（写しは `build/tmp/p045/` に、commit しない）。結果に書く。
- commit: `git commit -m WIP -- plan/ws334/tests/tls-wine.sh plan/ws335/phase045/phase.md plan/ws335/ws.md`、Q4 が A なら `userland/desktop/libbrowser/base/os-system.h userland/desktop/libbrowser/base/os-system-posix.c userland/desktop/libbrowser/base/os-system-win32.c plan/ws334/tests/unit/test-os-system.c` も、直したなら `plan/ws334/tests/cross-build.sh`・`net-ring-test.sh` も。push しない。
- 結果の節: コマンドと最後の行、自分で決めた点、未実施（aarch64 の Wine での実行など）、消してほしい path。ws.md の行は「実装済み・試験 PASS」。

## 結果

（実装の担当が書く）
