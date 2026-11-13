<!-- awesome-plan project=zedbsd record=ws335-p041 -->

# ws335-p041: mailer の TLS を libssl-compat・libcrypto-compat に切り替える

Status: cleared（2026-11-13 Q1 判定: compat に SSL_get1_peer_certificate・X509_digest（SHA-256）、mailer の ML_TLS_COMPAT。unit・ct・interop・faults・mailer の host 17・QEMU の guest 7 が PASS、check-skeleton は承認した header 3 file の差だけ（BASE を移す）。mailer の Makefile の 1 行はユーザーの判断待ち）
Parent: [WS335](../ws.md)

## 目的

2026-11-13 ユーザーの決定: WS335 に mailer の Phase を足し、終わってから WS335 を完了にする。

`userland/desktop/mailer/tls.c` は OpenSSL を名前で開き、自己署名の server の pin に `SSL_get1_peer_certificate` と `X509_digest` を使う。compat にはこの 2 つが無い（[p036](../phase036/phase.md) §4）。

## 範囲

1. libssl-compat に `SSL_get1_peer_certificate`（leaf の DER を SSL に残す）、libcrypto-compat に `X509_digest`（SHA-256）を足す。凍結の header の変更は Q1 が承認した（2026-11-13、check-skeleton の BASE はこの Phase の後に Q1 が移す）。
2. mailer の tls.c に `ML_TLS_COMPAT` で compat の名前だけを開く分岐（libbrowser の WB_TLS_COMPAT と同じ形）。mailer の Makefile の 1 行（`plan/ws335/phase036/mailer-Makefile.diff`）は差分の案としてユーザーへ（当てるまでの確かめは command line の -D で）。
3. 付随: top の Windows 版の入口が `make browser-win32`（ユーザーが mk/foreign.mk に足した）になったので、`plan/ws334/tests/cross-build.sh` の top の入口の段と comment、cockpit の UAT の手順の `make browser-win64-zip` を今に合わせる（cockpit は Q1 が直す。担当は案を返す）。
4. 確かめ: unit・interop の試験（自己署名の pin を含む）、x86_64 の build warning 0、QEMU の guest で mailer が TLS の server（試験の s_server）に pin で繋がる事。

## 結果（2026-11-13 P2 q2048-i01、branch `agent/p2-ws335e`、基点 main 327e229540、途中で main f592c01b00・142c8b28a6 を merge）

### 1. compat に足した 2 つ（凍結の header の変更は Q1 の承認済み）

- `SSL_get1_peer_certificate`（`<compat/ssl/ssl.h>`・exports.map）: handshake が終わった時に leaf の DER（`conn.hs.peer_der[0]`）から `X509` を作り `struct ssl_st` の `peer_certificate`（sslc-api.h）に 1 つの参照で持つ。get1 は参照を 1 つ足して返す（OpenSSL と同じく呼んだ側が `X509_free`）。SSL を free しても呼んだ側の参照は残る。接続の後の失敗（handshake の写しが消える）・shutdown の後も返す。verify の callback が通した自己署名の鎖でも返す。handshake の前・拒まれた handshake は NULL（error を積まない）。
  handshake の終わりの判定は `SSL_connect`・`SSL_read`・`SSL_write` の共通の `ssl_handshake`（暗黙の handshake も同じ道）。`X509` の確保に失敗した時はその呼びが -1・`SSL_ERROR_SSL`・error 1 つ（もう一度呼ぶと作り直す）。
- `X509_digest`（`<compat/crypto/x509.h>`・exports.map）: 保持している DER 全体を `EVP_Digest` で。`EVP_sha256()` だけを受け、他の md は `EVP_R_UNSUPPORTED_ALGORITHM`（EVP の error 1 つ）、NULL の certificate は X509 の `PASSED_NULL_PARAMETER`、NULL の md・出力は EVP の NULL、length は NULL 可。
- libbrowser の net/tls.c は `X509_digest` を「OpenSSL only」の任意の名前として探すが、使う道（`tls_refuses_distrusted`）は `X509_STORE_CTX_get_current_cert`・`_set_error` も要り、compat に無いので動きは変わらない。

### 2. mailer の tls.c

`ML_TLS_COMPAT` の build は `libcrypto-compat.so`・`libssl-compat.so` だけを開き（OpenSSL へ落ちない）、無ければ EPROTONOSUPPORT と「cannot load libssl-compat.so and libcrypto-compat.so (…)」。印の無い build（host）は OpenSSL のまま。誤りの文の「OpenSSL」を library を問わない文に。libbrowser の WB_TLS_COMPAT と同じ形（名前の表を build で選ぶ）。
mailer の Makefile の 1 行は案のまま当てていない（`plan/ws335/phase036/mailer-Makefile.diff`、今の tree に `git apply --check` で当たる）。確かめは command line の `'STANDALONE_CFLAGS_mailer=$(STANDALONE_PROGRAM_CFLAGS) -DML_TLS_COMPAT'`（差分 D と同じ値）で。

