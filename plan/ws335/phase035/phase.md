<!-- awesome-plan project=zedbsd record=ws335-p035 -->

# ws335-p035: 規約の全文の見直しと header の持ち越しの直し

Status: cleared（2026-11-12 Q1 判定: P2 q2019 で全試験 PASS（posix・win32 の unit 59 本ずつ、ct 19 本と matrix 76、interop host 139・win32 47、limbo 9,792 件、fuzz 11 target）、check-skeleton は許可した header の 18 file だけで FAIL。check-skeleton の BASE を 9e6a72f402 に移す）
Parent: [WS335](../ws.md)。担当: conformance-reviewer（他に作業が無い時、Q1 が割り当てる）。

- WS335 が足した C の全部（`userland/base/libcrypto-compat/`・`userland/base/libssl-compat/`・`include/libc/compat/crypto/`・`include/libc/compat/ssl/`・`plan/ws335/tests/`）を [coding-style.md](../../coding-style.md) の全文と照らし、書き方だけを直す（動作は変えない）。
- 実装の間に見つかった header の comment の誤り（各 phase.md の結果と design-review.md に記録された物）をここで直す。直した後は基点の SHA が変わるので、Q1 が新しい基点を ws.md に書く。
- 受け入れ: style-check の違反 0、run-unit.sh all・run-ct.sh all・check-skeleton.sh・統合の試験の件数が前と同じ。

## 結果（2026-11-02 P1、q1992-i01）

branch `agent/p1-ws335` の commit（WIP）: fca6a1927d・03b24e56e4・b6a201152c（振る舞いの直し）、dd060fa482（header の comment・design.md・run-unit の timeout）、2734d8dd80（library の規約）、fff1ae430e（試験の規約）、9fffbb39ac（main a212b29ea9 の merge、WS343 p002 を含む）、7b745e7e79（P2 の file と試験の道具の規約、evp.h・err.h・rand.h の comment、design.md §5、この記録）。

### 振る舞いの直し

| 項目 | 直した所 | 試験 |
| --- | --- | --- |
| 知らない critical の拡張を持つ anchor を断る（UNHANDLED_CRITICAL_EXTENSION） | cryc-chain.c・cryc-chain.h、limbo の除外の表から `rfc5280::unknown-critical-extension-root` を外す（test-limbo.c・limbo-api.c・test-chain.c） | test-chain（anchor の critical は depth 2 で拒否、critical でない未知の拡張は OK） |
| RFC 8446 4.2 の alert: 送った拡張が許されない message に来たら illegal_parameter、送っていない拡張は unsupported_extension | sslc-msg.c（`sslc_msg_misplaced_alert`）、EE と ServerHello | test-msg・test-hs13・test-hs12 |
| HRR でない ServerHello の cookie は illegal_parameter（1.2 も） | sslc-msg.c（cookie を記録）、hs13・hs12 で拒否 | test-hs13・test-hs12 |
| 保護された TLS 1.3 の方向で application_data・1 byte の CCS 以外の外側の type は長さに依らず EPROTO（unexpected_message） | sslc-record.c・sslc-record.h・sslc-conn.c の comment | test-record |
| SSL_new が context の設定（版の範囲・option・verify の mode と callback）を写す（OpenSSL と同じ） | sslc-api.h の ssl_st に 5 field、api-ssl.c（SSL_ctrl の get は SSL 自身の値） | test-api-ssl（SSL_new の後の context の変更は古い SSL に効かない） |
| test-msg が vector の無い時に UBSan・ASan で落ちていた | test-msg.c（`rfc_available`、無ければ RFC の試験を飛ばして FAIL） | vector 無しで `FAIL (1 of 933979 failed)`、有りで PASS 936799 |
| test-tls の `check_offered` の外側の loop が `index` を使い回していた（1 回だけ回る） | 冗長な外側の loop を外した | test-tls 2230（同じ） |

### header の変更（Q1 が p035 に許可した物。check-skeleton の BASE b8c4adaf6c からの差分）

