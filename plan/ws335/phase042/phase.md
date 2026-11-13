<!-- awesome-plan project=zedbsd record=ws335-p042 -->

# ws335-p042: TLS を libssl-compat・libcrypto-compat を前提に書き換える設計（mailer・libbrowser・browser-win32）と OpenSSL・OpenSSH を外部の package にする

Status: cleared（2026-11-13 Q1 判定。問いの判断は ws.md、makefile.diff はユーザーの承認待ち）。前: 実装の Phase の設計済み、Q1 の判定待ち（2026-11-13 P2 q2051-i01、branch `agent/p2-ws335f`、基点 main 70a3204e59）
Parent: [WS335](../ws.md)

## 目的

2026-11-13 ユーザーから、次の趣旨の決定を受けた（要点、原文ではない）:

1. mailer の TLS は選べる機能ではなく、compat の TLS（libssl-compat・libcrypto-compat）を前提に書き換える。`-DML_TLS_COMPAT` は要らない形にし、source でもその macro を使わない。Makefile もその前提に。
2. libbrowser と browser-win32 も同じく compat の TLS を前提にする。`WB_` で始まる compat の macro（`WB_TLS_COMPAT`）を使う今の実装（名前で開く表・`tls-static-compat.c` の表）を改める。
3. OpenSSL・OpenSSH は、利用者が明示的に指定しない限り build に入らない、完全に外部の package として扱う。計画書だけでなく source tree にも明記する。

（ユーザーは「libtls-compat」と呼んだ。Q1 は、今ある libssl-compat・libcrypto-compat の 2 つ（WS335 の成果）の事と読む。設計の担当も同じに読み、新しい library の名前は作らない: 下の「あるべき形」の 0。）

## 進め方

標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。この Phase は設計の担当（Opus）が行い、実装は Sonnet 5.5 high の Phase（p043 以降）に渡す。
この書き換えは新しい module を作らず、凍結の header（`include/libc/compat/`・`userland/base/lib*-compat/*.h`）も変えない。手順の「header が仕様」の役は、compat の公開の header（OpenSSL 3.5 と同じ signature、comment が約束）が担う。

## 範囲（設計の担当）

1. 今の形を調べる（下の §1 一覧）。
2. あるべき形を決める（§2）。
3. OpenSSL・OpenSSH の扱い（§3、Makefile の先頭の comment の案は makefile.diff に）。
4. Makefile・`mk/` の差分の案（[makefile.diff](makefile.diff)、§4）。
5. 実装の Phase（p043〜p047）の phase.md（§5）。
6. design-reviewer を 1 回（§6）。

## 1. 一覧: 今の macro・名前の表・OpenSSL へ落ちる道（main 70a3204e59）

### 1.1 libbrowser（`userland/desktop/libbrowser/`）

