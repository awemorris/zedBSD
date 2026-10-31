<!-- awesome-plan project=zedbsd record=ws335-design-review -->

# WS335 p001 の design-reviewer の記録（2026-11-01）

- review の対象: P6 の worktree `agent/p6-ws335` の 093dd4b3a（design.md・header・骨格・phase.md・試験の道具）。design-reviewer が 1 本、敵対的に読んだ（worktree は変えていない）。
- reviewer が自分で確かめた事: header の OpenSSL の定数（SSL_R_*・X509_V_*・ERR_LIB_*/ERR_R_*・flag の bit・SSL_CTRL_*・SSL_OP_*・SSL_ERROR_*・EVP_PKEY の NID・OSSL_PARAM の配置）は 3.5.8 と一致。FIPS 203・197-upd1・186-5 の節・algorithm・式の番号。header の単独の compile（cc・clang、-std=c89 -Wpedantic）。骨格の .c が全部どれか 1 つの Phase に入る。primitive の header に TLS の型が無い。
- reviewer が流していない物: check-skeleton.sh・run-unit.sh・run-ct.sh（worktree の build/ に書くため）。P6 が反映の後に流した（下）。
- 指摘 27 件（高 3・中 9・低 15）。全部を反映した。見送りは無い。27 は推定の指摘で、p038 で件数を見てから決める形にした。

| # | 重さ | 指摘（要点） | 扱い |
| --- | --- | --- | --- |
| 1 | 高 | RFC 8448 の full の trace は handshake で一致しえない（ClientHello の拡張と session id が違う、server の鍵が RSA-1024） | 反映: RFC 8448 は p026〜p028 だけに。handshake の試験の相手は新しい p037 の試験の道具 `tlspeer.h`（scripted な server と試験の側の ECDSA の署名器）。design D22、p029〜p031・p033 を直した。乱数の引く順を sslc-hs.h に書いた |
| 2 | 高 | 先読みで libbrowser が待ちに入る（SSL_pending==0 で poll） | 反映: sslc-conn.h に「今の record の残りだけを求める」、design D21、p031 に試験 |
| 3 | 高 | cryc_ed25519_sign が呼び手の公開鍵を取る（違う A で 2 回署名すると秘密の scalar が漏れる） | 反映: 引数から公開鍵を外し、seed から内部で導く（cryc-ed25519.h、骨格も） |
| 4 | 中 | TLS 1.3 の key change は record の境に（同じ record に残った message） | 反映: sslc-conn.h（1.3 と 1.2 の CCS の両方）、p031 に試験 |
| 5 | 中 | handshake の確かめの抜け（1.3 の ServerHello の許されない拡張、HRR の後の version と group、1.2 の session id の echo、1.3 の Certificate の context、EE の server_name、CCS を送る時、PSS の salt、1.2 の ECDSA は曲線を縛らない、1.2 の HelloRequest、legacy_version の alert） | 反映: sslc-hs.h の確かめの一覧に全部 |
| 6 | 中 | record の最小の長さ、長さ 0 の規則の場所、2^14 の上限の意味 | 反映: sslc-record.h（最小 17・24・16 は EBADMSG、長さ 0 は復号後、TLSInnerPlaintext は padding 込みで 2^14+1） |
| 7 | 中 | EVP_Cipher の戻り値（OpenSSL 3.5 は長さと -1） | 反映: evp.h |
| 8 | 中 | writer の深さ 4 が足りない | 反映: 6 に、comment を直した |
| 9 | 中 | vecfile.h の 1 byte の heap の溢れ | 反映: NUL の前にも伸ばす |
| 10 | 中 | 出典が file の先頭にある（仕様の版の文、論文、ctgrind、Layout referenced、ssh・net/tls.c の言及） | 反映: 版の文は include の後の block に移した。Montgomery の論文は `cryc_bn_mont_mul` の前の「Procedure referenced:」、ctgrind は ct.h の macro の block の「Procedure referenced:」、types.h は「Values referenced:」に。先の利用者の名前（ssh・libbrowser）は機能の言い方に替えた。header の先頭の「何の規格を実装するか」の 1 文（例 RFC 8017 の RSA）は機能の説明として残した（libz-compat の zlib.h の前例、RFC 1950） |
| 11 | 中 | SSL_write の再試行で長さが違う、長さ 0 と負 | 反映: ssl.h（BAD_WRITE_RETRY・BAD_LENGTH）、定数を足した |
| 12 | 中 | handshake・接続の試験の独立の相手が無い | 反映: p037（1 と同じ） |
| 13 | 低 | 接続の前の SSL_shutdown・SSL_free、handshake の途中の shutdown、fatal の後の close_notify | 反映: ssl.h・sslc-api.h・sslc-conn.h（SHUTDOWN_WHILE_IN_INIT、PROTOCOL_IS_SHUTDOWN） |
| 14 | 低 | SSL_set1_host の振る舞いが OpenSSL 3.5 と違う | 反映: ssl.h（host と address を消してから、IP の文字列は address に） |
| 15 | 低 | d2i_X509 の誤りの道と後ろの bytes | 反映: x509.h（*out を放して NULL、後ろの bytes は読まない） |
| 16 | 低 | 出典の番号の誤り（6.4.2 step、6.1.4 (g)、OpenSSL の counter の繰り上がり） | 反映: step 6・step 3、6.1.3 (b)(c)、evp.h の counter の注 |
| 17 | 低 | 点の decode の確かめの順で ENOTSUP が出ない | 反映: cryc-ecp.h に順を書いた |
| 18 | 低 | 負の serial を読む道が無い | 反映: cryc-x509.h（cryc_der_expect で読む） |
| 19 | 低 | sslc-tls.h の範囲に ALPN、SSL_CIPHER_get_name の言及 | 反映: 消した |
| 20 | 低 | name constraints の仕事の上限が無い | 反映: CRYC_NAME_MAX_CHECKS（4096、越えたら ENOTSUP） |
| 21 | 低 | KeyUpdate の flood、1.3 の user_canceled、NST の lifetime | 反映: sslc-conn.h（KeyUpdate と user_canceled を進まない record に数える、user_canceled は closure）、sslc-msg.h（lifetime を見ない） |
| 22 | 低 | lock の順が無い | 反映: cryc-chain.h・sslc-api.h（ctx の lock → store の lock、file の読みと callback は lock の外） |
| 23 | 低 | 依存の直列が多すぎ、p020 が大きすぎ、phase.md の §4 と §5 の矛盾 | 反映: p027 は p025・p002、p028 は p025 に。limbo の全件は新しい p038（Opus）に分けた。§4 の文を「内部の事は決める、interface の不備は止める」に |
| 24 | 低 | check-skeleton の BASE が試験の道具と消した header を見ない、run-unit の名前の誤り、ct-*.c が ASan で走らない | 反映: BASE は header と試験の道具を git の pathspec で（足した・消した物も）、fetch.sh の名前、run-unit が `ct-<名前>` も流す |
| 25 | 低 | SIGPIPE、Windows の CA file の module の言い方 | 反映: MSG_NOSIGNAL のある system では send(MSG_NOSIGNAL)、ENOTSOCK で write、zedBSD は write。CA file は libcrypto-compat の DLL の隣 |
| 26 | 低 | SSL_get_error の振る舞い | 反映: ssl.h（呼んだ時に決めた分類を返し、SYSCALL では errno を戻す） |
| 27 | 低（推定） | trust anchor の pathLen と basicConstraints を見ない事が limbo と違う | 反映の形: p038 が件数を見てから Q1 に案を返す（phase038 に書いた） |

