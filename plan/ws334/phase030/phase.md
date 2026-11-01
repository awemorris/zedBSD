<!-- awesome-plan project=zedbsd record=ws334-p030 -->

# ws334-p030 統合: OpenSSL の Windows の build と信頼する根（packages の ca-certificates ＋ Windows の store）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §6.6（S-A）、決定 16・18。
- 状態: cleared（2026-11-01 22:58 Q1、main で merge。Makefile と cross-build.sh の衝突を p026 と両方残して解き、合わせた tree で cross-build PASS（warning 0、1 つの .exe の import は system の DLL 13））。前: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1857-i01、Sonnet）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: OpenSSL は `no-shared` の static library（`libssl.a`・`libcrypto.a`）にして .exe に link する。DLL の名前の確かめは要らない。net/tls.c の表は Windows では `net/tls-static.c`（Windows の build だけが compile、sources.mk に書かない）が `<openssl/ssl.h>` の関数の 1 行の wrapper で埋め、開かない。始める前に、表の各行に「OpenSSL だけ」の印を足し、その行が無くても TLS が動く形にする（design §6.6、WS335 の D28 の 1）。手順 3 の「DLL が無い時」の EPROTONOSUPPORT は zedBSD・Linux だけ。受け入れの「DLL を作り」は「static library を作り」に読み替える。
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

## 結果（2026-11-01、q1857-i01、Sonnet、基点 main 3c076f4e8）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| ced9390093 | `libbrowser/net/tls.c`（「OpenSSL だけ」の印、根・Disallowed・設定を読まない・同梱の束）、新しい `net/tls-library.h`（関数の表）・`net/tls-static.c`（Windows の build だけ） |
| ac59023c55 | `apps/browser-win32/Makefile`（`openssl` の target、tls.c の `-DWB_TLS_STATIC`、tls-static.c、link）、`plan/ws334/tests/openssl-windows.sh`・`openssl-conf/10-mingw-arm64.conf`・`tls-main.c`・`tls-wine.sh`、`cross-build.sh`（定義済みの名前に libssl.a・libcrypto.a を足した） |

作った物と決めた形:

1. **OpenSSL の static library**（`plan/ws334/tests/openssl-windows.sh <x86_64|aarch64> <出力>`）: `userland/packages/security/openssl/Makefile` から版 3.5.8・SHA-256・URL を読み、共有の `build/distfiles/openssl-3.5.8.tar.gz`（あれば読むだけ、SHA-256 照合）を自分の出力の `work/` に展開して llvm-mingw で build。`--prefix=C:/Windows/System32/OmniGrammaton-no-prefix --openssldir=C:/Windows/System32/OmniGrammaton-no-config no-shared no-tests no-docs no-apps no-engine no-dso no-module threads`。x86_64 は `mingw64`（asm あり）、aarch64 は下の自前の target（no-asm）。出力は `<出力>/lib/libssl.a`・`libcrypto.a`・`include/openssl/`（`BROWSER_WIN32_BUILD/openssl/`、Makefile の `openssl` の target と、`libraries`・`test-program` の依存）。確かめ: `nm` で `SSL_CTX_new`・`X509_STORE_add_cert` の定義、`strings` で openssldir、`llvm-objdump -f` で machine。
   - Configure は prefix が host の絶対 path でないと止まるため `--cross-compile-prefix=<triple>-` を付けた（prefix が全部の道具の名前の前に付くので、`CC=clang AR=ar RANLIB=ranlib RC=windres` を PATH の llvm-mingw の bin から）。`make install_dev` は prefix が Windows の形だと staging の隣に書くので使わず、header と 2 つの .a を build 木から写す。
   - 3.5.8 の `Configurations/10-main.conf` に `mingw-arm64` は無い（mingw・mingw64 のみ。arm64 は MSVC の clang-cl 用）。Q1 の了承（2026-11-01）で、tarball を変えず `OPENSSL_LOCAL_CONFIG_DIR` が指す `plan/ws334/tests/openssl-conf/10-mingw-arm64.conf`（`mingw-common` を継ぐ、no-asm）で作った。**aarch64 の libssl.a・libcrypto.a は build・link できた（machine は coff-arm64、`tls-tool.exe` は ARM64 の PE）が、実行は未確認**（arm64 の Wine・実機が無い）。