| 場所 | 中身 | 新しい形での扱い |
| --- | --- | --- |
| `net/tls.c:9-45` | file の comment: `WB_TLS_STATIC`（Windows の表）・`WB_TLS_COMPAT`（zedBSD の名前）・印の無い build は OpenSSL を開く、「OpenSSL only」の member の説明 | compat を直に呼ぶ形の説明に書き直す |
| `net/tls.c:62-76` | OpenSSL の定数の写し `TLS_VERIFY_PEER`・`TLS_CTRL_SET_TLSEXT_HOSTNAME`・`TLS_NAMETYPE_HOST_NAME`・`TLS_CTRL_SET_MIN_PROTO_VERSION`・`TLS_VERSION_1_2`・`TLS_OP_IGNORE_UNEXPECTED_EOF`・`TLS_ERROR_*`・`TLS_VERIFY_OK`・`TLS_INIT_NO_LOAD_CONFIG`・`TLS_CERT_REJECTED` | 消す。`<compat/ssl/ssl.h>`・`<compat/crypto/x509.h>` の名前（`SSL_VERIFY_PEER`・`SSL_set_tlsext_host_name`・`SSL_CTX_set_min_proto_version`・`TLS1_2_VERSION`・`SSL_OP_IGNORE_UNEXPECTED_EOF`・`SSL_ERROR_*`・`X509_V_OK`）を使う |
| `net/tls.c:84-88` | `TLS_FINGERPRINT_SIZE`・`TLS_DISTRUSTED_MAX` | 消す（Disallowed は compat が拒む） |
| `net/tls.c:96-101` | `#ifdef WB_TLS_COMPAT` の `TLS_LIBRARY_NAMES`（失敗の文） | 消す |
| `net/tls.c:103-175` | `#ifndef WB_TLS_STATIC`: `struct tls_symbol` と表 `tls_symbols[]`（必須 29 行＋「OpenSSL only」5 行: `OPENSSL_init_ssl`・`X509_digest`・`EVP_sha256`・`X509_STORE_CTX_get_current_cert`・`X509_STORE_CTX_set_error`）、`#ifdef WB_TLS_COMPAT` の名前 `libcrypto-compat.so`・`libssl-compat.so`、**else の OpenSSL の名前 `libcrypto.so`・`libcrypto.so.3`・`libssl.so`・`libssl.so.3`（OpenSSL へ落ちる道 1: 印の無い build。Linux の host の試験が使う）** | 全部消す |
| `net/tls.c:194-226` | 表 `tls_library`（関数 pointer）、Disallowed の指紋の配列 3 つ | 表は消す。指紋の配列も消す |
| `net/tls.c:730-827` | `tls_bind_library`（`WB_TLS_STATIC` なら `tls_static_library`、他は `wb_os_library_open` で開く）、`tls_find`、`tls_find_all`（任意の行は NULL のまま） | 全部消す |
| `net/tls.c:875-882` | `init_ssl`（`OPENSSL_init_ssl(NO_LOAD_CONFIG)`、NULL なら呼ばない） | 消す（compat は設定の file を読まない） |
| `net/tls.c:924-1102` | `tls_refuses_distrusted`（表に OpenSSL の 4 関数があるか）、`tls_add_roots` の **OpenSSL の枝（OpenSSL へ落ちる道 2: `wb_os_trust_roots` の DER を `d2i_X509`・`X509_STORE_add_cert` で入れ、Disallowed の指紋を覚える）**、`tls_add_roots_each`、`tls_remember_distrusted` | `tls_add_roots` は `SSL_CTX_set_default_verify_paths` だけ（失敗は context の失敗、p036 の決定のまま）。他は消す |
| `net/tls.c:1132-1176` | 検証の callback `tls_verify`（Disallowed の指紋の照合） | 消す（`SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL)`） |
| `net/tls-library.h`（92 行） | 表の型 `struct tls_library`・`tls_verify_callback`・`tls_static_library` | file ごと消す（削除は Q1） |
| `net/tls-static.c`（449 行） | OpenSSL を static link した表（Windows の旧い build、p036 の差分 B の後は使われない） | file ごと消す（Q1） |
| `net/tls-static-compat.c`（400 行） | compat を static link した表（今の Windows の build） | file ごと消す（Q1） |
| `net/net.h:188` | comment「the OpenSSL package's library, loaded when first needed」 | 直す |
| `net/http.c:139-141` | comment「EPROTONOSUPPORT … without the OpenSSL package」 | 直す |
| `base/os-net.h:169-180` | `wb_os_socket_descriptor`・`wb_os_socket_last_error` の comment の「OpenSSL」 | 「the TLS library」に（関数は残す。tls.c が使う） |
| `base/os-system.h:155-172`・`os-system-posix.c:336`・`os-system-win32.c:402` | `wb_os_trust_roots`（OS の根を DER で列挙。使うのは tls.c の OpenSSL の枝だけ） | 使う所が無くなる。消すかは問い Q4（推し: p045 で消す） |
| `Makefile:16-18` | `STANDALONE_CFLAGS_libbrowser += -DWB_TLS_COMPAT` | 差分 A |
| `apps/browser-win32/Makefile:88・299` | `tls-static-compat.c` を core に足す行、`tls.o` に `-DWB_TLS_STATIC` | 差分 C |

`wb_os_library_open`・`wb_os_library_symbol` は `paint/vulkan.c:2038` も使うので残す。`wb_os_data_directory`（`tls_add_bundle`、`text/font.c`）・`wb_os_socket_descriptor`（`SSL_set_fd`）は残す。

### 1.2 mailer（`userland/desktop/mailer/`）

