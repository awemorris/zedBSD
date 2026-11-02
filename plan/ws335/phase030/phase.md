<!-- awesome-plan project=zedbsd record=ws335-p030 -->

# ws335-p030: TLS 1.2 の handshake

Status: 実装済み・試験 PASS（2026-11-02 12:20 Sonnet q1937-i01。cleared は Q1。依存: p029・p037）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-hs.h（1.2 の関数）`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc5246.txt`（SHA-256 `2ebe1b8e651696aeacc8ab3848bedb465505737901ea3b691acf40c392958feb`）: 7.3、7.4、8.1。
  - `rfc8422.txt`（SHA-256 `cac45754399e5115342cf1d773a51ac5a77ad221e9336e43ceadd8f294712350`）: 5。
  - `rfc7627.txt`（SHA-256 `3e908b1a511f5894b64b2c7867c5e0e8db43896add4d99a340fe67a07ee11a59`）: 4、5。
  - `rfc5746.txt`（SHA-256 `fb894a464bffd123ef7be802b63d7309b7af2bc34091683873c311556de2036d`）: 3.4。
  - `rfc5288.txt`（SHA-256 `f68375c3f1948802240ca5b21f3aa44311ea02f877e4e240d1fc70927ecf1307`）: 3。
  - `rfc7905.txt`（SHA-256 `afa1e233822c6bda1f1ced014d32e6d740454415b53130fa84630a9c5ad2fe69`）: 2。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-hs12.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-hs12.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 試験の相手は p037 の tlspeer.h（TLS 1.2 の ECDHE の server の flight と、試験の側の ECDSA の署名器）。
- EMS と RI の echo、downgrade の印。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- p037 の `plan/ws335/tests/unit/tlspeer.h`。全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh hs12     # この module
sh plan/ws335/tests/run-unit.sh --win32 hs12     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 11:25〜12:20 JST）Sonnet 5.5（q1937-i01、phase-runner-low）。worktree s-ws335-p030（最初に main c3f079afe へ fast-forward。p029 の merge の後）。

