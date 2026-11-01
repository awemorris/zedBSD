<!-- awesome-plan project=zedbsd record=ws335-p028 -->

# ws335-p028: handshake の message の encode と syntax の確かめ

Status: cleared（2026-11-02 02:34 Q1、main で test-msg PASS。拡張の alert の分けは p035。依存: p025）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-msg.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 4（4.1.2〜4.6.3）、4.2（拡張）、B.3。
  - `rfc5246.txt`（SHA-256 `2ebe1b8e651696aeacc8ab3848bedb465505737901ea3b691acf40c392958feb`）: 7.4。
  - `rfc8422.txt`（SHA-256 `cac45754399e5115342cf1d773a51ac5a77ad221e9336e43ceadd8f294712350`）: 5。
  - `rfc6066.txt`（SHA-256 `6a33cceaa5e1b6518923d9d04ffaa738bad0417806b3c94a995484e140f5cbc0`）: 3。
  - `rfc7627.txt`（SHA-256 `3e908b1a511f5894b64b2c7867c5e0e8db43896add4d99a340fe67a07ee11a59`）: 5。
  - `rfc5746.txt`（SHA-256 `fb894a464bffd123ef7be802b63d7309b7af2bc34091683873c311556de2036d`）: 3。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: rfc8448。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-msg.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-msg.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- RFC 8448 の ServerHello・EncryptedExtensions・Certificate・CertificateVerify・Finished を parse。ClientHello の writer は RFC 8448 の ClientHello と同じ内容を与えれば同じ bytes になるかを、扱う拡張の部分で確かめる（RFC 8448 の ClientHello は libssl-compat の送らない拡張を含むので全体一致は求めない。違いの拡張の一覧を結果に）。
- 壊した拡張の alert（decode_error・unsupported_extension・illegal_parameter）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh msg     # この module
sh plan/ws335/tests/run-unit.sh --win32 msg     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 02:30 JST）Sonnet 5.5（q1893-i01）。worktree s-ws335-p028（main e500ece243）。

作った物: `userland/base/libssl-compat/sslc-msg.c`、`plan/ws335/tests/unit/test-msg.c`。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh msg` | `test-msg: PASS (936784 checks)` / `run-unit: PASS`（ASan・UBSan） |
| `sh plan/ws335/tests/run-unit.sh --win32 msg`（x86_64、Wine、static link） | `test-msg: PASS (936784 checks) (win32)` / `run-unit: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `sh plan/ws335/tests/run-unit.sh all` | test-* 19 本と ct-* 11 本が全部 PASS（FAIL 0）/ `run-unit: PASS` |
| `git diff --check` | 出力なし |

`run-ct.sh` は未実施（この module に秘密を扱う関数は無く、定数時間の対象は無い）。

### 実装の中身

- 書く側: ClientHello の writer は header の順（server_name、supported_groups、〔1.2〕ec_point_formats・extended_master_secret、signature_algorithms、〔1.3〕supported_versions、cookie、〔1.3〕key_share）。cipher_suites の後ろに〔1.2 を出す時〕SCSV 0x00ff。Finished・空の Certificate・ClientKeyExchange・KeyUpdate の writer。`sslc_writer_*`（p025）の上に書き、最後に `sslc_writer_finish` の値と `used` を返す（失敗なら `*written` は 0）。
- 読む側: `sslc_reader_*` の上に、各 message の parse。誤りは `sslc_msg_fail` で alert と errno を同時に決める（EINVAL は decode_error、EPROTO は unsupported_extension・illegal_parameter・bad_certificate）。
- 表の値は定数 7 つ（最大の長さ、session id の上限、null 圧縮、host_name、uncompressed、named_curve、KeyUpdate の 2 値）で、block ごとに「Values referenced: RFC …」。

### 試験の中身（`test-msg`、936784 checks）