| 場所 | 中身 | 新しい形での扱い |
| --- | --- | --- |
| `tls.c:8-35` | file の comment（dlopen、`ML_TLS_COMPAT`、印の無い build は OpenSSL） | 書き直す |
| `tls.c:40` | `#include <dlfcn.h>` | 消す |
| `tls.c:49-53` | `#ifdef ML_TLS_COMPAT` の `TLS_LIBRARY_NAMES` | 消す |
| `tls.c:56-67` | OpenSSL の定数の写し | compat の名前に |
| `tls.c:75-107` | 表 `struct tls_library`（27 member） | 消す |
| `tls.c:109-165` | `struct tls_symbol`、名前の表（`ML_TLS_COMPAT` で compat、**else OpenSSL `libcrypto.so`・`.so.3`・`libssl.so`・`.so.3`: OpenSSL へ落ちる道 3**）、`tls_symbols[]` 26 行 | 消す |
| `tls.c:442-571` | `tls_load_locked` の dlopen、`tls_open_library`（`dlopen`・`dlerror`）、`tls_find_all`（`dlsym`） | `tls_load_locked` は context を作るだけに。他は消す |
| `tls.c:694-716` | `tls_find_peer_certificate`（`SSL_get1_peer_certificate`、無ければ **OpenSSL 1.1 の `SSL_get_peer_certificate`: OpenSSL へ落ちる道 4**） | 消す（`SSL_get1_peer_certificate` を直に） |
| `tls.c:677-686` | `tls_verify_later(int, void *)` | `SSL_verify_cb` の型 `(int, X509_STORE_CTX *)` に（関数 pointer の型の変換を無くす） |
| `mail.h:9-11` | file の comment「plain or TLS (the OpenSSL package loaded when first needed)」 | 直す |
| `mail.h:190` | comment「The TLS of the OpenSSL package」 | 直す |
| `sync.c:993・1032・1042・1048` | comment「OpenSSL's words」 | 「the TLS library's words」に |
| `sync.c:1076-1077` | `EPROTONOSUPPORT` の文「TLS is not available (the OpenSSL package is not installed)」 | 「TLS cannot be set up」に（§2 の 4） |
| `imap.c:159`・`smtp.c:84` | comment「OpenSSL's words」 | 直す |
| `Makefile` | `ML_TLS_COMPAT` は付いていない（p036 の差分 D は当たっていない） | 差分 B |

**今の main の欠陥（見つけた事）**: mailer の Makefile に `-DML_TLS_COMPAT` が無いので、**今の zedBSD の mailer は OpenSSL の名前（`libssl.so`）を開く**。既定の一覧（`mk/platform-*/defaults.mk`）に `openssl` は無いので、zedBSD の既定の image の Mail の TLS は EPROTONOSUPPORT で全部失敗するはず（未観測、code と Makefile を読んだ結論。p041 の guest の試験は command line で `-DML_TLS_COMPAT` を与えていた）。この設計の p044 と差分 B で直る。

### 1.3 試験の道具（OpenSSL を開く・名前の表に頼る所）

| file（WS） | 頼っている物 | 新しい形での扱い |
| --- | --- | --- |
| `plan/ws074/tests/host-build.sh`（WS074。WS074・WS334・WS345・WS346・`plan/tools/browser-component` の host の試験の全部が使う） | libbrowser の host の build。tls.c を印無しで compile（OpenSSL を開く）、libbrowser.so を `-z defs` で link | compat の sources を `obj/` に compile して `$engine` に入れる（image の compat の library と同じ形）。p043 |
| `plan/ws334/tests/net-ring-test.sh`（WS334） | `net/*.c` を host で compile（tls-static の 2 つを除く）、`-ldl` | compat の .a を link（新しい道具 `plan/tools/tls/host-compat-libs.sh`）。p043 |
| `plan/ws335/tests/run-nettls.sh` | OpenSSL と `-DWB_TLS_COMPAT` の 2 build、名前の symlink 無し、ld.so の log で開いた library を照合 | compat だけの 1 build（host-build.sh の出力）に。件数 http 15・loader 11 と、libbrowser.so に OpenSSL の DT_NEEDED・未定義の `SSL_` の名前が無い事。p043 |
| `plan/ws335/tests/run-mailer-tls.sh`・`mailer-tls.c` | OpenSSL と `-DML_TLS_COMPAT` の 2 build、`LD_LIBRARY_PATH`、「library 無しで EPROTONOSUPPORT」の case | compat の 1 build（.a を link）、8 case。「library 無し」の case は無くなる。p044 |
| `plan/tools/mail/run-host-mail-backend.sh`（tools.md の登録の道具） | mailer の tls.c を host の OpenSSL で（`-ldl`） | compat の .a を link。p044 |
| `plan/ws335/tests/run-mailer-guest.sh` | image の mailer の `.flags` に `-DML_TLS_COMPAT`、guest の program を `-l:libkeiland.so` だけで link | `.flags` の確かめを「`/bin/mailer` の NEEDED に libssl-compat.so」に、guest の program に `-l:libssl-compat.so -l:libcrypto-compat.so`、command line の `STANDALONE_CFLAGS_mailer` を外す（差分の後）。p046 |
| `plan/ws335/tests/run-browser-guest.sh` | 「libbrowser.so が開く名前」を strings で | 「libbrowser.so の NEEDED」に。p046 |
| `plan/ws334/tests/tls-wine.sh`（WS334） | `tls-static-compat.o` が build にある事 | 「tls.o が compat の名前を直に呼ぶ（`llvm-nm` で `SSL_connect` が U）」に。p045 |
| `plan/ws335/tests/tls-wine-compat.sh`・`config-p036.mk` | p036 の差分 B の前の写し・旧い config | 消す候補（p036 の記録のとおり。p046 で一覧に） |

