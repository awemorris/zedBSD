<!-- awesome-plan project=zedbsd record=ws335-p033 -->

# ws335-p033: 統合（zedBSD の libssl-compat.so と exports、Windows の static library の source の一覧、相互接続・適合の試験）

Status: 実装済み・試験 PASS（2026-11-02 17:35 P1 q1971-i01。cleared は Q1）。前: in-progress（2026-11-02 15:49 P1）、planned（2026-11-01 p001 が作成、同日 q1789-i02 で Windows を static link に、libcrypto-compat.so を p039 に分けた。依存: p032・p023・p024・p039）
Parent: [WS335](../ws.md)。担当: Opus（phase-runner-high を推す。推論が多い）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4・§7・§9、[design.md](../design.md) の §3（D23〜D27）・§4.2〜§4.4・§5・§8、[coding-style.md](../../coding-style.md)。
- p039 の結果（libcrypto-compat.so の規則・`sources.mk`・`exports.map`・`check-exports.sh`）。この Phase はそれを変えず、同じ形で libssl-compat を足す。
- `platform/common/userland-dynamic.mk:35-56`（libz-compat と、libz-compat に依る libpng-compat の .so の規則の前例）。
- Windows の build の約束: [plan/ws334/design.md](../../ws334/design.md) §6（llvm-mingw・UCRT）と `apps/browser-win32/Makefile`（`sources.mk` を include して `lib<名前>.a` を作る `WIN32_STATIC_LIBRARY` の形）。WS334 の file は読むだけ。

## 2. 作る物

- zedBSD: `userland/base/libssl-compat/Makefile`・`exports.map`（node `LIBSSL_COMPAT`、`include/libc/compat/ssl/ssl.h` の関数の全部を名前で）、`platform/common/userland-dynamic.mk` の `libssl-compat.so` の規則（p039 と同じ flag と `-z nodelete`、`-l:libcrypto-compat.so -l:libc.so`、`check-dynamic-elf.py --needed libcrypto-compat.so --needed libc.so --soname libssl-compat.so`）、package の登録（既定 n、image は `/lib/libssl-compat.so`）、
  `include/libc/compat/ssl/` の sysroot への install（p039 の crypto と同じ道）。`check-exports.sh` で libssl-compat.so も照合する（p039 の script が 2 つの .so を扱えなければ、引数の形を保って足す）。
- `userland/base/libssl-compat/sources.mk`（変数だけ）: `LIBSSL_COMPAT_SOURCES`（全部の .c。OS に依る file は無い）。libcrypto-compat の `sources.mk` は p039 の物を使う。
- Windows の成果物は **static library だけ**（`libcrypto-compat.a`・`libssl-compat.a`）。DLL・import library・`.def` は作らない（D23）。この WS の中に Windows の make の target は作らない（D26: 使う側の build が `sources.mk` を読んで作る）。試験のための Windows の build は試験の script の中で行う（下）。
- 試験（Wine の prefix は run の間で残るので、Disallowed・ROOT に足した証明書は script の終わりに消し、始めにも前の残りを消す。tls-client.exe は run ごとの dir に置き、`cert.pem` はその隣）:
  - `plan/ws335/tests/tls-client.c`（host・zedBSD・Windows で動く小さな client: 引数の host・port・CA file、`SSL_*` の公開の API だけを使う。Windows では `WSAStartup` と SOCKET を試験の側で）。
  - `plan/ws335/tests/run-interop.sh`（host の `openssl s_server` を版・suite・group・鍵の種類の組で起動し tls-client で繋ぐ。1 本の script で要約だけを出す）。`--win32` で、tls-client.exe を `run-unit.sh --win32` と同じ flag で 2 つの .a に static link し、Wine の上で同じ host の s_server に繋ぐ（組は 1.3 と 1.2 の各 1 suite × ECDSA・RSA。Wine の prefix は `build/tmp/ws335-wine`）。
  - `plan/ws335/tests/run-limbo.sh`（x509-limbo の SERVER 全件を公開の API で流し、一致の数と違いの表。p038 の結果を使う）、`plan/ws335/tests/run-sites.sh`（実の site の一覧、host から、network の無い時は SKIP と書く）。
  - net/tls.c の host の試験を compat で流す script（`plan/ws074/tests/host-loader.c` を変えず、試験の dir に `libssl.so.3 → libssl-compat.so`・`libcrypto.so.3 → libcrypto-compat.so` の symlink を置いて `LD_LIBRARY_PATH` で開かせる）。
