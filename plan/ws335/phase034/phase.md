<!-- awesome-plan project=zedbsd record=ws335-p034 -->

# ws335-p034: 安全の review と hardening

Status: 実装済み・試験 PASS（2026-11-02 21:35 P1 q1986-i01。cleared は Q1）。前: in-progress（2026-11-02 19:00 P1 q1986-i01）。前: planned（2026-11-01 p001 が作成。依存: p033）
Parent: [WS335](../ws.md)。担当: Opus（敵対的な review。実装の担当とは別の起動）。

## 1. 読む物

- [design.md](../design.md) §6・§8、全部の header（約束）、全部の .c。RFC 9325（BCP 195）、RFC 8446 の付録 C・E（実装の注意と安全の考察）、FIPS 203 の 3.3（実装の要件）。

## 2. 行う事

- 定数時間: run-ct.sh を全部の秘密の道（AES・GCM・ChaCha20/Poly1305・HMAC/HKDF・X25519・P-256 の scalar 倍と ECDH・Ed25519 の署名・ML-KEM の decaps・record の open・Finished の比較・鍵の schedule）に。足りない ct-*.c を足す。-O2 と -O3、cc と clang の両方。
- fuzz: libFuzzer（clang `-fsanitize=fuzzer,address,undefined`）で、DER・X.509・PEM・ServerHello・EncryptedExtensions・Certificate・ServerKeyExchange・record の open・conn の受信の全体・ML-KEM の decaps と ek の import。各 1 時間（夜の長い fuzz はユーザーに確かめてから）。corpus は `build/tmp/` に。
- review の checklist: 秘密の消去の漏れ（全部の return の道）、整数の overflow（size_t の加算・乗算）、長さの照合の漏れ、例外の点（無限遠・全零）、乱数の失敗の道、alert の選び方、thread の安全（SSL_CTX・store・誤りの queue）、Windows の道（UTF-8 と UTF-16、store の handle の解放）。
- 見つけた欠陥は直す（header の変更は Q1 に返す）。直した物ごとに試験を足す。

## 3. 受け入れ

- 全部の試験と fuzz が PASS（crash 0）、ct の違反 0、review の指摘の全部が直されたか理由付きで見送り（結果の表）。

## 4. 結果

2026-11-02（host の `date`、19:00〜20:10）P1（Opus、q1986-i01）。worktree `/home/awe/zedBSD-worktrees/p1-ws335`（main 52417432e6 から）。review は 5 つの読むだけの subagent（libssl の conn・record・msg・api、handshake と AEAD、X.509 と path、OS・ERR・EVP、primitive）に分けて行い、指摘は全部この担当が code で確かめてから直すか見送るかを決めた。凍結の header は変えていない。

### 4.1 道具（新）

- `plan/ws335/tests/run-ct-matrix.sh`: run-ct.sh の ct-*.c を {cc, clang} × {-O2, -O3} で build し valgrind memcheck で流す（run-ct.sh は凍結の道具なので変えず、別の script にした）。
- `plan/ws335/tests/run-fuzz.sh` と `plan/ws335/tests/fuzz/`: libFuzzer（clang 19、`-fsanitize=fuzzer,address,undefined`）の 11 の target: der、x509（cryc_x509_parse・名前の照合・d2i_X509）、pem（PEM の block・base64・store）、server-hello、encrypted-extensions、certificate、server-key-exchange、msg-other（CertificateVerify・CertificateRequest・NewSessionTicket・KeyUpdate）、record（平文、TLS 1.3 の正しい tag の内側の平文、TLS 1.2 の封と開の往復）、conn（SSL_connect の受信の全体、socket pair）、mlkem（decaps・ek の確かめと import）。`fuzz/seeds.py` が x509-limbo の証明書と手で書いた message・record から seed を作る。corpus は `build/tmp/ws335-fuzz-corpus/`。
- ct-hs.c（sslc_hs_shared_secret の ct）は作ったが、関数の中の誤りの値の分岐（primitive の誤り、設計上公開）を memcheck が秘密の分岐と報告し、関数の外から宣言を外せないので、tree に入れなかった（下書きは `build/tmp/p034-ct-hs.c`）。その中の算術は ct-ecc・ct-x25519 が確かめる。

### 4.2 指摘と処置