## 2. あるべき形

0. **library**: 今の libssl-compat・libcrypto-compat（公開の header は `include/libc/compat/ssl/ssl.h`・`include/libc/compat/crypto/*.h`、OpenSSL 3.5 の signature）。新しい library・header・exports は作らない。凍結の header は変えない（使う関数は全部、今の exports.map にある: §2 の 6）。
1. **source**: `net/tls.c` と mailer の `tls.c` は `<compat/ssl/ssl.h>`（と要る時 `<compat/crypto/evp.h>`）を include し、`SSL_*`・`X509_*`・`ERR_*`・`EVP_sha256` を直に呼ぶ。関数 pointer の表・名前の表・`dlopen`・`WB_TLS_COMPAT`・`WB_TLS_STATIC`・`ML_TLS_COMPAT` は無い。OpenSSL の header（`<openssl/...>`）も、`openssl-names/` の転送の header も使わない（OpenSSL と切り替える口は作らない: ユーザーの決定 1・2）。
2. **zedBSD**: libbrowser.so と `/bin/mailer` は `libssl-compat.so`・`libcrypto-compat.so` を link する（DT_NEEDED）。package の requirement（`USERLAND_PACKAGE` の 10 番目の引数）に `base/libssl-compat base/libcrypto-compat`、`mk/userland.mk` の stamp の順に 2 つの stamp（差分 A・B・D）。image に入るかは今までどおり `CONFIG_USERLAND_PROGRAMS` で決まり、6 つの platform の既定の一覧には既に 2 つがある（p036 の差分 C）。
   帰結: libbrowser・mailer を build すれば、requirement と stamp の順で 2 つの .so も必ず `build/<platform>/dynamic/` に作られ、`mk/rootfs.mk:163` は `dynamic/*` を選択に依らず全部 `/lib` に写すので、今の build では image から欠けない。欠けるのは rootfs の写し方が選択に従う形に変わった時だけ（問い Q6）。
3. **Windows**: 今と同じ `libssl-compat.a`・`libcrypto-compat.a` を OmniGrammaton.exe に static link（`WIN32_TLS_ARCHIVES`、D23）。tls.c は zedBSD と同じ source（`#ifdef` 無し）、`tls-static-compat.c` と `-DWB_TLS_STATIC` を外す（差分 C）。archive の順（tree → compat → TLS）は今のままで、libbrowser の参照は後ろの libssl-compat.a・libcrypto-compat.a で解ける。compat の header は今も `$(BROWSER_WIN32_OBJ)/include/compat/` に写され、core の object はその写しを order-only で待つ（tls-static-compat.c が今それで compile できている）。
4. **残す振る舞い**（両方）: TLS 1.2 以上、`SSL_VERIFY_PEER`、`SSL_OP_IGNORE_UNEXPECTED_EOF`、SNI と名前・address の照合（libbrowser は IP の時 `X509_VERIFY_PARAM_set1_ip_asc` で SNI 無し、mailer は名前だけ）、試験の CA の file（`net_tls_add_ca_file`・`ml_tls_add_ca_file`、4 つまで）、誤りの文（`<what>: certificate verify failed: <X509_verify_cert_error_string>`、`<what>: <ERR_error_string_n>`、`<what>: failed (TLS error N)`、mailer は `(SSL error N)`）、errno の約束（成功 0、`ENOMEM`、`EPROTO`、mailer の `ML_ERROR_UNTRUSTED`、context が作れない時の `EPROTONOSUPPORT`）、thread（1 度だけ lock の下で context を作り、失敗も覚える）、`SSL_read`・`SSL_write` に渡す長さを `INT_MAX` で切る事。
   - libbrowser: 根は `SSL_CTX_set_default_verify_paths`（zedBSD は `/etc/ssl/cert.pem`、Windows は ROOT から Disallowed を除いた物と .exe の隣の `cert.pem`、Disallowed は鎖のどこでも拒む: compat の約束）、**失敗は context の失敗**（p036 の決定）。続けて program の束（`<data>/certs/ca-certificates.pem`、無ければ黙る）、試験の CA の file。
   - mailer: 自己署名の pin（handshake は検証の結果に依らず最後まで（`tls_verify_later` が 1 を返す）、`SSL_get_verify_result` が `X509_V_OK` でなければ `SSL_get1_peer_certificate` と `X509_digest(EVP_sha256())` の 64 桁の小文字の hex を `ml_tls_fingerprint` に、pin と同じなら通す）。default verify paths の失敗は黙る（今のまま）。
   - `EPROTONOSUPPORT` の意味: 「この process では TLS を始められない（context が作れない）」。library が無い事はもう起きない（link の時に解ける）。文は理由の buffer に入れた物（「cannot make a TLS context」など）。sync.c の文は「TLS cannot be set up」に。