作った物: `userland/base/libssl-compat/sslc-hs12.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-hs12.c`。header・tlspeer.h・他の module の .c・試験の道具・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh hs12` | `test-hs12: PASS (17451 checks)` / `run-unit: PASS`（ASan・UBSan、leak 検出あり） |
| `sh plan/ws335/tests/run-unit.sh --win32 hs12`（x86_64、Wine、static link） | `test-hs12: PASS (17451 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（54 本が全部 PASS、FAIL 0。test-hs12 17451、test-hs13 18565 checks） |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS` |
| `git diff --check` | 出力なし |

`run-ct.sh` は流していない（ct-hs12.c は作らなかった）。理由は p029 と同じ: この module が秘密から作る値で分岐するのは Finished の比較の結果（`cryc_ct_equal` の答え、公開の結果）だけで、秘密の計算は hmac・prf・ecdh・x25519 の ct 試験の中。

試験の中身（test-hs12.c、server は tlspeer の message mode。client の SEND は message に分けて peer に渡し、SET_WRITE_KEYS は `peer->client_application_keys`、SET_READ_KEYS は次の item の `peer->write_keys[epoch]` と比べる。EMS と master・key block・Finished が食い違えば peer が失敗する）:

- 完全な handshake 24 通り: 3 つの suite（ECDHE_ECDSA の AES-128-GCM・AES-256-GCM・ChaCha20）× 2 つの group（x25519・secp256r1）× P-256・P-384 の leaf × client が 1.2 だけ・1.2 と 1.3、server の SNI の ack・point format・CertificateRequest（空の Certificate で答える）・EMS 無しを番号で変える。action の順 `S`（ClientHello）`S`（CKE）`C` `W` `S`（Finished）`R` `D`、message の数、peer の ems と client の ems の一致、鍵の一致、CCS がちょうど 1 回、key pair と master secret が消えている。ECDSA の scheme の hash と鍵の曲線は独立（P-384 に SHA-256、P-256 に SHA-384）。SNI 無し。
- 乱数の引く順: 1.2 だけは random 32・x25519 32 の 2 回、1.3 も申し出る時は session id 32 を足して 3 回、secp256r1 の server は ServerKeyExchange の後に keygen の draw を足す、x25519 の server は足さない（3 回目の失敗が EIO・alert 0 になるのは secp256r1 だけ）。
- tlspeer の壊し方: FINISHED → decrypt_error（client の flight は出た後）、SIGNATURE（P-256・P-384）→ decrypt_error（何も送る前）、DOWNGRADE12・11 → 1.3 も申し出る client は illegal_parameter・1.2 だけは接続、SESSION_ID → 1.3 も申し出る client は illegal_parameter（32 byte の echo）・1.2 だけは接続（空の echo は resumption でない）、ZERO_SHARE（x25519・secp256r1）→ illegal_parameter、NO_RI → handshake_failure、申し出ていない拡張（ALPN）→ unsupported_extension、ECDHE_RSA の suite に ECDSA の署名 → illegal_parameter、1.3 の suite → illegal_parameter。
- 手で作った ServerHello（1.2）: 取る形（EMS・point formats・SNI・suite 3 種・1.3 も申し出る client）と、RI の欠如・中身あり・TLS 1.3 の拡張（key_share）・SV（1.2 の値）・HRR の random（cookie 有無）・SNI を送っていない client への ack・cookie・申し出ていない拡張・session id の echo（whole、1 byte の違いを先頭と末尾で、prefix、空）。
- 手で作った ServerKeyExchange: curve_type が named_curve でない、offered でない group（secp384r1・0）、point の形（x25519 の 31・33・1 byte、secp256r1 の 32・64・66・先頭 2・先頭 0）、offered でない scheme（0x0603・0x0807・0x0201・0）、ECDSA の suite に RSA の scheme 6 つ、署名が検証されない（DER の形・DER でない・空）→ decrypt_error、切れた・後ろに byte。ECDHE_RSA の suite（RSA の leaf は無いので EC の leaf）に RSA の scheme（鍵の型）・ECDSA の scheme（suite との不一致）。
- 状態: 各状態（Certificate の前から接続の後まで）で 16 種の message type のうち受けない物は全部 unexpected_message、change_cipher_spec は WAIT_CHANGE_CIPHER_SPEC 以外の全ての 1.2 の状態と接続の後で unexpected_message。CertificateRequest（取る、2 回目は unexpected_message、空・切れは decode_error）、ServerHelloDone の body（decode_error）、Finished の長さ 0・11・13・32（decode_error）と 12 byte の各 byte の違い（decrypt_error）。
- flight の中身: action の並び（SEND・SEND_CCS・SET_WRITE_KEYS・SEND）と offset・size、ClientKeyExchange の bytes（x25519 は `10 00 00 21 20` と公開鍵、secp256r1 は 65 byte の点）、空の Certificate の 7 byte（CertificateRequest の時だけ、同じ SEND の先頭）、Finished の header、秘密の消去（x25519・p256 の秘密鍵は SHD の後に、master は server の Finished の後に）、CCS で SET_READ_KEYS の 1 つだけ。
- HelloRequest: server の flight のどの位置でも無視され transcript に入らない（handshake が完了する）、body 付きは decode_error、接続の後は `SEND_WARNING` no_renegotiation（100）を毎回、body 付きは decode_error。接続の後の他の message（NewSessionTicket・KeyUpdate・Certificate ほか）は unexpected_message。
- 証明書の判定（1.2 の道）: 空の store（unknown_ca、verify_peer 0 なら続ける）、callback の許可・拒否・有効な鎖への callback、期限切れ・host の不一致、root だけ・root を足した鎖、未対応の署名 algorithm の leaf（unsupported_certificate）、未対応の曲線の leaf、曲線の上にない公開鍵（verify_peer 0 で ServerKeyExchange の署名の確かめが bad_certificate）、parse できない DER、空の鎖（decode_error）。
- 切った・変えた message: 2 つの記録（平らな flight と、secp256r1・P-384・CertificateRequest の flight）の各 message を全ての長さで切る（header を合わせる・合わせない）、全 byte の 1 bit 反転（2 つ目は 3 byte ごと）、乱数の message 200 通り × 2。どれも完了せず、ASan・UBSan で落ちない。

わざと壊す確かめ（`build/tmp/p030-mut/`、`mut.py` が sslc-hs12.c の写しを 1 か所ずつ書き換え、他の module は 1 度 build した object（sanitizer 無し -O1）、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB か 150 秒で kill・4 並列。元の file の SHA-256 が前後で同じことを確かめた）: 166 通り（定数、比較の向き、境界、条件の反転・削除、transcript への吸収の抜け、鍵・random の取り違え、alert の取り違え、label、消去の抜けなど）。最初は 165 通りのうち 136 通りが FAIL で、「曲線の上にない鍵」の 2 通りに試験を足し、何も変えない 1 通りを除き、redundant な守りを 2 つ同時に外す 2 通りを足して、最後は 166 通りのうち 140 通りが FAIL・26 通りが生き残り（redundant な守りを 2 つ同時に外した 2 通りは FAIL する）。生き残りの内訳:

- RSA の道（13 通り）: ServerKeyExchange の RSA-PSS・PKCS #1 の algorithm・salt の対応 9 通り、RSA の scheme の auth の取り違え 2 通り、PKCS #1 の case の削除 1 通り、ENOTSUP の alert の 1 通り（RSA の鍵の大きさの範囲）。tlspeer は RSA の秘密鍵を持たないので、p033 の s_server との相互運用で確かめる。
- redundant な守り（6 通り）: scheme の offered の確かめ 3 通りと switch の default の受け入れ 1 通り（どちらか片方があれば同じ illegal_parameter。両方を外した 1 通りは FAIL）、forbidden の mask から supported_versions を外す（hs.c の版の選択が先に止める）、cookie を外す（cookie は HRR の random の時だけ parser が受け、その時は is_retry の確かめが先に止める。2 つとも外した 1 通りは FAIL）。
- 観測できない（7 通り）: local の premaster の消去の抜け 1、transcript hash の 0 の確かめ 3（hash は ServerHello の前でしか 0 にならない）、`sslc_hs12_make_p256` の 1 回だけの守りと ec の再確保（呼び出しが 1 回）2、queue_keys の ENOBUFS の alert 1（1.2 の flight の action は 4 つで上限の 8 に届かない）。どれも防御として残した。

自分で決めた点（header が黙る所）:

1. ServerHello（sslc_hs12_server_hello）の確かめの順: HRR の random（is_retry）→ supported_versions・key_share・cookie（illegal_parameter）→ renegotiation_info の欠如・中身あり（handshake_failure）→ SNI を送っていない client への server_name の ack（unsupported_extension、RFC 5246 7.4.1.4）→ session id の echo（illegal_parameter）→ EMS。HRR の random を持つ 1.2 の ServerHello は header に無いが、RFC 8446 4.1.3 が HRR 専用の値なので illegal_parameter。SNI の ack の確かめも header に無いが 1.3 の EncryptedExtensions と同じ理由（申し出ていない拡張）。
2. session id の echo は「空でなく、送った id と長さも bytes も同じ」時だけ（header の文のとおり）。server の独自の id（長さ・内容が違う、prefix、空）は取る（resumption は求めないので以後無視）。
3. EMS: server が echo しなければ EMS 無しで続ける（RFC 7627 5.2 は client に続けるか中止するかを許す。OpenSSL の既定の続ける側）。
4. HelloRequest: 1.2 の handshake の間（Certificate の待ちから server の Finished の待ちまで、server の CCS の前後とも）は body が空なら無視して transcript に入れない（RFC 5246 7.4.1.1）。ServerHello の前の無視は p029 の sslc-hs.c。body 付きは decode_error。接続の後は warning の no_renegotiation（100）、body 付きは decode_error。
5. ServerKeyExchange の確かめの順: 構文（parser）→ group が offered（x25519・secp256r1 の 2 つ）→ point の形（x25519 は 32 byte、secp256r1 は 65 byte で先頭 0x04）→ scheme（offered・suite の auth・鍵の型）→ 署名（decrypt_error、ENOTSUP は unsupported_certificate、EINVAL は bad_certificate）→ secp256r1 の key pair → 点の記録。曲線の上にあるかは shared secret の計算（ServerHelloDone）で `cryc_ecc_ecdh` が確かめる。形を署名の前に確かめるのは、署名の確かめ（重い）の前に明らかに壊れた点を断るため。
6. scheme の確かめ（header の「suite の authentication と鍵の型に合う」）: ECDSA の 2 つは EC の鍵なら曲線を問わず（TLS 1.2 では scheme は hash だけ、RFC 8422 5.1.3）、suite の auth が ECDSA。RSA-PSS rsae の 3 つと RSA PKCS #1 の 3 つは RSA の鍵、suite の auth が RSA、PSS の salt は hash の長さ。suite の auth の不一致を鍵の型の不一致より先に確かめる。
7. secp256r1 の key pair は ServerKeyExchange の署名が通った後に作る（header の「HRR か 1.2 の ServerKeyExchange が求めた時だけ」）。`sslc_hs_make_p256` は sslc-hs.c の static で呼べないので、同じ手順の `sslc_hs12_make_p256` を持つ（約 25 行の重複。header が口を出していないため）。
8. shared secret は ServerHelloDone で計算する（server の点は hs->server_group・server_point に写して持つ）。無効な点（x25519 の全零、曲線の上にない点）は ServerHelloDone の時に illegal_parameter。flight の action の順は `SEND`（Certificate があれば先頭に併合、ClientKeyExchange）・`SEND_CCS`・`SET_WRITE_KEYS`（client の鍵）・`SEND`（Finished、新しい鍵の下）。ClientKeyExchange の後に master secret（EMS は ClientKeyExchange までの session hash、でなければ 2 つの random）と key block を作る。
9. server の鍵は server の CCS の時に master secret から key block を作り直して `SET_READ_KEYS`（client の半分は捨てる）。client の flight の時に server の鍵を持ち越さず、hs に置き場所を足さない。master secret は server の Finished を確かめた後に消す（再開・再 handshake・exporter は無い）。private key は ServerHelloDone で shared secret を計算した後に消す。
10. 誤りの値: 構文は decode_error（EINVAL）、ServerHelloDone の body・Finished の長さ違いは decode_error、順序違い・来るはずのない message は unexpected_message（EPROTO）、署名の失敗は EBADMSG と decrypt_error、内部の誤り（ENOMEM、乱数の EIO、PRF の EINVAL）は alert 0。CertificateRequest は構文だけ確かめ（TLS 1.2 の context は無い）、2 回目は unexpected_message。
11. 試験の側: RSA の leaf は tlspeer・test の側に無いので、RSA の suite の確かめは EC の leaf を使った suite・scheme・鍵の不一致（illegal_parameter）まで。正の経路（RSA の署名の確かめ）は p033。

header の持ち越し: なし（header の不備は見つからなかった）。p029 の持ち越し（RFC 8446 4.2 の cookie は illegal_parameter）は 1.2 の ServerHello にも関わる: cookie は今 parser が HRR の random でない時 unsupported_extension で止める。test-hs12 はこの 1 点の alert を unsupported_extension か illegal_parameter のどちらでも通す（p035 で変わっても壊れない）。

未実施・残り:

- RSA の server（RSA-PSS・PKCS #1 の署名、ECDHE_RSA の suite）の正の経路は p033 の s_server（tlspeer に RSA の秘密鍵が無い）。
- record の境（server の CCS と Finished が同じ record、ServerHelloDone と CCS の前の flight の分け方）は接続の試験（p031）。hs の側は action の並びだけを確かめた。
- Windows の aarch64 は check-skeleton の compile と link だけ。

消してよい物（Q1 に頼む）: `build/tmp/p030-mut/`（わざと壊す確かめの script・写し 166 個・object・log）、`build/tmp/p030/`（all の log・hs12 の object）、`build/tmp/ws335-unit.run.*` のうち今の symlink が指さない物。