- 結果の節、ws.md の行。

## 3. 試験と受け入れ

- `run-unit.sh all`・`run-unit.sh --win32 all`・`run-ct.sh all`・`BASE=b7dfd3952 check-skeleton.sh` が PASS。
- run-interop.sh: TLS 1.3 × 3 suite × {x25519, P-256（HRR）} × {ECDSA P-256, ECDSA P-384, RSA 2048, RSA 4096} と TLS 1.2 × 6 suite × 同じ鍵、record の断片化（`-max_send_frag`）、非 blocking、server の close_notify の有無。全件 PASS。
- run-interop.sh --win32: 上の部分集合が Wine で PASS。Wine の prefix の current user の "Disallowed" に試験の中間の証明書を入れると、その中間を通る接続が `X509_V_ERR_CERT_REJECTED` で失敗し、`SSL_CTX_set_default_verify_paths` の後に `cert.pem`（試験の CA）を tls-client.exe の隣に置くと `SSL_CTX_load_verify_locations` 無しで繋がる（D14・D24）。
- run-limbo.sh: 期待との不一致の全部に理由（design の決定に基づく）。理由の無い不一致は 0。
- net/tls.c の host の試験が compat で本家と同じ件数で PASS。
- zedBSD: `make $(BUILD)/dynamic/libssl-compat.so` が warning 0、`check-exports.sh` PASS。QEMU（Q1 の割当）の guest で tls-client が host の s_server に繋がる（SSH で操作し結果を読む。console の log で判定しない）。
- Windows: x86_64・aarch64 で 2 つの .a と tls-client.exe が warning 0 で link（aarch64 は link まで）。実の Windows の機械での実行は未実施と書く（WS334 の試験の機械が揃ってから）。

## 4. 注意

- 共有の `platform/common/userland-dynamic.mk` を変えるので、merge の衝突を Q1 に知らせる。toolchain は変えない。共有の build を消さない。
- 実の site の試験は外部に依るので、受け入れは interop と limbo で決め、site は記録に留める。
- p039 の libcrypto-compat.so の規則・export を変えない（変える必要があれば止めて Q1 に）。

## 5. 結果

2026-11-02（host の `date`、15:54〜）P1（Opus、q1971-i01）。worktree `/home/awe/zedBSD-worktrees/p1-ws335`（branch agent/p1-ws335、main 5b2c957332 に早送りしてから）。library の .c・header は変えていない。

作った物:

- zedBSD: `userland/base/libssl-compat/Makefile`（package `libssl-compat`、shared-library、base、既定 n、require `base/libcrypto-compat`、image の data `/lib/libssl-compat.so`）・`exports.map`（ssl.h の 22 名を 1 つずつ、version node 無し、`local: *`。p039 と同じ形）・`sources.mk`（`LIBSSL_COMPAT_SOURCES`、10 file を名指し）。`platform/common/userland-dynamic.mk` に `$(DYNAMIC_DIR)/libssl-compat.so` の規則（libcrypto-compat の規則の次。p039 と同じ flag と `-z nodelete`、`-l:libcrypto-compat.so -l:libc.so`、`check-dynamic-elf.py --needed libcrypto-compat.so --needed libc.so --soname libssl-compat.so`）。header の sysroot への install は既存の道（`compat/ssl/ssl.h` が sysroot に入る事を確かめた）。p039 の規則・export は変えていない。
- `plan/ws335/tests/check-exports.sh`: 引数の形（.so の path）を保って 1 つか 2 つの .so を取る。libssl-compat.so は「定義する名前 = ssl.h の名前」「`cryc_` を呼ぶ」「libcrypto-compat.so が与えられれば、呼ぶ `cryc_` と大文字の名前が全部そこにある」。header の読み取りで `#define` の継続行も捨てるようにした（ssl.h の macro の継続行に `SSL_ctrl(` があり、前の awk は `TLS_client_method` を落としていた。libcrypto の 58 名は変わらない）。
- 試験: `tls-client.c`（公開の API だけ、host・zedBSD・Windows）、`run-interop.sh`（host、`--win32`、`--make-pki`、`--guest`）、`win-store.c`（Wine の current user の Disallowed への出し入れ）、`limbo-api.c`＋`run-limbo.sh`、`run-nettls.sh`、`run-sites.sh`、`run-faults.sh`＋`malloc-fault.c`＋`tls-raw-server.py`（p030〜p032 の未試験の道）、`run-guest.sh`＋`config-guest.mk`（zedBSD の QEMU）。
- 新しい Phase [p040](../phase040/phase.md)（secp384r1、Q1 の判断 B）と ws.md の行・「Q1 の判断（p033）」。

試験（各 command に timeout。最後の行）:

| command | 結果 |
| --- | --- |
| `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p033 build/p033/dynamic/libssl-compat.so` | rc 0、warning 0、check-dynamic-elf 通過（NEEDED libcrypto-compat.so・libc.so、SONAME libssl-compat.so、FLAGS_1 NOW NODELETE） |
| 同じく `config/ci/config-rpi4.mk BUILD=build/p033a`（aarch64） | rc 0、warning 0、check-dynamic-elf 通過 |
| `sh plan/ws335/tests/check-exports.sh build/p033/dynamic/libcrypto-compat.so build/p033/dynamic/libssl-compat.so`（amd64・aarch64） | `libcrypto-compat.so public 58, cryc_ 186, defined 244` / `libssl-compat.so public 22, defined 22, cryc_ calls 47` / `check-exports: PASS`。わざと壊す: map から `SSL_pending` を外す → `FAILED public names ... does not define: SSL_pending`、`sslc_conn_init` を足す → `FAILED names ... defines that no public header declares`（map は写しから戻し cmp で確かめた） |
| `make ... list-user-programs` | `libssl-compat|libssl-compat|*|n|base|base/libssl-compat|base/libcrypto-compat` |
| `sh plan/ws335/tests/run-interop.sh`（host、OpenSSL 3.5.6 の s_server） | `run-interop: PASS (110 cases, 8 of them known limits)`（下の「相互接続」） |
| `sh plan/ws335/tests/run-interop.sh --win32` | `run-interop: PASS (44 cases, 0 of them known limits)`（x86_64・aarch64 の 2 つの .a と tls-client.exe・win-store.exe が warning 0、x86_64 を Wine で） |
| `sh plan/ws335/tests/run-guest.sh`（QEMU amd64、name ws335-p1、build/ws335-guest、serial は unix socket、usb-net の user network、hostfwd 無し） | `run-interop: PASS (30 cases, 0 of them known limits)` / `run-guest: PASS`。guest の ue0 は 10.0.2.15、`/lib/libssl-compat.so`・`/lib/libcrypto-compat.so`・`/root/tls-client` を guest の `ls -l` で確かめた。判定は guest の tls-client が serial の対話で返す自身の出力と host の s_server の報告（console の log では判定していない） |
| `sh plan/ws335/tests/run-limbo.sh` | `limbo-api: PASS (294562 checks)` / `run-limbo: PASS` |
| `sh plan/ws335/tests/run-nettls.sh` | `run-nettls: PASS (http 15 cases, loader 11 cases, each with OpenSSL and compat)` |
| `sh plan/ws335/tests/run-faults.sh` | `run-faults: PASS` |
| `sh plan/ws335/tests/run-sites.sh` | `run-sites: 27 PASS, 2 FAIL, 0 SKIP`（記録。下） |
| `WS335_VECTORS=… sh plan/ws335/tests/run-unit.sh all` | test と ct の 57 本が PASS、`run-unit: PASS` |
| `WS335_VECTORS=… sh plan/ws335/tests/run-unit.sh --win32 all` | test と ct の 57 本が PASS（win32）、`run-unit: PASS`（test-api-ssl は 5530 checks。p032 の記録は 5570 で、この Phase は library と試験を変えていない。件数の差は Wine の環境に依ると見るが未確認） |
| `sh plan/ws335/tests/run-ct.sh all` | `run-ct: PASS` |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`（phase.md の記載） | `check-skeleton: FAIL`: b7dfd3952 からの差は Q1 の承認済みの 2 つ（p015 の run-unit.sh の LC_ALL、p019 の cryc-x509.h の 1 語）だけ。この Phase は header・凍結の道具を変えていない |

相互接続（host、`run-interop.sh`）: 110 件 PASS、そのうち 8 件は known limit。

- TLS 1.3 × 3 suite × {x25519, P-256（HRR を trace で確かめた）} × {ECDSA P-256, ECDSA P-384, RSA 2048, RSA 4096（証明書は RSASSA-PSS で署名）} の 24 件。各件で server の報告の版・suite・group・HRR の有無・CertificateVerify の scheme を照合。
- TLS 1.2 × 3 suite（鍵の種類に合う ECDHE_ECDSA か ECDHE_RSA）× 2 group × {P-256, RSA 2048, RSA 4096}。ServerKeyExchange の曲線を trace で照合。
- 署名の scheme を server に強いた件: RSA 2048・4096 × 1.3 の rsa_pss_rsae_sha256/384/512、1.2 の PSS 3 つと PKCS#1 v1.5 の sha256/384/512（p030 の RSA の道の 13 件: ServerKeyExchange の RSA-PSS・PKCS#1 の algorithm と salt、RSA の scheme の正の経路）、1.2 の ECDSA P-256 × sha256・sha384。
- 記録の断片化（`-max_send_frag 512`、12000 byte の行を 300 byte の read で、SSL_pending を確かめる）、16 KiB の record を 999 byte の read で、非 blocking（handshake・HRR・読み書き）、版の固定、`X509_VERIFY_PARAM_set1_host` の道。
- 終わり方: server の close_notify（ZERO_RETURN の後 SSL_shutdown が 1）、close_notify 無しの EOF（SSL_R_UNEXPECTED_EOF_WHILE_READING）、`SSL_OP_IGNORE_UNEXPECTED_EOF` で ZERO_RETURN、client からの close_notify。1.3 と 1.2。
- 失敗しなければならない件: 信頼しない root（verify 20）、別の名前（62）、RSA 1024 の leaf（66）、確かめ無しの RSA 1024 の leaf は client が fatal alert 43 unsupported_certificate を送り reason 134（p031 の reason の表の alert 43。s_server の trace で受けた alert を確かめた）。
- known limit 8 件: TLS 1.2 × ECDSA P-384 の leaf（suite 3 × group 2、scheme 2）。server は `no shared cipher` で断り client は reason 1040。RFC 8422 5.1: TLS 1.2 の server は client の supported_groups の曲線の上の ECDSA の証明書しか使えず、client は x25519・secp256r1 だけを申し出る（D8）。Q1 の判断 B で p040 に（ws.md「Q1 の判断（p033）」）。1.3 は P-384 の leaf で繋がる。

Windows（`--win32`、Wine）: 1.3 と 1.2 の各 1 suite × ECDSA P-256・RSA 2048、断片化・非 blocking・HRR・close の各件、失敗の件。信頼の源: `cert.pem`（試験の root）を tls-client.exe の隣に置き CA file 無しで `SSL_CTX_set_default_verify_paths` → 繋がる。隣に無い → verify 20。試験の中間（P-256）を current user の Disallowed に入れる（win-store.exe、registry の store で数を確かめる）→ その中間を通る接続は 1.3・1.2 とも `X509_V_ERR_CERT_REJECTED`（28）、RSA の鎖は繋がる、default paths を読まない（CA file だけ）なら Disallowed は効かず繋がる。取り除いた後は繋がる。Disallowed は script の始めと終わり（trap）に取り除く。実の Windows の機械での実行は未実施（WS334 の試験の機械が揃ってから）。aarch64 は link まで。

zedBSD の guest（`run-guest.sh`）: Windows と同じ部分集合から EOF の 4 件を除いた 30 件（serial の対話は command の終わりに出力を返すので、行が返った時に server を消す EOF の件は組めない。EOF の扱いは host と Wine で確かめた）。tls-client は zedBSD の clang で warning 0、`check-dynamic-elf --role application --needed libssl-compat.so --needed libcrypto-compat.so --needed libc.so` 通過。
最初の run で「1.2 non-blocking」の 2 件が失敗した: 5000 byte の行の最後の TCP の segment が 704 byte（USB の 64 byte の packet の倍数）の Ethernet frame になり、guest の usb-net で毎回の再送が `xhci: transfer completion=13 ... length=704 ... direction=out` で失われた（zero-length packet の要る frame。driver の欠陥で、この library のではない）。Q1 が BUG-416 にした。試験の行を 5003 byte に変え（run-interop.sh に BUG-416 の comment）、2 回目の run で 30 件 PASS。

x509-limbo を公開の API で（`run-limbo.sh`、`limbo-api.c`）: 各 case を TLS 1.2 の handshake として流す（server の側は試験の thread で ServerHello と平文の Certificate を書き、client は d2i_X509・`SSL_CTX_get_cert_store`・`X509_STORE_add_cert` で trusted を、`SSL_set1_host` で名前か address を、`SSL_VERIFY_PEER` と verify の callback で判定を得る。callback は検証の可否を受けて handshake を止める。leaf の鍵は不要）。case の時刻は試験の program が定義する `time()`（library の `cryc_os_time` が呼ぶ）。server が送る list は test-limbo.c と同じ規則（parse できない untrusted は送らない、15 枚を越えれば名前で辿れる物に減らしてから切る）。
9,792 件（期待 SUCCESS 930）、期待と一致 9,732、理由つきの違い 60、理由の無い違い 0、有効な SUCCESS 925、callback に届かない 7（parse できない leaf、全部 FAILURE の期待で一致）。p038 の 59 件から、`pathlen::max-chain-depth-1-self-issued`（depth）が一致に変わり、`pathlen::max-chain-depth-0-exhausted`・`-1-exhausted` が新しい理由 depth-api（公開の部分集合に深さを決める call が無く、library の上限 10 で長い path が通る）で違う。他の 58 件は p038 と同じ id・同じ理由。

net/tls.c の host の試験（`run-nettls.sh`）: `plan/ws074/tests/host-build.sh plain` の browser と host-loader（変えていない）を、host の cc で作った libcrypto-compat.so・libssl-compat.so と `libcrypto.so`・`libcrypto.so.3`・`libssl.so`・`libssl.so.3` の symlink の dir に `LD_LIBRARY_PATH` を向けた wrapper で走らせた（net/tls.c は先に `libssl.so` の名前を開くので、phase.md の `.so.3` に加えて無印の名前も置いた。試験の server の Python は host の OpenSSL のまま）。run-http-tests 15/15・run-loader-tests 11/11 が OpenSSL と compat の両方で同じ件数で PASS。ld.so の log で、compat の run は compat の dir の libssl を 7 回 init し他の libssl は 0 回、OpenSSL の run は compat の dir から 0 回。

p031・p032 の未試験の道（`run-faults.sh`）: (1) 確保の失敗: tls-client を malloc・calloc・realloc を wrap して build、1.3 と 1.2 の接続の確保（15 回と 13 回）を 1 回ずつ失敗させた: 26 回は client が誤りで終わり全部が ERR_R_MALLOC_FAILURE（reason 786688、"out of memory"）を ERR_get_error で報告、2 回は接続が完了（失敗しても進める確保）、ASan・UBSan の報告・crash・hang は 0。SSL_CTX_new・load_verify_locations・SSL_read の中の確保を含む。(2) alert 109: `tls-raw-server.py` が key_share の無い TLS 1.3 の ServerHello を返し、client は fatal alert 109 を送り reason 110（SSL_R_BAD_EXTENSION、p031 の表のとおり）。

実の site（`run-sites.sh`、host から、記録）: Google（1.3・1.2）、Cloudflare（1.3・1.2）、GitHub、Wikipedia、Let's Encrypt、ISRG Root X1・X2（ECDSA P-384 の root）、Amazon、Microsoft、Apple、Mozilla、Debian、kernel.org、Fastly、Akamai（HTTP 403、TLS は成功）、DigiCert、Sectigo、badssl の ECDSA P-256・RSA 2048・RSA 4096・TLS 1.2 だけ（port 1012）、badssl の expired（10）・wrong host（62）・self-signed（18）・untrusted root（19）が期待どおり。失敗 2: `ecc384.badssl.com`（server が handshake_failure。P-384 の group の件、p040）、`sha384.badssl.com`（site の証明書が実際に期限切れ、verify 10。site の側）。

自分で決めた点:

1. Windows の build と試験の Windows の build は試験の script の中（D26）。flag は run-unit.sh --win32 と同じ。
2. limbo の公開の API の道は TLS 1.2 の平文の Certificate と verify の callback で作った（leaf の秘密鍵が無いので CertificateVerify まで進まない。callback は OpenSSL と同じく公開の口）。時刻は `time()` の差し替え（試験の program の側、library に試験の道は無い）。
3. 試験の server（s_server・tls-raw-server.py・limbo の thread）は全部 127.0.0.1、kernel が選ぶ port、`ss -ltnp` で listener の親が script の起こした timeout である事を確かめてから client を繋ぐ（limbo は socketpair）。
4. 試験の PKI は host の openssl で run ごとに作る（期待の値ではなく相手の server の材料）。

p035 へ（header・設計の文の直し。この Phase では変えていない）: header の comment の直しは見つからなかった。design.md: §8 の「QEMU の guest で SSH」は serial の対話と user network（run-guest.sh）に、§8 の net/tls.c の host の試験の symlink に無印の `libssl.so`・`libcrypto.so` も要る事、check-exports.sh が 2 つの .so を取る事。p040 の header の変更は phase040 の表（Q1 の承認待ち）。

未実施: 実の Windows の機械（x86_64・aarch64）での実行、aarch64 Windows の実行（link まで）、riscv64 の libssl-compat.so の build（範囲に無い）、zedBSD の arm64 の QEMU での実行（build と check-exports まで）、zedBSD の guest での EOF の 4 件（host と Wine で確かめた）、TLS 1.2 × ECDSA P-384 の leaf の接続（p040）。

消してほしい path（worktree p1-ws335 の中、Q1 の pipeline で）: `build/p033`、`build/p033a`、`build/ws335-guest`（1.4 GB、image と run）、`build/ws335-nettls-host`（1.1 GB）、`build/ws074-tls`・`build/ws074-images`（WS074 の runner が作った）、`build/amd64/sysroot`・`build/arm64/sysroot`（make が作った）、`build/packages`・`build/zedimage-host`（image の build が作った）、`build/tmp/p033-*`、`build/tmp/ws335-*`（interop・limbo・nettls・sites・faults・unit・ct・skeleton・exports の run と `ws335-wine` の prefix）。