5. **host（Linux）の試験**: host の OpenSSL を使わない。compat を host の cc で build して link する。
   - libbrowser: `plan/ws074/tests/host-build.sh` が compat の sources（`sources.mk` の common・POSIX・ssl）を `$out/obj/libcrypto-compat-*.o`・`libssl-compat-*.o` に compile して `$engine` に入れる（image の compat の library と同じ形。`obj/*.o` を拾う WS345 の試験はそのまま動く）。
   - host の system の根: compat の POSIX の default paths は `/etc/ssl/cert.pem` だけ（`api-ssl.c`）。Debian の host には無く（`/etc/ssl/certs/ca-certificates.crt` だけ）、host の browser・mailer の道具は公開の site を検証できなくなる（今は host の OpenSSL が Debian の根を読む）。local の試験の CA を使う試験（http 15・loader 11、mailer の 8 case）には効かない。扱いは問い Q9。
   - 他（mailer の試験・net-ring-test）: 新しい共有の道具 `plan/tools/tls/host-compat-libs.sh <outdir>` が `<outdir>/libcrypto-compat.a`・`libssl-compat.a` と `<outdir>/include/compat`（`include/libc/compat` への symlink）を作る。使う側は `-I<outdir>/include` と `<outdir>/libssl-compat.a <outdir>/libcrypto-compat.a -lpthread`。`-Iinclude/libc` は host の libc の header を隠すので使わない。
6. **使う関数が compat にある事**（exports.map、p041 の後）: libssl-compat の `TLS_client_method`・`SSL_CTX_new`・`SSL_CTX_set_verify`・`SSL_CTX_load_verify_locations`・`SSL_CTX_set_default_verify_paths`・`SSL_CTX_set_options`・`SSL_CTX_ctrl`・`SSL_new`・`SSL_free`・`SSL_set_fd`・`SSL_ctrl`・`SSL_set1_host`・`SSL_get0_param`・`SSL_connect`・`SSL_read`・`SSL_write`・`SSL_get_error`・`SSL_pending`・`SSL_shutdown`・`SSL_get_verify_result`・`SSL_get1_peer_certificate`、libcrypto-compat の `X509_VERIFY_PARAM_set1_ip_asc`・`X509_verify_cert_error_string`・`ERR_get_error`・`ERR_error_string_n`・`ERR_clear_error`・`X509_digest`・`EVP_sha256`・`X509_free`。`SSL_CTX_get_cert_store`・`d2i_X509`・`X509_STORE_add_cert` は使わなくなる（compat に残る）。

## 3. OpenSSL・OpenSSH を外部の package に

- **今の事実**: 2 つとも `USERLAND_PACKAGE` の既定は `n`、6 つの platform の既定の一覧（`mk/platform-*/defaults.mk`）に無い。build に入るのは `CONFIG_USERLAND_PROGRAMS` で名指した時だけ（`openssh`・`curl`・`python3` の requirement に `security/openssl`）。source は tree に無く、tarball を取得・照合して patch する（`userland/packages/external.mk`）。
- **source tree に書く案**（makefile.diff の E・F。Makefile の先頭の comment）: 「外部の package。設定が package openssl を名指した時だけ build され image に入る（curl・python3・openssh はそれを要るので、それらを名指す設定は openssl も名指す。requirement から自動で作る規則は無い: `standalone_required_so` は `security/openssl` に .so を返さない）。system の何もこれを要らない。system の TLS と暗号は libssl-compat・libcrypto-compat、ssh は `userland/base/ssh`」。今の comment の「Provenance … plan/ws032/provenance.md」（source の comment に plan/ への参照、AGENTS.md §10）と「Registration as a selectable package … land with the build work」（済んで古い）を外す。
  併せて libssl-compat・libcrypto-compat の Makefile の「Off by default: a program that links it selects it」を「libbrowser・mailer（libcrypto-compat は ssh・fidoctl・passkey-fido2 も）が link する。それを作る build は先に 2 つを作り、image は build が作った shared library を全部取る」に（G）。openssl の Makefile の中ほどの `See plan/ws032/phase006/results.md` も外す（E）。