### 3. 確かめ

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| unit（足した 2 つ） | `WS335_CACERT=<取得して SHA-256 を照合した cacert-2026-08-13.pem> sh plan/ws335/tests/run-unit.sh api-ssl api-x509` | `test-api-ssl: PASS (5580 checks)`・`test-api-x509: PASS (507851 checks)`。足した試験: `test_peer_certificate`（TLS 1.3・1.2 × P-256・P-384、前・待ち・後・参照の数・同じ object・後の fatal alert・shutdown・SSL_free の後・暗黙の handshake・拒まれた鎖）、`test_digest`（SHA-256 の値と length、NULL・他の md の error、length を変えない） |
| unit 全部・Windows・定数時間 | `sh plan/ws335/tests/run-unit.sh all`・`run-unit.sh --win32 all`・`sh plan/ws335/tests/run-ct.sh all` | 3 つとも PASS（host 59 本、Windows（Wine）59 本、ct 19 本） |
| check-skeleton | `BASE=327e229540 sh plan/ws335/tests/check-skeleton.sh` | FAIL は 5 段目（凍結の差）の 1 件だけ: `include/libc/compat/crypto/x509.h`・`include/libc/compat/ssl/ssl.h`・`userland/base/libssl-compat/sslc-api.h`（承認済みの header の変更）。1〜4・6・7 段は PASS。BASE は Q1 が移す |
| zedBSD の build（x86_64） | `make CONFIGMK=plan/ws335/tests/config-p036.mk BUILD_JOBS=4 'STANDALONE_CFLAGS_mailer=$(STANDALONE_PROGRAM_CFLAGS) -DML_TLS_COMPAT' build/x86_64/packages/libcrypto-compat/.stamp build/x86_64/packages/libssl-compat/.stamp build/x86_64/packages/mailer/.stamp` | exit 0、warning 0。mailer の `.flags` に `-DML_TLS_COMPAT`、`bin/mailer` の文字列は `libcrypto-compat.so`・`libssl-compat.so`（と失敗の文）だけ |
| export | `sh plan/ws335/tests/check-exports.sh build/x86_64/dynamic/libcrypto-compat.so build/x86_64/dynamic/libssl-compat.so` | `check-exports: PASS`（crypto public 79＋cryc_ 190、ssl public 23）。`SSL_get1_peer_certificate`・`X509_digest` が T |
| mailer の host 試験（新） | `sh plan/ws335/tests/run-mailer-tls.sh` | `run-mailer-tls: PASS (17 cases)`: mailer の tls.c を OpenSSL と `-DML_TLS_COMPAT`（host の compat の .so）の 2 build で、host の `openssl s_server -rev` に。自己署名（TLS 1.3 P-256・TLS 1.2 RSA）の pin 無し → ML_ERROR_UNTRUSTED で指紋が `openssl x509 -fingerprint -sha256` と一致、その pin → 繋がり 1 行ずつ往復、他の cert の pin → 断る。CA の署名の server は CA の file で pin 無しに繋がり、file 無しは断る。compat の build は library 無しで EPROTONOSUPPORT（OpenSSL に落ちない）。ld.so の log: compat の run は libssl-compat を 8 回 init・OpenSSL の libssl 0 回、OpenSSL の run は逆 |
| 相互接続・確保の失敗（host） | `sh plan/ws335/tests/run-interop.sh`・`sh plan/ws335/tests/run-faults.sh` | `run-interop: PASS (139 cases, 0 of them known limits)`、`run-faults: PASS`（TLS 1.3 は確保 17 回（leaf の `X509` を含む）、各回の失敗で 16 が error・1 が完了・壊れ 0、TLS 1.2 は 15 回で 14・1・0） |
| QEMU の zedBSD の mailer の TLS（新） | `BUILD_JOBS=3 sh plan/ws335/tests/run-mailer-guest.sh`（QEMU `ws335-p2-p041`、snapshot、serial と QMP は unix socket、image は既定の一覧から `*-firmware` を除いた 241 個＋`CONFIG_GRAPHICAL_LOGIN=n`＋上の mailer の flag） | `run-mailer-guest: PASS (7 cases)`。guest の `/bin/mailer` が開く名前は `libcrypto-compat.so libssl-compat.so` だけ。image の mailer と同じ flag の tls.c で作った試験の program を serial で送り（cksum 一致）、guest から host の s_server（10.0.2.2）へ: 自己署名 1.3 の pin 無し → UNTRUSTED（指紋が host の openssl と一致）、その pin → 繋がる、他の pin → 断る、1.2 RSA の pin → 繋がる、他の pin → 断る、CA の file → 繋がる、無し → 断る。1 回目の run は 7 件とも PASS だったが image の build の noct の warning 3 件（toolchain 側の既存）を FAIL と数えたので、script を noct を別に数える形に直して流し直した。判定は guest の program の最後の行（serial）で、console の log は読んでいない |