## 反映の後の確かめ（2026-11-01 P6）

- check-skeleton.sh PASS（cc・clang、zedBSD の clang の x86_64・aarch64、llvm-mingw の x86_64・aarch64）、run-unit.sh all PASS（test-tools 18 checks）、`git diff --check` 空。
- WS336 の crypto-contract.c（P5）が compat の header と骨格で -Werror の build と link を通る（実行は骨格なので FAIL、実装の後に p023・p024 の受け入れ）。
- 凍結の基点: 60760d2a0。

## 2 回目（q1789-i02、2026-11-01）: U1〜U6 の答えと Windows の static link の変更

- review の対象: P6 の worktree の 43b8fb577 と da00b1f47 の差（header 5・骨格 4・試験の道具 2・design.md・ws.md・user-questions.md・phase.md、新しい p039）。design-reviewer が 1 本、敵対的に読んだ（worktree は変えていない）。
- reviewer が自分で確かめた事: tree の写しで check-skeleton.sh PASS（zedBSD の clang と BASE は写しなので流していない）、llvm-nm（COFF）と nm（ELF）の出力の読み、`run-unit.sh --win32 tools` が Wine 10.0 で PASS（18 checks）、UBSan の trap が Wine で FAIL になる事（偽の PASS にならない）、header が winsock2.h・windows.h・wincrypt.h・bcrypt.h の後でも compile できる事、OpenSSL 3.5.8 の `X509_V_ERR_CERT_REJECTED` 28 と文 "certificate rejected"、Wine の ROOT に host の CA が 155、Disallowed に足した証明書が prefix に残る事。
- 流していない物: zedBSD の clang の段、QEMU、実の Windows（logical store の中身、拡張鍵用途の property、process の終わりの FLS）。
- 指摘 16 件（高 1・中 6・低 9）。全部を反映した。見送りは無い。