- **見つけた事（問いへ）**:
  - `config/ci/*.mk`（ユーザーの `config-amd64-server.mk` を含む 9 file）の `ZEDBSD_USER_PROGRAMS` に `openssl`・`openssh`・`curl` がある（amd64・amd64-server・odroid-c5 の 2 つ・rpi4・rpi4-server・rpi5・visionfive2-keiland・zybo。zybo は curl 無し）。ただしこれは build の作り直し（2026-11-06）の前の変数で、今の build（`CONFIG_USERLAND_PROGRAMS`）は読まない（`.github/workflows/ci.yml` も旧い出力の path `build/amd64/hdd-image.img` と旧い target `firmware-licenses` を使う）。今は選ばれていないので食い違いは起きていないが、今の形に直す時に外すかが問い Q1。libssl-compat・libcrypto-compat も CI の一覧には無い。
  - `docs/reference/installer.md:140-141`（と `docs/howto/boot-and-storage.md:256`）は installer の既定で OpenSSH・「OpenSSL and CA certificates」を on とする。ユーザーの決定 3 と食い違う（問い Q2）。
  - openssl・openssh・curl・python3 の Makefile の `PACKAGE_FILES` は旧い変数 `USER_PROGRAMS`（`external.mk:39`、`USERLAND_PROGRAMS` から）で選ばれる。ca-certificates で直った BUG-418 C と同じ形で、今の build で名指しても image に入らない疑いがある（未確認、範囲の外）。

## 4. Makefile・`mk/` の差分の案（[makefile.diff](makefile.diff)、`git apply --check` が基点の tree に通る）

| 記号 | file | 変更 |
| --- | --- | --- |
| A | `userland/desktop/libbrowser/Makefile` | requirement に `base/libssl-compat base/libcrypto-compat`（libbrowser.so が `-l:libssl-compat.so -l:libcrypto-compat.so` で link）。`STANDALONE_CFLAGS_libbrowser += -DWB_TLS_COMPAT` と comment 2 行を外し、comment 2 行（直に呼び link する、OpenSSL は使わない） |
| B | `userland/desktop/mailer/Makefile` | requirement に同じ 2 つ、comment 2 行（p036 の差分 D は要らなくなる） |
| C | `apps/browser-win32/Makefile` | `LIBBROWSER_CORE_SOURCES += …/net/tls-static-compat.c` と `…/net/tls.o: WIN32_CFLAGS += -DWB_TLS_STATIC` の 2 行を外す |
| D | `mk/userland.mk` | libbrowser と mailer の stamp に libssl-compat・libcrypto-compat の stamp（top の並行の build で .so が先にできる。ssh・fidoctl の行と同じ形） |
| E | `userland/packages/security/openssl/Makefile` | 先頭の comment（§3）と中ほどの plan/ への参照の 1 行 |
| F | `userland/packages/network/openssh/Makefile` | 先頭の comment（§3） |
| G | `userland/base/libssl-compat/Makefile`・`userland/base/libcrypto-compat/Makefile` | 「Off by default」の comment（§3） |

- **当てる時期**（問い Q7）: ユーザーの承認の後、Q1 が **A・B・D・E・F・G を p043 の merge と同じ commit（か merge の直前）に**、**C を p045 の merge と同じ commit に**当てる。merge と差分を別の commit にして間の main を残さない。
  理由: A の requirement が無いまま p043 の tls.c が入ると、libbrowser.so に DT_NEEDED が無く、`-z now` の browser は起動で落ちる（package.mk は link の時に無い .so を黙って外し、shared library の link は `-z defs` ではない）。逆に A を先に当てて `-DWB_TLS_COMPAT` を外すと、今の tls.c は OpenSSL の名前を開いて zedBSD の https が止まる。
  C は急がない: p043 の後の tls.c は `WB_TLS_STATIC` を見ないので、C の前も Windows の build は `tls-static-compat.o` を作って参照しないだけで動く。C を p043 と同時に当てると、`plan/ws334/tests/tls-wine.sh` の「tls-static-compat.o がある」の確かめが p045 まで main で FAIL するので、p045 の tls-wine.sh の直しと同時にする。B・D・E・F・G は単独でも害は無い。
