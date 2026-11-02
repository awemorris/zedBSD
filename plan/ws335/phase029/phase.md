<!-- awesome-plan project=zedbsd record=ws335-p029 -->

# ws335-p029: handshake の共通と TLS 1.3

Status: cleared（2026-11-02 11:25 Q1）。前: 実装済み・試験 PASS（2026-11-02 P6、Opus、q1928-i01。cleared は Q1）。前: in-progress（2026-11-02 10:10 P6、Opus）。前: planned（2026-11-01 p001 が作成。起動は Q1。依存: p027、p028、p020、p011、p008、p037）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（重い。Q1 の判断で Opus）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-hs.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 2、4、6、7、付録 A.1、D.4。
  - `rfc8448.txt`（SHA-256 `6564d1376d1ec744fc7a9993da15ebc1b9be361908b166091f47ef605c537fba`）: 3、5。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: rfc8448。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-hs.c`、`userland/base/libssl-compat/sslc-hs13.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-hs13.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 試験の相手は [p037](../phase037/phase.md) の `tests/unit/tlspeer.h`（試験の側の scripted な TLS server）。RFC 8448 の full の trace は使わない（RFC 8448 の ClientHello は libssl-compat の送らない拡張を含み、server の鍵は RSA-1024 で CRYC_RSA_MIN_BITS に届かないため、transcript も CertificateVerify も一致しない。RFC 8448 の値は p026〜p028 が使う）。
- tlspeer で: 1-RTT（x25519、P-256 の HRR、ECDSA P-256 と RSA 2048 の鍵）、HRR の cookie、2 回目の HRR、downgrade の印、unsolicited な拡張、壊した Finished・CertificateVerify、ServerHello と EncryptedExtensions の同じ record（conn の試験だが hs の側の状態も確かめる）。固定の random の源で送る bytes が tlspeer の期待と一致すること。
- （2026-11-02 Q1 の判断）tlspeer は RSA の秘密鍵を持たない（p037）。上の「RSA 2048 の鍵」の server の場合は、ここでなく p033 の OpenSSL `s_server` との相互接続で確かめる。tlspeer の使い方は `tests/unit/tlspeer.h` の冒頭と [p037](../phase037/phase.md) の結果。
- 2 回目の HRR、downgrade の印、unsolicited な拡張の拒否。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- p037 の `plan/ws335/tests/unit/tlspeer.h`。全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh hs13     # この module
sh plan/ws335/tests/run-unit.sh --win32 hs13     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 10:10〜11:23 JST）P6（Opus、q1928-i01）。worktree p6-ws335d（main a6a0be590。worktree は 3416af409 だったので a6a0be590 へ fast-forward してから着手）。