| # | 指摘（出所） | 重さ | 処置 | 試験 |
| --- | --- | --- | --- | --- |
| 1 | clang 19 -O2/-O3 が `cryc_bn_select` と `cryc_bn_mod_sub` の mask を秘密による分岐と load の選択に変えた（P-256・P-384 の scalar 倍と ECDH、RSA の Montgomery）。cc では起きない（ct matrix） | 高 | 直した: mask を volatile の壁（`bn_opaque`）に通す（cryc-bn.c） | run-ct-matrix の ct-bn・ct-ecp・ct-ecc（clang）が修正前 FAIL、後 PASS |
| 2 | 同じ形の潜在の危険: Ed25519 の scalar の mod L（`ed_reduce_order`）、ML-KEM の poly の加減、f25519 の cswap、`cryc_ct_select_bytes`・`cryc_ct_copy_if`（LTO なら mask の値域が見える）（primitive の review） | 中（潜在） | 直した: 各 file の中の同じ壁 | ct matrix 72 build PASS、run-unit |
| 3 | `aes_field_square` の局所の `squared[8]`（秘密の slice）を消していない | 低 | 直した | run-unit aes |
| 4 | Windows: EKU の property が無い ROOT を `CertGetEnhancedKeyUsage` の文書どおりに扱わず「全部の目的で無効」と捨てていた（cUsageIdentifier 0 と GetLastError の区別） | 中（閉じる側） | 直した（cryc-os-win32.c） | test-os --win32（property 無し・空の property の 2 件は前から有る。Wine の返し方では前も通った。実の Windows は未実施） |
| 5 | iPAddress の制約の mask で、部分の byte の後の byte が 0 でなくても通った（`ff f0 f0 00`） | 低 | 直した（cryc-name.c） | test-name に 4 件（修正前 3 件 FAIL） |
| 6 | 自分の close_notify の後に、KeyUpdate（update_requested）の答えや no_renegotiation を送っていた（RFC 8446 6.1、RFC 5246 7.2.1） | 中 | 直した: CLOSE_SENT の後は SEND・CCS・WARNING・SET_WRITE_KEYS を捨てる（読みは続く）（sslc-conn.c） | test-conn に 1 件（修正前 FAIL） |
| 7 | 失敗した接続で `SSL_connect` が 1 を返した | 低 | 直した: FAILED なら PROTOCOL_IS_SHUTDOWN（api-ssl.c） | test-api-ssl に 1 件（修正前 FAIL） |
| 8 | TLS 1.2 だけの client が ServerHello の前の CCS を捨てていた（RFC 5246 7.1） | 低 | 直した: 1.3 を申し出た時だけ捨てる（sslc-hs.c） | test-hs13 に 1 件（修正前 FAIL） |
| 9 | client の Finished の stack の写し（hs13・hs12）、再組み立ての buffer（free の前）を消していない | 低 | 直した | run-unit |
| 10 | CCS が分割された handshake message の間に来る（RFC 8446 5.1）| — | 見送り: 既に unexpected_message で断っている（sslc-conn.c:855）。試験を足した（test-conn の manual_server で Certificate を 2 つの record に割り間に CCS） | test-conn |
| 11 | Disallowed の印が DER 全体の SHA-256 なので、ECDSA の (r, n−s) で DER を変えると盗まれた中間が通る | 高 | 直した（Q1 が案 A を承認）: 印は tbsCertificate の SHA-256（parse できない DER は全体の SHA-256）、照合は候補の tbsCertificate と DER の両方の SHA-256（cryc-chain.c）。cryc-chain.h の 2 つの comment を承認の範囲で直した。D24 の文は p035 | test-chain の malleate()（署名を (r, n − s) に書き直した中間）: 修正前は印を通り抜けて FAIL（test-chain.c の REJECTED の確かめ）、後は REJECTED。印の無い時は正しい鎖として通る事も確かめる |
| 12 | 信頼の起点の知らない critical の拡張を断らない | 中 | 見送り: Q1 の判断（p038）で p035 に予定済み | — |
| 13 | cookie を HRR でない ServerHello に、知っている拡張を EE に置いた時の alert が unsupported_extension（RFC 8446 4.2 は illegal_parameter） | 低 | 見送り: p028 の Q1 の判断で p035 に予定済み（header の comment も） | — |
| 14 | 保護された 1.3 の向きで短い別の型の record が bad_record_mac（長ければ unexpected_message） | 低 | 見送り（p035 へ）: sslc-record.h の約束と test-record が今の振る舞いを定める。直すなら header の comment の変更 | — |
| 15 | TLS 1.3 の CertificateRequest で context が空でない・signature_algorithms が無いのを受ける | 低 | 見送り: sslc-msg.h は構文だけを約束、RFC の SHALL は server に課す。client は空の Certificate を返すだけで安全に影響しない | — |
| 16 | 1 つの record に詰めた多数の HelloRequest に毎回 warning で答える（約 8 倍の増幅） | 低 | 見送り: peer の帯域の定数倍で、record ごとの無進捗の上限 32 の中 | — |
| 17 | 名前の制約の照合で、別の形の subtree も毎回読む（予算は同じ形の比較だけを数える） | 低 | 見送り: cryc-name.h が比較の数を約束、test-name が「別の形は数えない」を固定。最悪は約 10^8 の要素の読みで、Certificate の 100 KiB の上限の中 | — |
| 18 | 名前の空の subject を critical でない SAN でも受ける | 低 | comment だけ直した: 発行の側の規則で、limbo の issuance の理由（p038）と同じ判断 | — |
| 19 | PSS の DEFAULT の値の明示の encode を受ける、IP の文字列を set1_host で名前として扱う | 低 | 見送り: 害が無い、OpenSSL と同じ | — |
| 20 | Windows の部分の不信（disallowed の日付・EKU の property）を読まない、FLS の呼び出しが GetLastError を変えうる | 低 | 見送り: D24 の範囲の外（記録）。WSAGetLastError は errno と SSL_get_error が正しく返す | — |
| 21 | Ed25519 の確かめは cofactor 無しで小さい位数の A・R を受ける、SampleNTT の 64 block の上限 | 注 | 見送り: RFC 8032 5.1.7・FIPS 203 のとおり | — |