- **当てる前の確かめ（実装の Phase）**: zedBSD は make の command line で同じ値を与える（p041 と同じやり方）: `'STANDALONE_CFLAGS_libbrowser=$(STANDALONE_SHLIB_CFLAGS)'`、`'STANDALONE_SO_LIST_libbrowser=libtruetype.so libvulkan.so libmedia.so libjpeg-compat.so libpng-compat.so libz-compat.so libgif-compat.so libwebp-compat.so libssl-compat.so libcrypto-compat.so'`、`'STANDALONE_SO_LIST_mailer=libvulkan.so libwayland-client.so libkeiland.so libtruetype.so libssl-compat.so libcrypto-compat.so'`（package.mk の `standalone_required_so` を当てた値。link の recipe は `$$(STANDALONE_SO_LIST_<名前>)` を後で展開し、command line の変数は入れ子の make にも渡るので効く。compat の 2 つの stamp は前の make で先に作る。`readelf -d` の NEEDED で効いた事を確かめる）。Windows は `apps/browser-win32/Makefile` の写しに C を当てて `make -f <写し>`（p036 と同じ。`git apply` の `--include` は `--directory` を付けた後の path と照合される）。
- zedBSD の package の compile の flag には `-Wall` が無い（`package.mk:143-165`）ので「warning 0」は弱い確かめ。型の誤りは host の build（`-Wall -Wextra -Werror`）が捕まえる。
- **当てた後の確かめ**: p046 が素の main で（command line の上書き無し）。
- C を当てた後に Q1 に消してもらう物: `userland/desktop/libbrowser/net/tls-library.h`・`tls-static.c`・`tls-static-compat.c`（どこからも compile されない。p043 の後の tls.c は include しない）、`plan/ws334/tests/openssl-windows.sh`・`plan/ws334/tests/openssl-conf/`（p036 の記録のとおり既に使われない）、`plan/ws335/phase036/mailer-Makefile.diff`（B が置き換える）。

## 5. 実装の Phase

| ID | 目的 | 定義（model） | 依存 | 主な file |
| --- | --- | --- | --- | --- |
| [p043](../phase043/phase.md) | libbrowser の net/tls.c を compat を直に呼ぶ形に。host の道具（`plan/tools/tls/host-compat-libs.sh` 新、host-build.sh、net-ring-test.sh、run-nettls.sh）。zedBSD の build（command line の上書き）と、Windows の core の compile（写しの Makefile） | phase-runner-low（Sonnet 5.5 high） | p042（Q1 の判定・問いの答え・makefile.diff の承認） | `net/tls.c`・`net/net.h`・`net/http.c`・`base/os-net.h` |
| [p044](../phase044/phase.md) | mailer の tls.c を compat を直に呼ぶ形に。mail.h・sync.c・imap.c・smtp.c の文。host の試験（run-mailer-tls.sh・mailer-tls.c・plan/tools/mail/run-host-mail-backend.sh）。zedBSD の build（上書きか当てた後の Makefile） | phase-runner-low | p043 の merge（host-compat-libs.sh） | `mailer/tls.c` ほか |
| [p045](../phase045/phase.md) | Windows: OmniGrammaton と試験の program の build（差分 C の写し）、tls-wine.sh、cross-build.sh。Q4 が A なら `wb_os_trust_roots` を消す。merge と同時に Q1 が C を当てる | phase-runner-low | p043 の merge | `plan/ws334/tests/tls-wine.sh`、`base/os-system*` |
| [p046](../phase046/phase.md) | 統合（Q1 が makefile.diff を当て、tls-static などを消した後の素の main）: x86_64 の image で browser と mailer の guest の試験、aarch64・riscv64 の build と NEEDED、tree に macro・OpenSSL の名前が残っていない事、design.md の直し、消す試験の一覧 | phase-runner（Opus） | p043・p044・p045 の merge | `run-browser-guest.sh`・`run-mailer-guest.sh`、design.md |
| [p047](../phase047/phase.md) | 規約の全文の見直し（p043〜p045 が変えた C） | conformance-reviewer | p046 | 変えた .c・.h |

- 並べてよい物: p044 と p045 は p043 の merge の後に並べてよい（変える file が重ならない）。ws.md の表の行は Q1 が merge で解く。
- WS335 の完了は p047 の後（ユーザーの決定: p041 の後の完了はこの後）。

## 6. design-reviewer

2026-11-13 に 1 回起動した（重大 1・中 8・軽 8）。指摘と扱いは [design-review.md](design-review.md) の「p042 の design-reviewer」の節。全部を反映した。

## 7. Q1 への問い（推しを付けて）