- 宣言・field の変更は 1 つ: `sslc-api.h` の `struct ssl_st` に `min_version`・`max_version`・`options`・`verify_mode`・`verify_callback`（SSL_new の写し）。
- comment だけ: `include/libc/compat/crypto/evp.h`（失敗の時の誤りの積み方と reason、EVP_PKEY の失敗が出力を変えない事、名前の大文字小文字、EVP_MD_get_size、EVP_MD_CTX_copy_ex の元の条件、EVP_DigestSign の大きさの問い合わせにも start が要る事、EVP_DigestVerify の戻り値と誤りの積み方、EVP_PKEY_derive_init が peer を手放す事、全零の derive は length を変えない事、EVP_PKEY_keygen が古い `*pkey` を解放しない事、EVP_PKEY_encapsulate の length と片方だけの buffer、ChaCha20 の key だけ・IV だけ・counter の終わり、EVP_MAC_init の鍵、EVP_MAC_final の問い合わせ）、`err.h`（`lib(<n>)`・`reason(<n>)`）、`rand.h`（負の count・NULL の buffer・拒否で誤りを積む）、`ssl.h`（SSL_set1_host の NULL と空）、`cryc-api.h`・`cryc-chain.h`・`cryc-err.h`・`cryc-gcm.h`・`cryc-hash.h`・`cryc-mlkem.h`・`cryc-os.h`・`cryc-sigver.h`・`cryc-x509.h`・`sslc-msg.h`・`sslc-record.h`・`sslc-tls.h`（各 phase の持ち越しの EINVAL の一覧などと、上の振る舞いの直しの文）。
- 試験の道具: `run-unit.sh` の 1 本あたりの timeout 600 → 1200 秒（wineboot は 600 のまま）。
- 他の phase の持ち越し（p002・p005・p007・p010・p013・p015・p017・p018・p021・p023・p024・p028・p029・p030・p038・p040）は全部この Phase の header と design.md に入れた。p002 の Wine の prefix の件はこの host で再現しなかった。

### 規約の全文の見直し

- `plan/tools/style-check.py` の違反: 始め（main の p034 の後）library 192・試験 約 3600 → 28。残る 28 は規約が許す物だけ: sslc-keysched.c の goto 8 と tls-client.c の goto 19（どれも 1 つの片付けの label への前向きの跳び、§14）、cryc-os-win32.c の `!BCRYPT_SUCCESS(status)` 1（Windows の header の macro で関数の呼び出しではない）。
- 直した種類: 閉じ括弧の後の空行と段落の comment、static 関数の前方宣言、条件の中の呼び出しを局所変数へ、`?:` を if へ、入れ子の宣言を関数の頭へ、複数行の本体の括弧。定数時間の mask・barrier・wipe と秘密に依る式は変えていない（library の comment でない差分を読んで確かめた）。test-tls の CHECK_VALUE の macro は外して展開した（check の数は同じ）。
- Sonnet の helper（conformance-reviewer）を file の組ごとに使った。TB（x509 側の試験）は指示に反して自分で 7 の sub-helper を立てた（Q1 の上限 3 を超えた）。結果は下の件数の照合で確かめた。

### 試験（host、`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）

- worktree の `build/ws335-vectors` が無くなっていた（P1 は消していない）。共有の写しを使った。
- `run-unit.sh all`（posix、試験の規約の後・merge の前）: 57 本 PASS、`run-unit: PASS`。
- 件数の照合: HEAD（2734d8dd80、試験の規約の前）を `build/tmp/p035-base/` に `git archive` で写して同じ 36 本を流し、規約の後と全部同じ件数（例 test-msg 936799、test-name 255575、test-sigver 492861、test-api-x509 507799、test-buf 1559755、test-conn 164467）。P2 の試験は helper TP が前後で同じ件数を確かめた（test-aes 28516・test-api-evp 190769・test-api-pkey 40701・test-api-passkey 24754）。ct-* は helper TC が valgrind で前後同じ件数。limbo-api・tls-client・malloc-fault・crypto-smoke は helper TD が新旧の出力の一致を確かめた（limbo-api 9792 件 294559 checks、tls-client の 13 通りの option と s_server、crypto-smoke 47 checks）。
- 最後の tree（7b745e7e79、merge と P2 の file の規約の後）の `run-unit.sh all`（posix）: 59 本 PASS（WS343 の test-api-passkey 24754・ct-api-passkey 23 を含む）、`run-unit: PASS`。前の 57 本の件数は merge の前と全部同じ。

## 引き継ぎ（2026-11-02 23:59 P1 ラップアップ）

未実施（次の担当が流す）:

1. `sh plan/ws335/tests/run-unit.sh --win32 all`（Wine。test-os・test-api-ssl など _WIN32 の差分がある）。
2. `sh plan/ws335/tests/run-ct.sh all`・`run-ct-matrix.sh`（library の規約の差分は comment・空行・公開の値の条件だけだが、ct の確かめは未実施）。
3. `BASE=b8c4adaf6c sh plan/ws335/tests/check-skeleton.sh`: 上の「header の変更」の一覧だけで FAIL するはず（全部 Q1 の許可の範囲）。その後 Q1 が BASE を移す。
4. `run-interop.sh`（host と `--win32`）・`run-limbo.sh`・`run-fuzz.sh` の短い流し（tls-client・limbo-api・fuzz の規約の差分のため）。win-store.c は mingw の構文の確かめだけ。
5. 全部 PASS なら ws.md の p035 の行を「実装済み・試験 PASS」にし、cleared は Q1。

p036 への申し送り:

- 規約の見直しは style-check の機械の規則と目で読んだ範囲。全ての `if`・return の前の comment など、checker が見ない規則は helper の読みに依る（TA・TP・TC は「全部には付けていない」と報告）。
- 試験の小さな気付き（直していない）: test-api-passkey.c の `check_hkdf` の `memset(out, 0, sizeof(other))`（害は無い）、test-api-pkey.c の `test_ed25519_states` の使われない `length`、test-sha3.c の連鎖の試験の読まれない `expected`、win-store.c の `walk_registry` が Disallowed の key を開けない時 0 を返す事。
- test-keysched.c と test-util.c の乱数の引数の評価の順が左から右に決まった（前は compiler 次第）。check の数は同じ。
- 削除を頼む path（Q1）: `build/tmp/p035-base/`・`build/tmp/p035-reg/`・`build/tmp/p035-*`（scratch）・`build/tmp/style-*`（helper の作業場所）。

## 結果（2026-11-12 P2、q2019-i01）

worktree `p2-ws335a`（基点 main 4bdcf82fd6、P1 の p035 の commit は全部入っている）。source の変更は無し（直す物が無かった）。試験の data は共有の `build/sources/tls-vectors` が無くなっていたので、`sh plan/ws335/tests/fetch.sh specs`・`fetch.sh vectors` で worktree の `build/ws335-specs`・`build/ws335-vectors` に取り直した（両方 `fetch: PASS`、digest は script の表のとおり）。`check-skeleton.sh` の zedBSD の clang のために `make CONFIGMK=build/p035/config.mk llvm-toolchain-cached`（`PLATFORM := x86_64` だけの config）で `build/llvm` を入れた。試験の script は新しい build system の前提に触れない（host の cc・clang・llvm-mingw・Wine と `build/llvm/bin/clang` だけを使う）ので直していない。