### 4. Windows の入口（付随）

`plan/ws334/tests/cross-build.sh`: top の入口を `make browser-win32 WIN32_ARCH=<arch>`（user が mk/foreign.mk に足した物、素のまま）に、zip を `make -f apps/browser-win32/Makefile WIN32_ARCH=<arch> zip` に。既定は x86_64（CI）、`WIN32_ARCH=aarch64` か `"x86_64 aarch64"`。
2026-11-13 のユーザーの決定（Q1 から: vulkan-1.dll の遅延の読み込みは要らない、`--no-gpu` は成り立たない）に合わせ、4 を「OmniGrammaton.exe の import に vulkan-1.dll があり delay-import の表が無い」に、6a・6 は vulkan-1.dll を普通の import として許し遅延は不可に、delay-load.c と Wine の with/without の確かめを外した。`plan/ws334/tests/host-tests-windows.sh` の 5（vulkan-1.dll を無効にして delay-load の例外でない事）も外した（Q1 が範囲に足した）。

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| 基点の tree（327e229540 と自分の変更） | `sh plan/ws334/tests/cross-build.sh`（その時の形: x86_64・arm64、遅延の import の確かめあり、config.mk の無い checkout なので `CONFIGMK=config/config.x86_64.mk` を渡した） | `cross-build: PASS`（win64・winarm64: warning 0、import の表、OmniGrammaton.exe は system の DLL 24（vulkan-1.dll は遅延）、zip 23 file で同じ byte、Wine で delay-load・乱数・script） |
| main f592c01b00 を merge した tree | 同じ（x86_64） | `cross-build: FAIL`: その main の `apps/browser-win32/Makefile:50` の `-Wno-maybe-uninitialized` を llvm-mingw の clang が `-Werror,-Wunknown-warning-option` で拒み 1 つ目の file で止まる（P3 の直し待ち） |
| main a6c6628101 を merge した tree（clang の flag が直った後） | `WIN32_ARCH="x86_64 aarch64" sh plan/ws334/tests/cross-build.sh`（今の形: 素の `make browser-win32`、vulkan-1.dll は普通の import） | `cross-build: FAIL`、失敗は 7（zip）だけで win64・winarm64 とも同じ: zip に `share/licenses/` の `Apache-2.0.txt`・`LLVM-LICENSE.txt`・`MinGW-w64-runtime.txt`・`winpthreads.txt`・`libwebp-COPYING.txt` が無い（main の `apps/browser-win32/Makefile` の `OMNIGRAMMATON_LICENSES` から 5 行が消えた。.exe に入る物の許諾の文なので試験の側は直さない、ユーザーの判断）。他は PASS: warning 0、compat と libtruetype の warning 0、import の表、public 65 関数と exports.map、vulkan-1.dll は OmniGrammaton.exe の普通の import で delay-import の表無し、OmniGrammaton.exe は system の DLL と vulkan-1.dll（25）、pthread_ 無し、test program は system の DLL 13、Wine で乱数・script（x86_64） |

### 5. 返す物

- Makefile の案（当てていない）: `plan/ws335/phase036/mailer-Makefile.diff`（`userland/desktop/mailer/Makefile` に `STANDALONE_CFLAGS_mailer += -DML_TLS_COMPAT` と 2 行の comment。今の tree に当たる）。当てた後の確かめ: `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 build/x86_64/packages/mailer/.stamp` → `.flags` に `-DML_TLS_COMPAT`、`IMAGE` を与えずに `sh plan/ws335/tests/run-mailer-guest.sh`（その時は script の command line の mailer の変数は差分と同じ値で害は無い。外すかは当てた後に）。
- 消す候補（Q1 の pipeline で）: `plan/ws334/tests/delay-load.c`（cross-build.sh の 4 を外したので、どこからも使われない。記録の phase.md の言及だけ）。worktree の `build/p041/`・`build/tmp/ws335-*`・`build/tmp/ws334-cross`・`build/x86_64`（image 込み）。
- 未実施: `run-nettls.sh`（host の browser の build 2 つ、各 1.2 GB。libbrowser は変えておらず、compat の新しい 2 つの名前は libbrowser の道で使われない: §1 の最後）、`run-limbo.sh`（証明書の検証は変えていない）、`host-tests-windows.sh` の実行（5 を外しただけ、構文の確かめのみ。全体は Wine で数時間）、mailer の GUI の操作での接続（mailer に非対話の口が無いので、image の mailer と同じ flag の tls.c を試験の program で guest で動かした）、aarch64 の Windows の program の Wine（cross-build は arm64 を link まで）、i386・rpi・vf2 の image、実機。