| # | 問い | 選択肢 | 推し |
| --- | --- | --- | --- |
| Q1 | `config/ci/*.mk`（`config-amd64-server.mk` はユーザーの物）の `openssl`・`openssh`・`curl`。今の build は `ZEDBSD_USER_PROGRAMS` を読まないので効いていない（ci.yml も旧い path・target） | A: 今は触らない。CI の config を今の build の形に直す時（別の WS）に、OpenSSL・OpenSSH・curl を外し、libcrypto-compat・libssl-compat を足す。B: 今 9 file から 3 つの名前を外す（効かない変数の手入れ、ユーザーの file を含む） | A（効いていない行を今直しても意味が無く、ユーザーの file に触る。CI が今の build で動かない形である事はユーザーに伝える） |
| Q2 | installer の既定（docs/reference/installer.md の表、howto の文）で OpenSSH・「OpenSSL and CA certificates」が on | A: OpenSSH・OpenSSL を off に、CA certificates を別の item で on（docs を直す。installer の実装は今の build に無い）。B: 今のまま | A（ユーザーの決定 3。CA certificates は compat の default paths が読む） |
| Q3 | curl・python3 は OpenSSL を要る（requirement `security/openssl`） | A: そのまま。curl・python3 を選ぶ事を「明示の指定」とみなし、OpenSSL の Makefile の comment にそう書く（案 E のとおり）。B: curl・python3 を compat に移す（別の WS。curl は libssl の広い API を使う） | A |
| Q4 | `wb_os_trust_roots`（`base/os-system.h`、POSIX と Windows の実装、WS334 の test-os-system の case）は書き換えの後どこからも使われない | A: p045 で消す（Windows の ROOT・Disallowed の列挙は compat の cryc-os-win32 が持つ）。B: 残す | A（使われない OS の層の code と試験を残さない） |
| Q5 | 他の WS の file を WS335 の Phase が直す事: `plan/ws074/tests/host-build.sh`（WS074）、`plan/ws334/tests/net-ring-test.sh`・`tls-wine.sh`・`unit/test-os-system.c`（WS334）、`plan/tools/mail/run-host-mail-backend.sh`（登録の道具）、新しい `plan/tools/tls/host-compat-libs.sh`（tools.md への登録は Q1）。記録の写し `plan/ws074/design.md:270-275`（D2: OpenSSL を dlopen）・`plan/ws334/design.md:317・726`（`wb_os_trust_roots`、S-A）の注 | A: 試験の道具は p043〜p045、記録の写しの注は p046 の範囲に入れる（道具を直さないと host の build が `-z defs` で落ちる）。B: 各 WS に頼む | A |
| Q6 | link にしたので browser・mailer は 2 つの .so が無いと起動しない。今の build では requirement と stamp の順で .so が作られ、`mk/rootfs.mk:163` が `dynamic/*` を全部 `/lib` に写すので欠けない。rootfs が選択に従う形に変われば欠けうる | A: 受け入れる（ssh・fidoctl と同じ。既定の一覧にも 2 つはある）。B: mk に「選んだ package の requirement を一覧に足す」規則を作る（mk の変更、全 package に効く） | A |
| Q7 | makefile.diff を当てる時期 | A: A・B・D・E・F・G を p043 の merge と同じ commit に、C を p045 の merge と同じ commit に（`git apply --include=<path>` で分ける）。B: 全部を p043 の merge の時（p045 まで main の tls-wine.sh が FAIL） | A（どの時点の main も build と試験が通る） |
| Q9 | host（Linux）の試験の道具は compat を link するので、system の根は `/etc/ssl/cert.pem` だけになり、Debian の host（`/etc/ssl/certs/ca-certificates.crt`）の根を読まない。local の試験の CA の試験には効かない | A: 受け入れ、host で公開の site を見る時は `--ca-file /etc/ssl/certs/ca-certificates.crt` を渡すと道具の comment に書く。B: compat の POSIX の default paths に Debian の path を足す（凍結の header の約束の変更、zedBSD には無い path） | A |
| Q8 | 見つけた事（範囲の外）: (1) 今の zedBSD の mailer は `-DML_TLS_COMPAT` が無く OpenSSL の名前を開くので、既定の image で Mail の TLS は全部失敗するはず（p044 と差分 B で直る）。(2) openssl・openssh・curl・python3 の `PACKAGE_FILES` が旧い `USER_PROGRAMS` で選ばれる（BUG-418 C と同じ形の疑い、未確認）。(3) `plan/ws074/tests/guest-build.sh` は旧い build の形で engine の object を link する（書き換えの後は compat も要る。既に動かない道具なので消す候補）。(4) `plan/tools/guest/config-amd64-ssh.mk` ほか `plan/tools/` の旧い config にも `openssl` がある（今の build は読まない） | (1) は p044 で直す。(2) は Bug の候補 | (2) を Bug Board に（担当は書かない） |

## 結果（2026-11-13 P2 q2051-i01）

- 置いた物: この phase.md、[makefile.diff](makefile.diff)、[design-review.md](design-review.md) の「p042」の節、[p043](../phase043/phase.md)〜[p047](../phase047/phase.md) の phase.md、[ws.md](../ws.md) の表の行、[design.md](../design.md) §4.2 の注。
- 確かめた事: `git apply --check plan/ws335/phase042/makefile.diff`（基点の tree に通る）。一覧の行番号は基点の tree で読み、design-reviewer の指摘で mailer の行を直した。
- source（.c・.h）・Makefile・`mk/` は変えていない。
- 未実施: build・試験（この Phase は設計だけ）。command line の上書きの値（§4）は package.mk を読んで作った物で、make で流してはいない（p043・p044 の最初の確かめ）。