2. **net/tls.c**: 関数の表（`struct tls_library`）を `net/tls-library.h` に出し、表の行に「OpenSSL だけ」（`optional`）の印を付けた: `OPENSSL_init_ssl`・`X509_digest`・`EVP_sha256`・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error`（design §6.6・WS335 D28 の 1）。その行が無い library（開く側で見つからない時）は NULL のままで、設定を切る・Disallowed の指紋・検証の callback を使わずに TLS が動く（context は callback 無しの `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL)`、今の zedBSD と同じ）。p030 が使う `SSL_CTX_get_cert_store`・`d2i_X509`・`X509_STORE_add_cert`・`X509_free` は必須の行（WS335 の公開の部分集合にある）。
   - context を作る時: (1) `init_ssl` があれば `OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL)`（context の前）、(2) `wb_os_trust_roots` の distrusted 0 の DER を `d2i_X509` → `X509_STORE_add_cert`（parse できない物は飛ばす）、distrusted 1 は SHA-256 の指紋を一覧に（上限 1024、超えるか指紋が取れなければ context を作らず EPROTONOSUPPORT: 通してしまう方に倒さない）、(3) 一覧が空でなければ検証の callback（`tls_verify`: 鎖の各証明書の指紋を照合し、一致すれば `X509_STORE_CTX_set_error(X509_V_ERR_CERT_REJECTED)` で 0）を `SSL_CTX_set_verify` に、(4) 同梱の束 `wb_os_data_directory` + `/certs/ca-certificates.pem` を `SSL_CTX_load_verify_locations`（無い・読めない時は黙って続ける）、(5) 追加の CA file（`browser_add_ca_file`、今のまま）。`SSL_CTX_set_default_verify_paths` は「`wb_os_trust_roots` が 1 つも根を渡さなかった時」だけ呼ぶ（POSIX は 0 を返すので今まで通り呼ぶ、Windows は ROOT が空でない限り呼ばない）。`#ifdef` は書いていない。
   - library の束縛: `WB_TLS_STATIC` を付けた build（Windows）では `tls_bind_library` が `tls_static_library`（`net/tls-static.c`）で表を埋め、何も開かない。それ以外は今の `wb_os_library_open`。`#ifdef WB_TLS_STATIC` は tls.c の 2 か所（表・名前・探す関数の block と、`tls_bind_library` の 2 つの定義）。WS335 の p036 の変更は、この 2 か所の差し替えと `#ifdef` 無しの根の loop の側の変更だけで足りる見込み。
   - `net/tls-static.c`: `<openssl/*.h>` を include し、表の member と同じ型の 1 行の wrapper（関数 pointer の型の変換なし）。検証の callback だけは OpenSSL の型（`X509_STORE_CTX *`）の trampoline を挟み、tls.c の callback を static 変数に持つ。sources.mk には書かず、Windows の Makefile だけが `LIBBROWSER_CORE_SOURCES` に足し、`tls.o` に `-DWB_TLS_STATIC`、`tls-static.o` に `-I<openssl>/include`。
3. **zedBSD が openssl.cnf に頼る所**: `userland/`・`platform/`・`config/`・`tools/` の source・Makefile・script を検索して、libbrowser が `openssl.cnf`・`OPENSSL_CONF` に頼る所は無い（package が `install_ssldirs` で stock の cnf を置くだけ）。zedBSD でも `OPENSSL_INIT_NO_LOAD_CONFIG` を呼ぶ形のままにした。

確かめ:

- `timeout 3500 sh plan/ws334/tests/openssl-windows.sh x86_64 build/tmp/p030/o64c` → `openssl-windows: x86_64 libssl.a 1567620 bytes, libcrypto.a 9050542 bytes`・`PASS`。aarch64（`.../oa64`）→ libssl.a 1474696・libcrypto.a 7428178 bytes、`PASS`。
- `timeout 3500 sh plan/ws334/tests/tls-wine.sh` → 最後の行 `tls-wine: PASS`（最終の run。log `build/tmp/p030/tls-wine.run`）。host の `openssl s_server`（試験の CA と 127.0.0.1 の server 証明書を実行ごとに作る、外の network なし）へ、blocking と loader の両方で: a. `--ca-file` で足すと 200・20907 byte、b. 足さないと `certificate verify failed: unable to get local issuer certificate`（EPROTO）、c. 試験の program が Wine の current user の ROOT に入れると足さずに 200、d. 同じ CA を Disallowed にも入れると `certificate verify failed: certificate rejected`、e. その CA を `<.exe の隣>/share/certs/ca-certificates.pem` に置くと 200、f. `OPENSSL_CONF` に「base provider だけを activate する」設定（読まれると algorithm が無くて TLS が失敗する）を置いても 200。`strings` に openssldir（`C:/Windows/System32/OmniGrammaton-no-config`）。
- わざと壊す確かめ（tls.c を git の外で変えて tls-wine.sh、元に戻して `cmp` で確認）: (1) `OPENSSL_init_ssl` の呼びを外す → f が FAIL（`cannot make an OpenSSL context`。最初の f の設定は不正な行だけで、(1) でも PASS してしまったため、上の「provider だけ」の設定に直した）、(2) 検証の callback を付けない → d が FAIL、(3) 束の file 名を変える → e が FAIL、(4) 根を store に足さない → c と d が FAIL。
- Wine の癖（試験の program に反映）: 登録簿ベースの system store は、書いた変更が他の open の読みに出るのに `CertControlStore(CERT_STORE_CTRL_COMMIT)` が要る（足した後と消した後に呼ぶ）。Wine の ROOT は既に約 155 個（current user と machine で同じ物を二重に）持つ。Disallowed の store は Wine でも列挙でき、d は Wine の上で確かめられた（p005 の「Disallowed は実機で」は、少なくとも current user の Disallowed では Wine でも通る。実機の Windows は未確認）。
- `timeout 3400 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（win64・winarm64 とも warning 0、libbrowser.a の未定義の名前が表の中、libkeiland.a に pthread の名前なし、1 つの .exe の import は system の DLL 13、win64 は Wine で Vulkan の遅延 import・random・Date）。OpenSSL を足したので、libbrowser.a の未定義の名前の確かめに libssl.a・libcrypto.a の定義を足した（SSL_* などが libbrowser.a の未定義に出る）。
- zedBSD・Linux の host の TLS の試験（`plan/ws334/tests/stage1.sh TREE plain 300` と、基点 3c076f4e8 の `git archive` の木で同じ物）: どちらも host-build exit 0 warnings 0 built 94、host-tests 75 exit 0・17 other（`host-exits.txt` の 92 行が基点と完全に一致）、js 17/17、loader 11/11、http 15/15（https の 4 case と失敗 2 case を含む）、http-async 19/20（基点も同じ 19/20）、fetch 38/38、component 83。asan は未実施（host の plain だけ）。
- zedBSD amd64: main の `build/amd64/sysroot` を `build/tmp/p030/sysroot-amd64` に写し、tree の `include/libc`・`userland/desktop/include` を重ねて（p020 以降と同じ）`make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=<写し> -o <写し>/.zedbsd-sysroot-complete -j16 build/amd64/dynamic/libbrowser.so` → exit 0、warning 0（`check-dynamic-elf.py` は PATH の python3 で通った）。OpenSSL の package（`userland/packages/security/openssl/`）は読んだだけで変えていない。
- `git diff --check` → 出力なし。

自分で決めた点:

1. aarch64 の target は自前の `.conf`（上）。OpenSSL に `mingw-arm64` が無い件は Q1 に先に報告し、了承を得た。aarch64 は no-asm（asm の arm64 の mingw の scheme が無い）なので、速さは x86_64 より劣る。
2. `no-autoload-config`（OpenSSL の build の option）は付けなかった。付けると「設定を読まない」が OpenSSL の側で常に成り立つが、`OPENSSL_INIT_NO_LOAD_CONFIG` を呼ぶ tls.c の側の試験（f）が空になるため（design の通り呼びで守る）。付けても害は無いので、Q1 が望めば足せる。
3. 同梱の束の path は phase.md の通り `<data directory>/certs/ca-certificates.pem`（Windows は .exe の隣の `share\certs\`）。design §6.2・§6.10 の `cert.pem`（.exe の隣）とは名前が違う: p031 が束を置く時に 1 つに揃える（WS335 の切り替えの時に compat が読む名前も `cert.pem`）。
4. ROOT が空の Windows（根が 0 個）では `SSL_CTX_set_default_verify_paths` を呼ぶ。openssldir は存在しない path なので何も読まないが、OpenSSL は環境変数 `SSL_CERT_FILE`・`SSL_CERT_DIR` も見る（利用者が決められる）。通常の Windows は ROOT が空でないので呼ばない。気になる場合は Windows では呼ばない形に変える（`#ifdef` が要る、または根が 0 個でも呼ばない選択）。
5. Disallowed の指紋の数は 1024 を上限にし、超えたら TLS を使えなくする（通してしまう方に倒さない）。
6. 試験の program の ROOT の store への追加は current user の store だけ（machine の store は書かない）。実機の Windows では ROOT への追加に確認の dialog が出る（Wine は出ない）ので、この試験は Wine 専用。

未実施・残り:

- aarch64 の実行（build・link・`tls-tool.exe` の machine の確認だけ）、実機の Windows の確認（Windows の ROOT・Disallowed の実際の中身、`CertEnumCertificatesInStore` の順、Windows の store が空の時の `SSL_CERT_*` の挙動）、asan の stage1、外の network の https（実在の site）。
- 共有の `build/distfiles` への写しは不要（tarball は既にあった）。Q1 に頼む消す物（rm 不要）: `build/tmp/p030/`（o64・o64b・o64c・oa64・w64・base・sysroot-amd64・diag ほか）、`build/tmp/ws334-tls-wine.run.*`・`ws334-tls-build`・`ws334-tls-wine-prefix`・`ws334-cross.run.*`・`ws334-stage1-plain` など（使い捨て。`o64`・`o64b` は make の失敗の跡で使わない）。
- p031（配る形）が束を `share/certs/ca-certificates.pem` に置くか `cert.pem` に揃えるかの 1 点（上の 3）。

### Q1 の判断（2026-11-01 22:58）

- 同梱の束の名前は phase.md の share/certs/ca-certificates.pem に揃える。design の cert.pem は p031（配る形）で直す。
- 根が 0 個の時に Windows でも SSL_CTX_set_default_verify_paths を呼ぶ件は、このままにする（普通は ROOT の store が空でなく通らない。環境変数は利用者自身が設定する物）。
- no-autoload-config は付けない（試験 f が意味を持つため）。