- 期待の値: 手で書いた 2 つの ClientHello の全 byte（TLS 1.3 only の 112 byte、TLS 1.2 only で名前 "a.b" ありの 93 byte。RFC 8446 4.1.2・4.2、RFC 5246 7.4.1.2、RFC 8422 5.1、RFC 6066 3 から）、RFC 8448 の各 message、試験の中に別に書いた model（writer は試験側の builder、parser は KeyUpdate・CertificateVerify・ServerKeyExchange・Certificate（1.2・1.3）・EncryptedExtensions の 5 つの model）。他の実装の出力は使っていない。
- RFC 8448 の ServerHello・HelloRetryRequest（section 3、5 の 2 つ、6、7。random・session id・suite・版・group・key（RFC が別に印刷する鍵と一致）・cookie 114 byte・seen の組）、EncryptedExtensions（5 つ。どれも record_size_limit を持つので `unsupported_extension` で断り、除いた形は通る）、Certificate（5 つ。DER の SEQUENCE の長さが一致）、CertificateVerify（5 つ。scheme と署名の長さ）、Finished（5 つ。writer が byte 単位で一致）、NewSessionTicket、CertificateRequest。各 message の全ての短い prefix は誤り（ServerHello だけは固定部の直後の切れが TLS 1.2 の「拡張なし」で正しい）。
- ClientHello の writer を RFC 8448 の 5 つ（section 3、5 の 2 つ、6、7）と照合: random・session id・suite・compression が同じ byte、送る拡張（server_name・supported_groups・signature_algorithms・supported_versions・key_share・cookie）が RFC と同じ byte、RFC にあって送らない拡張の一覧（下）が一致、RFC に無い拡張は送らない。
- writer: 3 つの版の組 × 名前 5 通り（無し・1 文字・通常・253 文字・数字）× session id 4 通り × cookie 6 通り（0・1・2・255・256・1024）× share 数 0〜2 を model と byte 一致、各条件の EINVAL（版・session id 33・suite/group/scheme の無し・NULL・32767 個・share の 3 個目・版なしの share・key の 0 と NULL と 65536・cookie の版なし・1025・NULL・名前の 254 文字・空・NULL の hello・誤りが room 0 より先）、room 0 から size-1 の全部が ENOBUFS（`*written` は 0）、ちょうどの room で成功、room が大きい時に後ろを触らない（canary）、suite 32766 個（SCSV で 65534 byte）の上限、group・scheme の 30000 個、key の 1 byte と 60000 byte、拡張 block が溢れる大きさは EINVAL。
- parser の表: ServerHello 73 件（固定部の切れ、session id、block の長さ、各拡張の syntax と重複、unsupported_extension 12 種、illegal_parameter、先に見つけた誤りが決める順）。cookie の 1024 と 1025、session id の 0〜32、HelloRetryRequest の 3 拡張の 6 通りの順、random を 1 byte だけ変えると通常の ServerHello。EncryptedExtensions 約 55 件（model と照合）、Certificate は 0〜17 個 × 6 つの大きさ × 両方の版（16 個まで通り 17 個は bad_certificate）、entry の拡張の 11 通りの大きさ、context 0・1・255、2^24-1 byte の list と 2^24-1 byte の Finished（16 MiB の block）、CertificateVerify の長さ（0・1・境・65535）、ServerKeyExchange の curve_type 256 通り・point 0〜255・signature 0〜65535・全ての cut、CertificateRequest 26 件（両方の版）、NewSessionTicket 15 件と ticket 65535・拡張 65534 通り・65535 は誤り、KeyUpdate の全 256 値と長さ 0〜3。
- 変異と乱数: 14 の seed を各 2500 回、1〜3 か所を変えた物（flip・set・cut・挿入・削除）と、乱数の長さ・内容の 6000 入力を、全 parser に渡す。heap の exact-size の block（ASan で境の外の読みを検出）。status は 0・EINVAL・EPROTO だけ、alert と矛盾なし、view が body の内側、cookie・session id の上限、model のある物は status・alert・全 field が一致。

### RFC 8448 の ClientHello と writer の違い（送らない拡張）

writer は `supported_versions` を最高の版だけで出し、他の拡張は RFC と byte 一致。RFC の ClientHello にあって writer が送らない拡張（RFC の出現順）:

| section | 送らない拡張 |
| --- | --- |
| 3 | renegotiation_info（0xff01）、session_ticket（0x0023）、psk_key_exchange_modes（0x002d）、record_size_limit（0x001c） |
| 5（1 つ目）・6・7 | renegotiation_info、psk_key_exchange_modes、record_size_limit |
| 5（2 つ目、cookie あり） | 上の 3 つと padding（0x0015） |

拡張の並びも違う（RFC は server_name の次に renegotiation_info、key_share が supported_versions の前、など）。writer は header の順。

### わざと壊す確かめ

`build/tmp/p028/brk.py`（git の外。実装の copy を 1 か所だけ書き換え、test-msg と他の module の object（ASan・UBSan の `-O1`）で build して流す。実装 file は触らず、前後の sha256 が変わっていない事を script が確認）。153 通り（header の読み、定数、writer と入力の検査、ServerHello と各拡張、EncryptedExtensions、Certificate、CertificateVerify、ServerKeyExchange、CertificateRequest、NewSessionTicket、KeyUpdate、小さい writer、補助関数）。

- 146 通りが試験の失敗: 140 通りが `FAIL`（check の不一致）、6 通りが sanitizer・exit で落ちた（例: 17 個目の証明書を許す変更が `der[16]` の境の外書きで ASan が止める）。**build の誤りは 0、BAD-MUTATION（書き換えが当たらない）は 0**（変更した後に当たらなくなった 1 件は書き換えの対象を直して再実行し、FAIL を確認）。
- 7 通りが見逃し。全部、同じ結果になる等価な変更（検査が二重になっている）と判断した: (1) `group_count`・`scheme_count` の上限の `>` を `>=`（32766 個はどちらも EINVAL: 拡張 block が 2^16-1 byte を越える）、(2) share の key の上限 `> 65535` を `>=`（65535 byte はどちらも EINVAL: 同じ理由）、(3) Certificate の entry の拡張の `size != 0` を `> 1`（1〜3 byte は先の syntax の検査が decode_error にし、4 byte 以上はどちらも unsupported_extension）、(4) Finished の `verify_data == NULL && size != 0` の検査、空の Certificate の `context == NULL && size != 0` の検査、ClientKeyExchange の `point == NULL` の検査を外す（その後の `sslc_writer_bytes` が NULL を EINVAL にする。試験は NULL の入力が EINVAL である事を確認している）。
- 最初の実行では 11 通りが見逃しだったので、試験に足した: 2^24-1 byte の Finished と Certificate、list・share の key の境、HelloRetryRequest の random の 1 byte 違い（32 通り）。