作った物: `userland/base/libssl-compat/sslc-hs.c`、`userland/base/libssl-compat/sslc-hs13.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-hs13.c`。header・tlspeer.h・他の module の .c・試験の道具・Makefile は変えていない。`sslc-hs12.c` は骨格のまま（p030）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh hs13` | `test-hs13: PASS (18565 checks)` / `run-unit: PASS`（ASan・UBSan、leak 検出あり、約 50 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 hs13`（x86_64、Wine、static link） | `test-hs13: PASS (18565 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（test と ct の 53 本が全部 PASS、FAIL 0。test-hs13 18565 checks） |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

`run-ct.sh` は流していない（ct-hs13.c は作らなかった）。理由: handshake の秘密の計算は x25519・ECDH・HKDF・HMAC・`cryc_ct_equal` の中で行い、それぞれの module に ct 試験がある。この module 自身が秘密から作る値で分岐するのは Finished の比較の結果（`cryc_ct_equal` の答え、公開の結果）だけで、ctgrind の方式ではこの分岐が必ず報告され、試験として成り立たない。

試験の中身（test-hs13.c、tlspeer の message の使い方。client の SEND は message に分けて peer に渡し、SET_READ_KEYS は次の item の `peer->write_keys[item.epoch]` と、SET_WRITE_KEYS は `peer->client_handshake_keys`・`client_application_keys` と比べる）:

- ClientHello: 固定の乱数の源（SHA-256 の stream）で、版の 3 通り（1.2〜1.3、1.3 だけ、1.2 だけ）× SNI の有無の 6 通りについて、sslc-msg.h の順と RFC 8446 4.1.2 の構文から試験の側で書いた期待の bytes と byte ごとに一致。乱数の引く順（random 32、session id 32（1.3 を申し出る時だけ）、x25519 32）と回数、同じ源で同じ bytes・別の seed で別の random。HRR の後の 2 回目の ClientHello も期待の bytes（同じ random・session id、secp256r1 の share だけ、cookie）と一致。
- 完全な handshake（connected、peer も CONNECTED、鍵が全部一致、client の CCS がちょうど 1 回、action の順 `S` `R` `CWSWRD`、HRR の時 `S` `CS` `R` `WSWRD`、handshake の秘密が消えて application の秘密が残る）: 3 つの suite、P-384 の leaf、1.3 だけの client、server の CCS、SNI の ack、EE の supported_groups、CertificateRequest（空、3 byte、255 byte の context、extension 付き）、SNI 無し、HRR（secp256r1、cookie つき、AES-256-GCM、最大 1024 byte の cookie だけの HRR、HRR の後の CertificateRequest と P-384）。
- tlspeer の壊し方: FINISHED・SIGNATURE（P-256・P-384）→ decrypt_error、SECOND_HRR → unexpected_message、DOWNGRADE12・11 → illegal_parameter、1.2 の server と 1.3 だけの client → protocol_version、SESSION_ID・CERT_CONTEXT・UNSHARED・ZERO_SHARE（x25519、HRR の後の secp256r1）・1.3 の SH の 1.2 の suite → illegal_parameter、申し出ていない拡張（SH・HRR・EE・CertificateEntry）→ unsupported_extension、1.3 の SH・HRR の EMS と server_name → illegal_parameter、EE の EMS → unsupported_extension、SNI を送らない client への ack → unsupported_extension、壊れた EE → decode_error。
- CertificateVerify（cv_raw）: 申し出ていない scheme・RSA PKCS #1・EC 鍵への RSA-PSS・P-256 鍵への P-384 の scheme（と逆）→ illegal_parameter、DER でない・空の署名 → decrypt_error、短い → decode_error、CV の場所の Finished → unexpected_message。
- 証明書の判定: 空の store（unknown_ca、verify_peer 0 なら続けて verdict を記録、callback 1 で続行・0 で unknown_ca）、正しい鎖での callback（preverified 1、0 で certificate_unknown）、期限切れ・まだ有効でない（certificate_expired）、host の不一致（bad_certificate）、root だけ（DEPTH_ZERO_SELF_SIGNED）・root を鎖に足して信頼しない（SELF_SIGNED_IN_CHAIN、どちらも unknown_ca）・信頼すれば通る、ecdsa-with-SHA224 に書き換えた leaf（unsupported_certificate）、扱えない名前付き曲線の leaf を verify_peer 0 で（ENOTSUP unsupported_certificate）、parse できない DER（bad_certificate）。
- 手で作った ServerHello・HRR で header の確かめ全部（legacy_version、supported_versions の値と申し出、1.3 だけの client への SV 無し、suite の申し出と版、compression、session id の echo、key_share の欠如・未送の group・長さ・零点、TLS 1.2 の拡張、cookie、downgrade の印 2 つ、1.2 の SH の 1.3 の suite）、HRR の確かめ（申し出ていない group、送り済みの group、変える物の無い HRR、許されない拡張、session id・suite・compression、SV 無し）、HRR の後の SH（別の suite、古い group、SV 無し、曲線外の点）、HRR での secp256r1 の鍵の生成の失敗（EIO、alert 0）。
- 状態: 各状態で flight の他の全ての message が unexpected_message（20 通り）、CCS は ServerHello の前から Finished の前まで捨てる（40 回でも）・接続の後は unexpected_message、ServerHello の前の HelloRequest（1.2 を許す時は無視、body 付き・長さの食い違いは decode_error、1.3 だけなら unexpected_message）、header の長さの食い違い（長い・短い）・3 byte（heap の 3 byte で ASan が見る）・NULL・上限超え、始めていない handshake、失敗の後は全部 EPROTO で alert を保つ、hash の長さより 1 byte 長い Finished → decode_error。
- 接続の後: NewSessionTicket（捨てる）、KeyUpdate（要求なし → `R`、要求あり → `RSW` で peer が client の KeyUpdate を受けて鍵が一致、連続 2 回）、不正な値・長さの KeyUpdate、HelloRequest、壊れた ticket。
- 内部の口: queue_message の併合・CCS で切れる・first_hello・2048 byte の上限・ENOBUFS・EINVAL、queue_action の鍵の複写・上限、shared_secret（RFC 7748 6.1 の値、CAVP KAS ECC CDH P-256 COUNT 0、長さ違い・零点・鍵の無い secp256r1・申し出ていない group・曲線外・圧縮・短い・無限遠）、sslc_hs_certificate の 2 回目・context・切れた body。
- 切った・変えた message: 平らな flight と HRR の flight の各 message を全ての長さで切る（header を合わせる・合わせない）、全 byte の 1 bit 反転（HRR の flight は 3 byte ごと）、乱数の message 200 通り。どれも完了せず、ASan・UBSan で落ちない。

わざと壊す確かめ（`build/tmp/p029-mut/`、`mut.py` が sslc-hs.c・sslc-hs13.c の写しを 1 か所ずつ書き換え、他の module は 1 度 build した object（sanitizer 無し -O1）、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB か 150 秒で kill・4 並列。元の file の SHA-256 が前後で同じことを確かめた）: 120 通り（定数、比較の向き、境界、条件の反転・削除、transcript への吸収の抜け、鍵の取り違え、alert の取り違え、消去の抜けなど）。最初は 105 通りが FAIL、残り 15 通りのうち 7 通りに試験を足し（長い Finished、header の長さが短い HelloRequest、各状態での前の message、HRR の後の正しい点での suite の違い、HRR の後の x25519 の消去、鍵の無い secp256r1、未知の group の alert）、最後は 112 通りが FAIL。sanitizer 無しで残る 2 通り（3 byte の message の読み越し、ec の解放の抜け）は run-unit と同じ ASan で build して heap-buffer-overflow・LeakSanitizer で落ちることを確かめた。残る 6 通りは観測できる違いが無い（等価）: config の min > max と min の範囲（sslc_msg_write_client_hello が同じく EINVAL）、SV の 1.3 が申し出られたかの確かめ（1.3 の suite は申し出られていないので suite の確かめが同じ alert で止める）、HRR の後の版の確かめ（SV の無い SH は suite の確かめで必ず止まる）、suite の申し出の確かめ（扱える suite は許した版の分を全部申し出る）、CV の scheme の申し出の確かめ（switch に無い scheme は default で同じ alert）。どれも防御として残した。

自分で決めた点（header が黙る所）:

1. sslc-hs.c と sslc-hs12.c の境: ServerHello のうち両方の版の確かめ（legacy_version、版の選択、downgrade の印、suite の申し出と版、compression）と TLS 1.3 の SH の確かめ・HRR は sslc-hs.c が行い、transcript の hash の選択と SH の吸収も両方の版で sslc-hs.c が済ませてから `sslc_hs13_server_hello`・`sslc_hs12_server_hello` を呼ぶ。TLS 1.2 だけの確かめ（renegotiation_info、EMS、session id の echo、SV・key_share・cookie の illegal_parameter）は sslc_hs12_server_hello（p030）に残した。
2. ServerHello の前の HelloRequest: 版が未定（HRR の前）で TLS 1.2 を許す時だけ無視（body は空、でなければ decode_error）。それ以外は unexpected_message。
3. change_cipher_spec: WAIT_SERVER_HELLO（TLS 1.2 だけの client も、header の文のとおり）から HS13_WAIT_FINISHED までは捨てる。HS12_WAIT_CHANGE_CIPHER_SPEC は sslc_hs12_change_cipher_spec。他は unexpected_message。
4. client の handshake の書く鍵は ServerHello の時でなく server Finished の後の flight で、CCS の後に入れる（D.4: 最初の保護された record の前に CCS）。flight の action は `SEND_CCS`（HRR で送っていなければ）・`SET_WRITE_KEYS`（client handshake）・`SEND`（空の Certificate と Finished を 1 つに併合）・`SET_WRITE_KEYS`（client application）・`SET_READ_KEYS`（server application）・`DONE`。ServerHello の後は `SET_READ_KEYS`（server handshake）だけ。そのため ServerHello の後に client が送る alert は平文になる（tlspeer は平文の alert を受ける。相互接続は p033 で）。
5. session id の echo は HRR にも当てる（RFC 8446 4.1.4 は SH と同じ確かめを求める）。HRR に許すのは SV・key_share・cookie だけで、他の知っている拡張は illegal_parameter（4.2 の表）。SV の無い HRR は TLS 1.2 の SH として扱い、その 1.3 の suite で illegal_parameter。
6. HRR の後の SH に SV が無い時: TLS 1.2 を許すなら illegal_parameter（4.1.4 同じ版）、許さないなら protocol_version。
7. 誤りの値: 証明書の判定で止める時 EPROTO（alert は判定から）、DER の parse の失敗 EINVAL・bad_certificate、leaf の鍵が RSA でも EC でもない時 ENOTSUP・unsupported_certificate（判定の後に確かめるので verify_peer 0 でも止まる）。CV の `cryc_signature_verify` の EBADMSG → decrypt_error、ENOTSUP → unsupported_certificate、EINVAL（鍵の形が壊れている、例えば verify_peer 0 での曲線外の点）→ bad_certificate。内部の誤り（ENOMEM、ENOBUFS、乱数の EIO、鍵の schedule の EINVAL）は alert 0。
8. 失敗の時: 状態を FAILED にし、actions を消し、秘密鍵・schedule・master を消す。後の呼び出しは EPROTO で alert は最初の物のまま。`sslc_hs_free` は構造体の全体を消して状態を FAILED にする（0 にした構造体にも 2 回でも安全）。`sslc_hs_start` は最初に構造体を 0 にする（使い回す前に free が要る）。config の EINVAL: 版の値、min > max、random が NULL、store が NULL。
9. 秘密の消去の時: x25519 の秘密鍵は ServerHello で（secp256r1 を求める HRR ではその時）、secp256r1 の秘密鍵は ServerHello で、Master Secret と handshake の traffic secret は Finished の後に消す。application の traffic secret は KeyUpdate のために残す。共有の秘密・traffic keys・verify_data の local は使った後すぐ消す。
10. hs->out の使用量は今の呼び出しの SEND の action の終わりから求める（header に field が無いため）。first_hello は「ClientHello の型で HRR の前」、first_hello の SEND には何も併合しない。KeyUpdate の答え（update_not_requested）も queue_message で送るので transcript に入るが、接続の後の transcript は使わない。

header の持ち越し（p035 へ、Q1 の判断の適用）:

- Q1 の判断（RFC 8446 4.2: 知っていてその message に指定の無い拡張は illegal_parameter、送っていない拡張は unsupported_extension）を p035 で sslc-msg に当てると、test-hs13 の次の 2 か所の期待が変わる: 1.3 の SH の cookie（今は sslc-msg が unsupported_extension、`test_server_hello`）と EE の extended_master_secret（今は unsupported_extension、`test_breaks`）。header の SH の文（cookie は illegal_parameter）と sslc-msg の今の動きの食い違いはこの 1 点。
- p027 の注意（schedule は段を検出しない）: sslc_tls13_start → sslc_tls13_handshake（ServerHello）→ sslc_tls13_master（server Finished）の順は状態機械が守る（各関数は 1 つの状態からしか呼ばれない）。

未実施・残り:

- TLS 1.2 の道（sslc-hs12.c は p030）。test-hs13 は 1.2 の ServerHello を downgrade の印・suite・版の確かめまでしか試していない。
- record の境（ServerHello と EncryptedExtensions の同じ record、KeyUpdate の後の同じ record）は接続の試験（p031）。hs の側は ServerHello の後に `SET_READ_KEYS` だけを返して WAIT_ENCRYPTED_EXTENSIONS になることを確かめた。
- RSA の server（RSA-PSS の CertificateVerify）は tlspeer に RSA の秘密鍵が無いので p033 の s_server。
- Windows の aarch64 は check-skeleton の compile と link だけ。

消してよい物（Q1 に頼む）: `build/tmp/p029-mut/`（わざと壊す確かめの script・写し・object・log・ASan の build）、`build/tmp/ws335-unit.run.*` のうち今の symlink が指さない物（run-unit の古い run）。

- 注意（2026-11-02 Q1、p027 の結果から）: struct sslc_tls13_schedule は段を表す field を持たず、sslc_tls13_handshake・sslc_tls13_master を順どおりに呼んだかを検出しない。順を守るのは p029（handshake の状態機械）の責任。header は変えない。