| 試験（command） | 結果 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh all`（posix） | `run-unit: PASS`、59 本（test 40・ct 19）。件数は P1 の記録と同じ（test-msg 936799、test-name 255575、test-sigver 492861、test-api-x509 507799、test-buf 1559755、test-conn 164467、test-aes 28516、test-api-evp 190769、test-api-pkey 40701、test-api-passkey 24754） |
| `sh plan/ws335/tests/run-unit.sh --win32 all`（Wine 10.0） | `run-unit: PASS`、59 本。posix と違う件数は Windows の分岐のある test-api-ssl 5785（posix 5342）・test-api-x509 507796・test-err 4587・test-os 1455 だけ（前の Phase の win32 の件数と同じ形） |
| `sh plan/ws335/tests/run-ct.sh all` | `run-ct: PASS`、19 本 |
| `sh plan/ws335/tests/run-ct-matrix.sh all`（cc・clang × -O2・-O3） | `run-ct-matrix: PASS (76 builds)`（19 × 4、FAIL 0） |
| `BASE=b8c4adaf6c sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: FAIL`。FAILED は「headers or test tools changed since b8c4adaf6c」の 1 つだけで、18 file は上の「header の変更」の一覧と同じ（err.h・evp.h・rand.h・ssl.h・run-unit.sh・cryc-api.h・cryc-chain.h・cryc-err.h・cryc-gcm.h・cryc-hash.h・cryc-mlkem.h・cryc-os.h・cryc-sigver.h・cryc-x509.h・sslc-api.h・sslc-msg.h・sslc-record.h・sslc-tls.h）。他の検査（cc・clang の単独の compile、zedBSD の clang の x86_64・aarch64、llvm-mingw の x86_64・aarch64 と static link、層、名前空間）は FAILED の行が無い。BASE を移すのは Q1 |
| `sh plan/ws335/tests/run-interop.sh`（host） | `run-interop: PASS (139 cases, 0 of them known limits)` |
| `sh plan/ws335/tests/run-interop.sh --win32` | `run-interop: PASS (47 cases, 0 of them known limits)`（tls-client.exe と win-store.exe は x86_64・aarch64 で warning 無しに build、cert.pem と Disallowed の case を含む） |
| `sh plan/ws335/tests/run-limbo.sh` | `run-limbo: PASS`。9,792 件のうち一致 9,733・理由つきの違い 59・理由の無い違い 0、limbo-api 294559 checks（P1 の記録と同じ） |
| `FUZZ_SECONDS=300 sh plan/ws335/tests/run-fuzz.sh` | `run-fuzz: PASS`、11 target 全部 crash 0（例 conn 30717 runs、record 193026 runs、x509 24745909 runs）。11 target を並べて流した（Q1 の 4 thread の上限の指示の前に終わった） |

- 持ち越し（Q1）の 2 点（cryc-hash.h の HKDF・HMAC の init の EINVAL の comment、cryc-chain の知らない critical の拡張を持つ anchor の拒否）は P1 の commit で main に入っていた。header の文と `cryc-chain.c` の分岐を読んで確かめ、試験は上の test-chain・test-hash・limbo の PASS。
- Wine の prefix の持ち越し（p002）: この host（Wine 10.0、Debian 10.0~repack-6）で新しい prefix `build/tmp/ws335-wine` を `run-unit.sh --win32` が `wineboot -i` で作り、約 1 分で `.ws335-ready` が付いた（kernel32.dll の止まりは再現しない、P1 の所見と同じ）。script は直していない。再現した時の手当て（出来た prefix を写す手順）は、再現の後に決める。
- 未実施: zedBSD の `libcrypto-compat.so`・`libssl-compat.so` の build と check-exports（新しい build system の target、sysroot が要る。今回の引き継ぎの範囲の外。zedBSD の header に対する compile は check-skeleton の zedBSD の clang の段で PASS）。QEMU の guest の interop。
- cleared の材料: 受け入れの「style-check の違反 0」は P1 の 28 件（規約が許す物だけ）、run-unit all（posix・win32）・run-ct all・run-ct-matrix・interop（host・Wine）・limbo・fuzz が PASS で件数は前と同じ、check-skeleton は許可済みの header の差だけ。p035 は cleared にできると考える（判定は Q1）。
- 試験の道具: `run-fuzz.sh` は `FUZZ_JOBS`（既定 4、上限 4）本ずつ流す形にした（2026-11-12 q2021 P2、`FUZZ_SECONDS=30` で 11 target PASS、同時は最大 4 本）。check-skeleton の BASE はこの merge に移す。

## 持ち越し（Q1）

- 2026-11-01 p002: `cryc-hash.h` の HKDF（extract・expand）と HMAC の init の comment に、NULL の pointer と 0 でない長さの組が EINVAL である事を書く（今は ENOTSUP だけを約束）。
- 2026-11-01 p002: `plan/ws335/tests/run-unit.sh --win32` が新しい Wine の prefix を作る時、この host では `wineboot -i` が kernel32.dll を読めず止まる（担当は ws334-p019 の prefix を写して回避）。prefix の作り方を直すか、出来た prefix を写す手順を script に入れる。


## Q1 の追記（2026-11-02 02:35）

- p028 の結果から: ServerHello・HelloRetryRequest で、知っていてその message に指定の無い拡張は RFC 8446 4.2 どおり illegal_parameter、送っていない拡張は unsupported_extension に分ける（sslc-msg.c と header の comment）。`sslc_msg_write_client_hello` の EINVAL の一覧も header に足す。

- （2026-11-02 11:50 Q1、p038 から）cryc-chain.h と cryc-chain.c: 知らない critical の拡張を持つ anchor を断る。run-unit.sh の 1 本あたりの timeout を 1200 秒に。


- 2026-11-12 Q1: check-skeleton の BASE を 9e6a72f402（p035 の後の main）に移す。以後の照合は `BASE=9e6a72f402 sh plan/ws335/tests/check-skeleton.sh`。
