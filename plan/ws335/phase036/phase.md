<!-- awesome-plan project=zedbsd record=ws335-p036 -->

# ws335-p036: libbrowser の net/tls.c の libssl-compat・libcrypto-compat への切り替え

Status: in-progress（2026-11-13 P2 q2029-i01。source の実装・host の試験・Wine の試験・zedBSD の build は済み。Makefile・mk/ の 3 つの差分は案（ユーザーの確認待ち）。QEMU の画面の確かめは下の「結果」。cleared は Q1）
Parent: [WS335](../ws.md)。担当: P2（q2029-i01、worktree `p2-ws335c`）。依存: p033（cleared）、WS334 の p022・p030（cleared）。

前の版（2026-11-01 p001 の提案、Status planning）を 2026-11-13 に今の main（500159a81d）と照らして書き直した。前の版の §1〜§3 の要点は下の「形」と「乖離の確かめ」に入れた。

## 1. 形（[design.md](../design.md) §4.2・§4.4、U2・U5 の答え）

- **zedBSD（U2 の A）**: libbrowser を `-DWB_TLS_COMPAT` で compile した build では、net/tls.c は `libcrypto-compat.so`・`libssl-compat.so` だけを開く。無ければ https は `cannot load libssl-compat.so and libcrypto-compat.so` で EPROTONOSUPPORT（OpenSSL へは落ちない。OpenSSL への依存を無くす目的に対し、欠けを隠さないため。2026-11-13 Q1 の指示）。印の無い build（Linux の host の試験）は今までどおり OpenSSL の `libcrypto.so`・`libssl.so`（と `.so.3`）だけを開く。
- **Windows（static link）**: `net/tls-static-compat.c`（新しい file）が `<compat/ssl/ssl.h>`・`<compat/crypto/err.h>`・`<compat/crypto/x509.h>` を include し、`struct tls_library` の各 member と同じ型の 1 行の wrapper で表を満たす（関数 pointer の型を変換しない）。「OpenSSL only」の 5 つの member は NULL。OpenSSL の `net/tls-static.c` は Windows の Makefile が切り替わるまで残す（切り替えの後は使われないので Q1 に削除を頼む）。
- **compat の下の net/tls.c**（D28 の 1〜4）:
  1. 表の「OpenSSL only」の印（`OPENSSL_init_ssl`・`X509_digest`・`EVP_sha256`・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error`）は WS334 p030 の後の main に既にある（任意の行、無ければ NULL）。
  2. `OPENSSL_init_ssl` は NULL なら呼ばない（既にそうなっている）。
  3. **直した所**: 表に指紋と検証の状態の関数が無い library（libssl-compat）では、OS に依らず `SSL_CTX_set_default_verify_paths` を呼び（失敗は context の失敗）、`wb_os_trust_roots` の DER を入れる loop と Disallowed の指紋の一覧と検証の callback を使わない（`tls_refuses_distrusted`）。OpenSSL では今までどおり。
  4. Windows の static の表 = `tls-static-compat.c`。
- **根の file**: Windows は今の `share/certs/ca-certificates.pem`（WS334 p031、`tls_add_bundle` が読む）が compat でも働く（下の試験の e）。.exe の隣の `cert.pem`（U5）は compat が置かれていれば読む（試験の g）。配る zip には新しい file を足さない案（同じ束を 2 つ置かない）。

## 2. 乖離の確かめ（2026-11-13、main 500159a81d と WS074・WS334 の今の記録）

| 前の版の記述 | 今の main・記録 | 扱い |
| --- | --- | --- |
| libbrowser の Makefile が zedBSD の build で `-DWB_TLS_COMPAT` を付ける | build system の作り直し（2026-11-06）の後、libbrowser は `userland/base/package.mk` の `USERLAND_PACKAGE`（shared-library）で compile され、package ごとの CFLAGS の口が無い（`STANDALONE_CFLAGS_libbrowser` = `STANDALONE_SHLIB_CFLAGS`）。Makefile・mk/ はユーザーだけが変える（2026-11-12）。 | 差分の案 A（§4）。確かめは make の command line で同じ変数を与えた |
| 読み込みは WS334 p022 の後の `wb_os_library_open` | main の net/tls.c は `wb_os_library_open`・`wb_os_library_symbol` を使う | 一致 |
| WS334 p030 が表に OpenSSL だけの関数を足すので「開く名前だけ」では済まない | 足された 5 つは「OpenSSL only」の任意の行で、NULL で動く形（D28 の 1・2 は済み）。ただし根の扱い（D28 の 3）は未対応だった: `tls_add_roots` は OS が DER を渡した時（Windows）に `SSL_CTX_set_default_verify_paths` を呼ばない。compat を Windows に link すると ROOT は loop で入るが、Disallowed は指紋の関数が無いので覚えられず、default paths も呼ばれないので、**Disallowed の証明書が拒まれない**（`tls_remember_distrusted` の comment「other implementation does its own refusing」は default paths を呼ぶ前提で、code と食い違っていた） | net/tls.c を直した（§1 の 3） |
| Windows の表の file（例 `net/tls-static.c`） | `net/tls-static.c` は既にあり、Windows の build が OpenSSL を static link するための物（`apps/browser-win32/Makefile` が `plan/ws334/tests/openssl-windows.sh` で OpenSSL を cross build） | compat の表は別の file `net/tls-static-compat.c` |
| `apps/browser-win32/Makefile` は WS334 の担当（P7）が直す | Makefile は Claude（担当も）が変えない規則（2026-11-12）。WS334 の担当も直せない | 差分の案 B（§4）をこの Phase が用意 |
| 2 つの `sources.mk` | `userland/base/libcrypto-compat/sources.mk`・`userland/base/libssl-compat/sources.mk` は p039・p033 で main にある | 一致（案 B が include） |
| ca-certificates の置き場（WS334 の決定 18、`share/certs/ca-certificates.pem`） | main のまま。net/tls.c の `tls_add_bundle` がどの library でも読む | 一致（新しい file は足さない案） |
| libbrowser の package の依存に `base/libssl-compat` を足す | package の依存（REQUIRE）は link の `-l:` になる（`standalone_required_libs`）。足すと libbrowser.so が DT_NEEDED で libssl-compat.so を持ち、「無くても program は起動する」形（dlopen）と食い違う。image に入るかは `CONFIG_USERLAND_PROGRAMS`（`mk/platform-*/defaults.mk`）だけで決まり、今の既定に compat は無い（`openssl` はある） | REQUIRE には足さず、既定の一覧に足す差分の案 C（§4） |
| 試験の道具（`run-image.sh`・`run-guest.sh`・`plan/tools/guest/test-image.sh`） | 旧い build（`ZEDBSD_CONFIG`・`BUILD=`）の形のまま。新しい build に試験の file を image に足す口は無い | この Phase は top の Makefile の `disk` と serial で file を渡す新しい script（run-browser-guest.sh）。旧い道具の直しは範囲の外（Q1 へ） |

範囲の外で見つけた事: `userland/desktop/mailer/tls.c` も OpenSSL を名前で開く（net/tls.c の写しに近い）。OpenSSL への依存を無くすなら同じ切り替えが要る（別の Phase の候補、Q1 へ）。

## 3. 実装（branch `agent/p2-ws335c`）

- `userland/desktop/libbrowser/net/tls.c`: `WB_TLS_COMPAT` で開く名前（compat だけ）と失敗の文（`TLS_LIBRARY_NAMES`）を分ける。`tls_refuses_distrusted` と `tls_add_roots` の分岐（§1 の 3）。`tls_remember_distrusted` の NULL の確かめを外した（呼ばれるのは OpenSSL の時だけ）。誤りの文の「OpenSSL」を library を問わない文に（OpenSSL だけの道の文は残す）。file の comment から plan/ への参照を外した（AGENTS.md §10）。
- `userland/desktop/libbrowser/net/tls-library.h`: comment（「OpenSSL only」の意味、tls-static の 2 つの file）。
- `userland/desktop/libbrowser/net/tls-static-compat.c`: 新しい file（§1）。
- 試験: `plan/ws335/tests/run-nettls.sh`（compat の run を `-DWB_TLS_COMPAT` の build で、OpenSSL の名前の symlink 無しに）、`plan/ws335/tests/tls-wine-compat.sh`（新、Windows の compat の static link）、`plan/ws335/tests/run-browser-guest.sh`（新、QEMU）、`plan/ws335/tests/config-p036.mk`・`config-p036-guest.mk`（新しい build の config）。

## 4. Makefile・mk/ の差分の案（ユーザーの確認待ち。Claude は当てない）

- **A. `userland/desktop/libbrowser/Makefile`**（[libbrowser-Makefile.diff](libbrowser-Makefile.diff)、3 行）: `STANDALONE_CFLAGS_libbrowser += -DWB_TLS_COMPAT`。zedBSD の libbrowser.so が compat の名前を開く。
  別の案: `userland/base/package.mk` に package ごとの `<名前>_CPPFLAGS` の口を作る（他の package にも使えるが mk の変更が大きい）。推しは A（1 package の 1 行、`STANDALONE_CFLAGS_<名前>` は package.mk の中の名前に頼る点が弱み）。
  当てた後の確かめ: `make CONFIGMK=plan/ws335/tests/config-p036.mk BUILD_JOBS=4 build/x86_64/packages/libbrowser/.stamp` → `grep WB_TLS_COMPAT build/x86_64/packages/libbrowser/.flags`、`strings build/x86_64/dynamic/libbrowser.so | grep -x 'libssl-compat.so'`。
- **B. `apps/browser-win32/Makefile`**（[browser-win32-Makefile.diff](browser-win32-Makefile.diff)）: 2 つの `sources.mk` の include と static library `libssl-compat.a`・`libcrypto-compat.a`（`WIN32_STATIC_LIBRARY`、warning 0 の flag）、`tls-static.c` を `tls-static-compat.c` に、OpenSSL の archive・`openssl` の target（`tls` に）・`openssl-windows.sh` の規則・OpenSSL の許諾の文の同梱を外す。`-lbcrypt -lcrypt32 -lws2_32` は今の `WIN32_SYSTEM_LDFLAGS` にある。tree の Zlib の許諾は既に `zedBSD-LICENSE.txt`。
  一緒に直す物（Makefile でないが B と同時でないと食い違う。WS334 の file）: `apps/browser-win32/resources/licenses-README.txt` の OpenSSL の 2 行、`plan/ws334/tests/zip-contents.txt` の `share/licenses/OpenSSL-LICENSE.txt`、`plan/ws334/tests/cross-build.sh:116` の OpenSSL の archive、`plan/ws334/tests/tls-wine.sh`（openssldir の確かめは OpenSSL だけの物。`tls-wine-compat.sh` の `COMPAT_BY_MAKEFILE=1` が置き換え）。B の後、`tls-static.c`・`openssl-windows.sh`・`openssl-conf/` は使われない（削除は Q1）。
  当てた後の確かめ: `COMPAT_BY_MAKEFILE=1 sh plan/ws335/tests/tls-wine-compat.sh`、`sh plan/ws334/tests/cross-build.sh`（上の直しの後）、`llvm-objdump -p build/browser-win64/OmniGrammaton.exe` の import に OpenSSL の DLL が無い事。
- **C. `mk/platform-*/defaults.mk`**（[defaults.diff](defaults.diff)）: libbrowser を持つ 7 つの platform の `CONFIG_USERLAND_PROGRAMS` の `libbrowser` の後に `libcrypto-compat`・`libssl-compat`。A と同時に当てないと、image の browser は https ができない（A だけだと compat が image に無い）。`openssl` の package は curl・openssh・mailer が使うので残す（外すかは製品の判断）。
  注意: compat の build が確かめられているのは amd64・aarch64・riscv64（p033・p039）。i386（`i386`・`pc9821`）は未確認なので、その 2 つを外して当てる手もある（推し: 7 つとも当て、i386 の build は当てた後に確かめる）。
  当てた後の確かめ: `make CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 disk` の warning 0、`sh plan/ws335/tests/run-browser-guest.sh`。

## 5. 受け入れ

- zedBSD: WS074 の host の試験（本家と compat の両方）、compat の表の名前が全部 libssl-compat.so・libcrypto-compat.so の export にある事、zedBSD の build の warning 0、QEMU の zedBSD でブラウザが https の頁を開く（画面の PNG）。
- Windows: WS334 の Wine の https の試験の組（a〜f と .exe の隣の cert.pem の g）が compat の static link で PASS。`OmniGrammaton.exe` の import に OpenSSL の DLL が無い事（B の後）。

## 6. 試験のコマンド

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws335/tests/run-nettls.sh` | `run-nettls: PASS (http 15 cases, loader 11 cases, each with OpenSSL and compat)` |
| `sh plan/ws335/tests/tls-wine-compat.sh` | `tls-wine-compat: PASS`（B を写しに当てて build） |
| `make CONFIGMK=plan/ws335/tests/config-p036.mk BUILD_JOBS=4 'STANDALONE_CFLAGS_libbrowser=$(STANDALONE_SHLIB_CFLAGS) -DWB_TLS_COMPAT' build/x86_64/packages/libcrypto-compat/.stamp build/x86_64/packages/libssl-compat/.stamp build/x86_64/packages/libbrowser/.stamp` | warning 0（A を当てた後は最後の変数が要らない） |
| `sh plan/ws335/tests/check-exports.sh build/x86_64/dynamic/libcrypto-compat.so build/x86_64/dynamic/libssl-compat.so` | `check-exports: PASS` |
| `make CONFIGMK=plan/ws335/tests/config-p036-guest.mk CONFIG_GRAPHICAL_LOGIN=n BUILD_JOBS=4 '<A の変数>' disk` → `sh plan/ws335/tests/run-browser-guest.sh` | `run-browser-guest: PASS`、PNG |

## 結果

（記入中）
