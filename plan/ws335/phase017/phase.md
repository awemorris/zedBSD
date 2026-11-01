<!-- awesome-plan project=zedbsd record=ws335-p017 -->

# ws335-p017: X.509 の parse（証明書・公開鍵・署名の algorithm）

Status: 実装済み・試験 PASS（2026-11-02 02:36 Sonnet。cleared は Q1。依存: p016）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-x509.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc5280.txt`（SHA-256 `a2f2628c0a83b873fc4786abd921f9b2c02395954b655d190bf16b831633345d`）: 4.1、4.2、4.2.1.1〜4.2.1.3、4.2.1.6、4.2.1.9、4.2.1.10、4.2.1.12。
  - `rfc3279.txt`（SHA-256 `6d3f19f18e17fa1c68da5aaf4021327748fabca840d7300443b77357a1fc1614`）: 2.2.3、2.3.1。
  - `rfc5480.txt`（SHA-256 `593bf29fd0da2ff8b903c3ebf1c9d189a770039159e2ba46a0c3b91355037f26`）: 2。
  - `rfc4055.txt`（SHA-256 `b8a1ef3fb135c32aed4eee121264f3ec83a46def746c8fe68e05bd1b60324e9a`）: 3、5。
  - `rfc5758.txt`（SHA-256 `4d02628ff0875a1960d34be584a68f88528b96242bdc5a05a40a29ef01cf1532`）: 3.2。
  - `rfc8017.txt`（SHA-256 `1e72dc473d18df3fc5598cdc12795a9f18f36f1aef15abc23a55eb0d58151d11`）: A.1.1、A.2。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: limbo（証明書の parse だけ）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-x509.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-x509.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- OID は data（block ごとに出典、RFC の ASN.1 の module の値）。
- ca-certificates の全部が parse でき、鍵の種類の数を結果に記録。x509-limbo の証明書（peer・trusted・untrusted）の parse の失敗の数を記録し、その理由を分類。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh x509     # この module
sh plan/ws335/tests/run-unit.sh --win32 x509     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 02:36 JST）Sonnet 5.5 high（q1895-i01）。worktree s-ws335-p017（main b238dfeb1）。

作った物: `userland/base/libcrypto-compat/cryc-x509.c`、`plan/ws335/tests/unit/test-x509.c`。header・試験の道具・他の module は変えていない（`git status` で確認）。ct の試験は無い（証明書は公開の data）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`、各 command に `timeout` と `ulimit -f` を付けた）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh x509` | `test-x509: PASS (159986 checks)` / `run-unit: PASS`（ASan・UBSan） |
| `sh plan/ws335/tests/run-unit.sh --win32 x509`（x86_64、Wine） | `test-x509: PASS (159986 checks) (win32)` / `run-unit: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（posix。test 本と ct- 13 本が全部 PASS、test-x509 は 159986 checks） |

試験の中身:

- 署名 algorithm: sha256/384/512WithRSAEncryption（NULL・parameters なし）、ecdsa-with-SHA256/384/512（parameters なし）、RSASSA-PSS（SHA-256/384/512 と同じ hash の MGF1、salt 0・20（既定）・32・48・64、trailer 1）、範囲外（SHA-1・MD5・SHA-224 の RSA、ECDSA の SHA-1・SHA-224、Ed25519、未知の OID、PSS の hash 欠落（SHA-1）・別の hash の MGF1・MGF1 でない mask・salt 65 以上・trailer 1 以外・parameters なし）は UNSUPPORTED、壊れた物（PKCS#1 に NULL 以外、ECDSA に parameters、PSS の parameters の型・順序・重複・余り・負の salt、MGF1 の parameters 欠落・SET、hash に NULL 以外、OID が壊れ、余りの byte、2 個目の parameters）は EINVAL。全 prefix と 20000 本の乱数。
- 公開鍵: RSA（2048・4096・1 byte の n、e の 1〜3 byte、n と e の 0・負・非 minimal・余り、NULL parameters の欠落・誤り、unused bits）、EC（P-256・P-384 の点、圧縮点は UNSUPPORTED、長さ・先頭 byte・曲線と長さの食い違い・unused bits は EINVAL、P-521・secp256k1・brainpool・parameters なし・NULL・明示 parameters は UNSUPPORTED、曲線 OID が壊れ）、他の algorithm（Ed25519・X25519・RSASSA-PSS 鍵・未知）は UNSUPPORTED、SPKI の構造の誤り、全 prefix。
- 証明書: derwriter と hex で組んだ証明書の全 field（version の 5 通りと [0] の中身、serial の 1〜21 byte・負・0・非 minimal・22 byte、tbs と外の algorithm の食い違い、issuer の空・subject の空、Name の構造の誤り 10 通り、validity の UTCTime・GeneralizedTime・1950 と 2049 の端・不正な field、unique ID の順序・version・BIT STRING、署名値の unused bits・空、余りの byte 3 か所）、extension の全種（basicConstraints・keyUsage の各 bit と 9 bit まで・extKeyUsage・SAN・nameConstraints（critical 必須・minimum 0・maximum 拒否・順序・空）・SKI・AKI）、重複・criticality・未知の critical・policy 系（critical でも通す）・64 個まで・65 個目は EINVAL・DEFAULT FALSE の明示は EINVAL・Extension の構造の誤り 8 通り・[3] の枠、全 prefix（EINVAL で cert が全部 0）、全 byte を 4 通りに置き換え、乱数の変異 30000 本（受理した物は全 view が入力の内側）。
- 外の corpus: ca-certificates の bundle（cacert-2026-08-13.pem）の 121 枚は全部 parse でき（version 3 が 121）、全部が自己発行。**鍵の種類: RSA 80、P-256 3、P-384 37、P-521 1（e-Szigno TLS Root CA 2023、CRYC_KEY_UNSUPPORTED）**。各証明書で tbs・署名の位置と、鍵の種類（SPKI の byte を OID で単純に探した結果）との一致を照合。x509-limbo の 9,792 件の peer・trusted・untrusted の全 39,936 枚のうち 39,920 枚が parse でき、**16 枚が EINVAL**。理由の分類（下の表。id 一覧は試験の `limbo_refused_ids` に固定、各 id がちょうど 1 枚拒否されることを照合）。Wycheproof の ECDSA 4 file と RSA 5 file と Ed25519 の group の key（3588 件）: EC は曲線と点が file の `uncompressed` と一致、RSA は parse した n・e から組んだ RSAPublicKey が file の `publicKeyAsn` と一致、Ed25519 は UNSUPPORTED。

limbo の拒否 16 枚の理由（該当 .c の行は拒否した所の確認用の一時の build で確かめた。実装は変更していない）:

| 理由 | case の id | expected |
| --- | --- | --- |
| RSAPublicKey が SEQUENCE でない | invalid::invalid-issuer-key | FAILURE |
| extKeyUsage が空 | rfc5280::eku::ee-eku-empty | FAILURE |
| nameConstraints が critical でない | rfc5280::nc::permitted-dns-match-noncritical、rfc5280::nc::not-allowed-in-ee-noncritical、**webpki::nc::permitted-dns-match-noncritical** | FAILURE、FAILURE、**SUCCESS** |
| nameConstraints の subtree の列が空 | rfc5280::nc::permitted-empty-sequence-excluded-nonempty、rfc5280::nc::permitted-nonempty-excluded-empty-sequence、webpki::nc::intermediate-permitted-excluded-subtrees-both-empty-sequences | FAILURE |
| nameConstraints に permitted も excluded も無い | webpki::nc::intermediate-permitted-excluded-subtrees-both-null | FAILURE |
| subjectAltName の値が SEQUENCE でない | rfc5280::san::malformed | FAILURE |
| serial が 21 byte を越える | rfc5280::serial::too-long | FAILURE |
| issuer が空（CA の空の subject が issuer になった） | rfc5280::ee-empty-issuer、rfc5280::ca-empty-subject | FAILURE |
| keyUsage が空の BIT STRING | rfc5280::root-inconsistent-ca-extensions | FAILURE |
| extension の重複 | rfc5280::duplicate-extensions | FAILURE |
| tbsCertificate と外の signatureAlgorithm の食い違い | rfc5280::mismatching-signature-algorithm | FAILURE |