| # | 重さ | 指摘（要点） | 扱い |
| --- | --- | --- | --- |
| 1 | 高 | WS334 p030 が net/tls.c の表に OpenSSL だけの関数（`OPENSSL_init_ssl`・`X509_digest`・`X509_STORE_CTX_*`）と証明書ごとの callback を足すので、「開く名前だけ替える」切り替えは zedBSD でも Windows でも成り立たない | 反映: D28（OpenSSL だけの関数は compat に作らない）、design.md §4.2 に compat の下の net/tls.c の変更 1〜4、p036 を直した。Q1 への頼み（p030 の前に P7 と表の印を合わせる）を §4.2 に |
| 2 | 中 | `CertOpenSystemStoreW` は current user しか開けない | 反映: cryc-os.h に `CertOpenStore(CERT_STORE_PROV_SYSTEM_W, …)` と 4 つの組と flag、X509_ASN_ENCODING でない物を飛ばす。p015 |
| 3 | 中 | 信頼しない印の確かめの時と深さ、WORK_EXCEEDED との優先が決まっていない | 反映: cryc-chain.h に「完全な path の最初に leaf から上へ、一番下の深さで REJECTED」と、どの path も通らない時の code の優先。p020 の試験 |
| 4 | 中 | `cryc_store_add_system` の lock の持ち方 | 反映: cryc-chain.h（cryc_os_system_roots の間は持たない、callback ごとに取る） |
| 5 | 中 | Wine の prefix が run の間で残り、試験が足した証明書が後の run に効く | 反映: run-unit.sh の comment と p015・p033（足した物は終わりに消し、始めにも前の残りを消す） |
| 6 | 中低 | 同じ run の試験が他の試験の `cert.pem` を見る、p020 の add_system が POSIX だけの言い方 | 反映: run-unit.sh の .exe を試験ごとの dir に。p020 に `--win32` での期待 |
| 7 | 中（一部未確認） | ROOT の証明書ごとの制限（拡張鍵用途の property、「全部の目的で無効」）を見ない、store の場所 | 反映: cryc-os.h（serverAuth を持たない property の物は飛ばす、logical store が physical の部分を含む事）、D24、p015 |
| 8 | 中低 | check-skeleton の 7 が nm の失敗・空の glob で黙って PASS | 反映: nm の終わりの値、object の有無、`cryc_`・`ERR_`・`sslc_`・`SSL_` の名前が見える事（正の対照）を FAIL の条件に。壊した nm（存在しない path）で FAIL になる事を確かめた |
| 9 | 低中 | zedBSD の .so の dlclose と thread の値の destroy、`WSAStartup` の約束 | 反映: cryc-os.h（`-z nodelete`）、p039・p033 の規則、ssl.h の `SSL_set_fd` に `WSAStartup` |
| 10 | 低中 | 記録が 2 回目の review を「反映した」と先に書いていた | 反映: この節を書いてから commit（ws.md・design.md の文はこの節を指す） |
| 11 | 低 | gnu11・macro 無しの compile を確かめる道具が無い、§4.4 の言い過ぎ、p037・p038 に `--win32` が無い | 反映: check-skeleton の 6 に `-std=gnu11 -O2`（macro 無し、x86_64）、§4.4 の 5 を直した、p037・p038 に `--win32` |
| 12 | 低 | `cryc_store_free` が印の配列に触れない、`cryc_os_read_file` の binary の mode、UTF-16 の不正な surrogate | 反映: cryc-chain.h・cryc-os.h（`rb`・`L"rb"`、`WC_ERR_INVALID_CHARS`、変換できない path は無い file と同じ）、p015 |
| 13 | 低 | p021 の Windows の thread の作り方と、見える事 | 反映: p021（CreateThread・`_beginthreadex`、別の thread の queue が見えない事、1000 の thread） |
| 14 | 低 | ROOT を Disallowed と照らす filter は重複で、同じ根の code が出所で違う | 反映: filter を外した（cryc-os.h、D24、p015） |
| 15 | 低 | 相対の `WS335_VECTORS` の Z: の path、wineboot の Mono・Gecko の問い | 反映: run-unit.sh（絶対の path に、`WINEDLLOVERRIDES='mscoree,mshtml='`） |
| 16 | 低 | test-tools.c の「全部の module を link」、cryc-chain.h の「ROOT は約 400」、user-questions.md の元の問いの DLL の文 | 反映: 前の 2 つを直した。user-questions.md は元の問いの文として残し、「決まった」の行で答えを書いた |

## 2 回目の反映の後の確かめ（2026-11-01 P6）

- 基点の commit b7dfd3952 の tree で: check-skeleton.sh PASS（cc・clang、zedBSD の clang の x86_64・aarch64、llvm-mingw の x86_64・aarch64 の compile・static link・名前の範囲、x86_64 の gnu11）、`BASE=b7dfd3952` でも PASS、
  run-unit.sh all PASS（test-tools 18 checks）、run-unit.sh --win32 all PASS（test-tools 18 checks、Wine）、`git diff --check` 空。WS335_VECTORS は共有の `/home/awe/zedBSD-claude1/build/sources/tls-vectors`。
- 壊す確かめ: 失敗する nm を PATH の先に置くと check-skeleton の 7 が `FAILED cc: nm failed on …` で FAIL。名前の範囲の外の名前（`helper`・crypto の object の `SSL_x`）を filter が拾う事を別の object で確かめた。