### 自分で決めた点（header が黙っていた所）

1. 誤りの順: 拡張を読む順に、最初に見つけた誤りが alert を決める（unsupported_extension の拡張が先にあれば、後ろの syntax の誤りは見ない。逆も同じ）。
2. ServerHello・HelloRetryRequest で「許す」の外にある拡張は全部 unsupported_extension（EPROTO）にした。header の「any other」に従った。RFC 8446 4.2 は、知っていて、その message に指定が無い拡張（例: ServerHello の cookie）を illegal_parameter とも書いている（どちらも EPROTO の fatal）。**header の文面を優先したので、alert の違いは Q1 が見て決める**（p029 の handshake が送る alert の値）。
3. HelloRetryRequest の判定は random の 32 byte が `sslc_tls_hrr_random` と等しい事。key_share は HelloRetryRequest なら group の 2 byte だけ、ServerHello なら group と key（1 byte 以上）。TLS 1.2 の ServerHello に key_share があっても syntax だけ見る（意味は handshake）。cookie は HelloRetryRequest だけ許し、上限 1024 を越えると illegal_parameter。renegotiation_info は view（空でも非 NULL）。拡張 block が長さ 0 でも通す（TLS 1.3 の最小 6 byte は強制しない: supported_versions の欠けは handshake が見る）。
4. parse の出力: 呼ぶ時に struct を 0 で埋め、`*alert` を 0 にする。誤りの時の struct の中身は不定とみなす。
5. EncryptedExtensions の supported_groups は list の長さ 2 以上の偶数。Certificate の context は TLS 1.2 では NULL・0。空の list は decode_error（1.2 でも）。17 個目の entry を読む前に bad_certificate。TLS 1.3 の entry の拡張は、syntax が壊れていれば decode_error、整っていて空でなければ unsupported_extension。
6. CertificateRequest: 1.3 は context と拡張の syntax（拡張の重複と signature_algorithms の有無は見ない: header の「syntax only」）。1.2 は certificate_types 1 byte 以上、algorithm の対 2 byte 以上の偶数、authorities の各 name 1 byte 以上。
7. NewSessionTicket の拡張も syntax を見る（block の上限は 2^16-2）。
8. writer の EINVAL: header の 4 つに加え、版が TLS 1.2・1.3 以外、session id が 33 byte 以上、groups・schemes が空・NULL、suites・groups・schemes が 32766 個を越える（2 byte × 個数と SCSV が list の長さに入る）、share の key が空・NULL・65535 を越える、share が 1.3 なしで、cookie が 1.3 なし・NULL・1024 を越える、server_name が空、NULL の hello。1.3 で share が 0 個の key_share（再試行を求める形）は許す。server_name の byte の中身（ASCII か）は見ない（呼び出し側が normalize 済み）。検査は room の前（誤った入力は room 0 でも EINVAL）。拡張 block の全体が 2^16-1 byte を越えると、writer の終わりで EINVAL（長い group の list や 65529 byte 以上の key）。
9. Finished の verify_size は 0〜2^24-1（0 も書く）。空の Certificate の context は 0〜255 byte。ClientKeyExchange の point は 1〜255 byte。KeyUpdate は 0・1 以外が EINVAL。
10. 試験に使った RFC 8448 の ClientHello は、読み取った field（random・suite・group・scheme・key・名前・cookie）を writer に渡す形（RFC の ClientHello の bytes そのものを writer に与えるのでなく、parse した値からの再構築）。

### header と phase.md の食い違い・持ち越し

- phase.md と仕様の食い違い: 見つからなかった。
- header の comment（規約の見直しの Phase に持ち越し）: `sslc_msg_parse_server_hello` の「any other is unsupported_extension」が RFC 8446 4.2 の illegal_parameter と違う（上の 2）。`sslc_msg_write_client_hello` の EINVAL の一覧は 4 つだが、実際は上の 8 のとおり。

### 未実施

- `run-ct.sh`（対象無し）、32 bit の size_t の機械・aarch64 Windows の実行（build だけ、check-skeleton）、実機・QEMU（範囲外）、RFC 8448 の section 4（0-RTT、pre_shared_key）の ClientHello の writer との照合（送らない拡張が多く、この client は PSK を送らない）。

commit: 下の「commit」に Q1 への報告のとおり。