**header と limbo の食い違い（Q1 へ）**: `webpki::nc::permitted-dns-match-noncritical` は expected SUCCESS（信頼する root の nameConstraints が critical でない）だが、header の cryc_x509_parse は「nameConstraints は critical 必須、でなければ EINVAL」と書くので、header に従い EINVAL（trusted の root が読み込めない）。RFC 5280 4.2.1.10 は CA に critical を MUST とするが、verifier が critical でない nameConstraints を処理することは禁じない。header を変える提案: 「critical でなくても受理し、処理する」にすれば、この SUCCESS の case が通り、rfc5280::nc::*-noncritical の 2 件（expected FAILURE）は p020 の path の確かめで別の理由があるかを見る要がある。header の変更は Q1 の判断（p019・p020 の前が良い）。試験は現状の header の動作を固定している（`limbo_refused_ids` と `LIMBO_SUCCESS_REFUSED`）。

わざと壊す確かめ（git の外。`build/tmp/p017/brk/brk.py`、`.c` を `build/tmp/p017/brk/mNNN/` に複写して書き換え、その module だけを build し、試験を `ulimit -f` と `timeout 600` と出力 file で流す。実装 file は触らず、`git status` で変化が無いことを確認）。最終の試験で 254 通り: **試験の失敗（`test-x509: FAIL` の行）241、sanitizer の異常終了 3（実装の読み・index の範囲外を ASan・UBSan が検出）、見逃し 10、build の誤り 0**。区別は BUILD-ERROR（compiler の誤り）・FAIL・CRASH・SURVIVED の 4 つで、build の誤りの mutation（未使用の変数）は `(void)` 付きに直して流し直した。

- 変えた物: 定数（拡張の上限、pathLen・salt の上限、version・keyUsage の bit 数、各 tag、点の先頭 byte、座標の size）、全 OID の 1 byte、対応表（algorithm・hash・曲線・拡張の種類・purpose の bit）、`oid_equal`・`is_null` の比較、AlgorithmIdentifier・PSS の各 field の検査、公開鍵の各検査（n・e の 0、unused bits、圧縮点、先頭 byte、長さ）、Name・serial（境界の `>`→`>=`、非 minimal の 2 通り）、Certificate・tbs の各検査（version の +1、既定の version、algorithm の比較、validity の取り違え、自己発行の比較、unique ID）、extension の列（上限の `>=`→`>`、重複、critical の扱い）と各 extension の検査（bit の順、byte の shift、path 長、SAN・NC・SKI・AKI の view と flag）。
- 見逃し 10 通りは等価か冗長で、観測できる違いが無い: (1) MGF1 の parameters の有無の検査を消しても、続く parameters の tag の検査が欠落（tag 0）を拒む、(2) trailer が 5 byte 以上のとき `trailer_unsupported = 0` にしても、`cryc_der_small_integer` が値を 0 にするので 1 との比較で同じ結果、(3) `!mask_is_mgf1` の早期 return を消しても、MGF1 でなければ mask_hash が NONE で hash と一致せず同じ結果、(4) mask 欠落の既定 `mask_is_mgf1 = 1` を 0 にしても、MGF1 と SHA-1 は SHA-2 の hash と一致しないので同じ結果、(5) RSA の NULL parameters の「有り」の検査を消しても、欠落は tag 0 で NULL と判定されず同じ結果、(6) tbs と外の algorithm の size の比較を消しても、同じ長さの header でない限り memcmp が違いを見る（DER の長さの octet が先頭近くにある）、(7) tbs の algorithm の parse を消しても、外と同じ byte なら外の parse が既に成功、(8) 自己発行の `==` を `<=` にしても、DER の header が違うので memcmp が違いを見る、(9) keyUsage の size 3 以上の検査を消しても、続く 9 bit の検査が拒む、(10) keyUsage の size 0 の検査を消しても、続く「bit が 1 つ以上」の検査が拒む。
- 最初の実行で見逃した 11 通りは試験を足して塞いだ: 圧縮でも context でもない private class の GeneralSubtree の base、PSS の parameters・hash の field・mask の field・MGF1 の parameters が SEQUENCE でなく SET の場合、version の [0] に INTEGER 以外が入る場合（`version_raw`）、unique ID の BIT STRING の誤り（`uid1_raw`）、RSA の unused bits（最終 byte が偶数の exponent で、bit string の検査でなく RSA の検査で拒むこと）。試験の側の null 参照で試験が異常終了する場合を FAIL の行になるよう、view の比較を `CHECK_VIEW`・`view_byte` に直した。