### 4.3 試験

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-ct-matrix.sh all`（修正前） | `run-ct-matrix: FAIL (6 of 72 builds)`（ct-bn・ct-ecp・ct-ecc の clang -O2・-O3） |
| `sh plan/ws335/tests/run-ct-matrix.sh all`（修正後） | `run-ct-matrix: PASS (72 builds)` |
| `sh plan/ws335/tests/run-ct.sh all` | `run-ct: PASS` |
| `FUZZ_SECONDS=300 sh plan/ws335/tests/run-fuzz.sh`（11 target を並べて 5 分、修正後の code） | `run-fuzz: PASS`、crash 0。実行の回数: der 4.35M、x509 26.1M、pem 1.42M、server-hello 91.7M、encrypted-extensions 120.6M、certificate 59.3M、server-key-exchange 93.6M、msg-other 83.3M、record 148k、conn 30k、mlkem 87k |
| `FUZZ_SECONDS=3600 sh plan/ws335/tests/run-fuzz.sh`（ユーザーの承認を Q1 が伝えた。11 target を並べて 1 時間、20:26〜21:27、#11 の修正の後の code） | `run-fuzz: PASS`、crash・leak・timeout 0。実行の回数: der 57.0M、x509 264.8M、pem 4.94M、server-hello 993M、encrypted-extensions 1286M、certificate 635M、server-key-exchange 999M、msg-other 832M、record 688k、conn 336k、mlkem 973k |
| `WS335_VECTORS=… sh plan/ws335/tests/run-unit.sh all` | test と ct の 57 本が PASS、`run-unit: PASS` |
| `WS335_VECTORS=… sh plan/ws335/tests/run-unit.sh --win32 all` | test と ct の 57 本が PASS（win32）、`run-unit: PASS` |
| `BASE=ddc678f690 sh plan/ws335/tests/check-skeleton.sh` | #11 の前は `check-skeleton: PASS`。#11 の後は `check-skeleton: FAIL`、FAILED は凍結の差の 1 つだけ: `userland/base/libcrypto-compat/cryc-chain.h`（16 行、Q1 が承認した 2 つの comment）。他の check は通る。BASE は Q1 が移す |
| `sh plan/ws335/tests/run-interop.sh` | `run-interop: PASS (139 cases, 0 of them known limits)` |
| `sh plan/ws335/tests/run-interop.sh --win32` | `run-interop: PASS (47 cases, 0 of them known limits)` |
| `sh plan/ws335/tests/run-limbo.sh` | `run-limbo: PASS` |
| #11 の後の回帰（`run-unit.sh all`・`--win32 all`・`run-interop.sh` host と `--win32`・`run-limbo.sh`・`run-ct.sh all`） | 57 本・57 本 PASS、`run-interop: PASS (139 cases, …)`・`(47 cases, …)`、`run-limbo: PASS`、`run-ct: PASS` |
| `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p034 build/p034/dynamic/libssl-compat.so`（#11 の前） | rc 0、warning 0、check-exports PASS |
| わざと壊す（写しから戻し cmp で確かめた） | iPAddress の mask の修正を外す → test-name 3 件 FAIL。close_notify の後の送りを戻す → test-conn FAIL。SSL_connect の FAILED の確かめを外す → test-api-ssl 5 件 FAIL。1.2 の CCS の確かめを外す → test-hs13 3 件 FAIL |

未実施: 実の Windows の機械（#4 の確かめ）、aarch64 の ct（host は x86_64 だけ。primitive の review は aarch64・armv7・riscv64・i386 の clang の assembly の分岐の数を数えた）、LTO の build での ct。

消してほしい path（worktree の中）: `build/p034`、`build/tmp/p034-*`・`build/tmp/p034b-*`、`build/tmp/ws335-*`（ct・ct-matrix・fuzz・unit・interop・limbo・skeleton の run）、`build/tmp/ws335-fuzz-corpus` は次の fuzz の始まりに使えるので残すかは Q1 の判断。