自分で決めた点（header が黙っていた所）:

1. 署名 algorithm: PKCS #1 の OID に NULL 以外の parameters は EINVAL（RFC 4055 5 は NULL か欠落）、ECDSA の OID は parameters が何であれあれば EINVAL（RFC 5758 3.2 は「省く」）。header は NULL の ECDSA だけを例に挙げていた。RSASSA-PSS の hash・MGF1 の hash に付く parameters は NULL か欠落（RFC 4055 2.1）、それ以外は EINVAL。PSS の既定の salt は 20、`trailerField` 1 の明示は受理（RFC 4055 3.1）、その他の値と 5 byte 以上の整数は UNSUPPORTED、salt の負は EINVAL・65 以上は UNSUPPORTED。MGF1 の parameters 欠落は EINVAL（RFC 4055 2.2 の MUST）。
2. 公開鍵: rsaEncryption の parameters が NULL でないか欠落は EINVAL。n・e の 0 は EINVAL。EC の parameters が OID でない（欠落・NULL・SEQUENCE）は UNSUPPORTED（well-formed だが範囲外）、OID の contents が壊れていれば EINVAL。EC の点が圧縮形（02 または 03 と座標 1 つ）は UNSUPPORTED、それ以外の長さ・先頭 byte の誤りは EINVAL。BIT STRING の unused bits は RSA・EC では 0 必須（EINVAL）、未対応の algorithm は BIT STRING の形だけ確かめる。
3. 証明書: Name の構造（SEQUENCE OF SET（空でない）OF SEQUENCE { OID, 値 }）を確かめ、issuer の空は EINVAL、subject の空は受理（header 通りの「whole encodings」を view にする）。serial は 1〜21 byte に加えて DER の minimal（先頭 00 の後が 7f 以下、先頭 ff の後が 80 以上は EINVAL）を確かめる。validity は UTCTime と GeneralizedTime のどちらでも受理（RFC 5280 4.1.2.5 の「2049 までは UTCTime」は強制しない）、not_before が not_after より後でも受理。署名値が空（unused の octet だけ）の BIT STRING は受理（署名の確かめが落とす）。
4. extension: Extension の critical に DEFAULT の FALSE を明示した物は EINVAL（DER は DEFAULT を省く）。basicConstraints の cA の FALSE の明示も EINVAL（同じ理由）。拡張の数は 64 までで 65 個目は EINVAL（重複の確かめの O(n²) を限るため。実の証明書は 20 個未満、limbo の 39,936 枚と bundle の 121 枚に超える物は無かった）。subjectAltName の各 GeneralName は element として壊れていないことだけを確かめる（意味は cryc-name）。nameConstraints の base は context class の tag（GeneralName の CHOICE は全部 context）を要求し、minimum は 0（明示された 0 を受理）、maximum は EINVAL。authorityKeyIdentifier は構造を確かめ、keyIdentifier だけ保つ（authorityCertIssuer と serial の組の規則は確かめない）。subjectKeyIdentifier が空（長さ 0）でも受理し、ski_size 0 で ski は NULL でない view になる。
5. 内部: static の補助（`x509_oid_lookup`・OID の表の形・`x509_read_algorithm`・`x509_read_pss_field`・`x509_open_value`・各 extension の関数）の分け方、拡張の OID の重複を `seen[]` の配列で確かめること、parse を内部の関数で行い公開の関数が失敗時に cert を 0 にすること。

header と phase.md の食い違い: 上の nameConstraints の critical（header 対 limbo）。phase.md の §3 の試験の項目と矛盾する点は無かった。header の comment で気づいた細かい点: `cryc_x509_signature_algorithm` の comment は NULL の ECDSA だけ EINVAL と読めるが、parameters が何であれ ECDSA は欠落が正（RFC 5758 3.2）なので、上の決定 1 を採った。直すかは規約の見直しの Phase（p035）。

未実施: run-ct（秘密を扱わない）、32 bit の size_t の機械、aarch64 Windows の実行（build のみ、check-skeleton）、性能の測定、実機・QEMU（範囲外）、nameConstraints の critical を緩めた時の limbo の再確認（header の判断待ち）。
