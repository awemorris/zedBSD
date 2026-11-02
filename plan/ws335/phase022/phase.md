<!-- awesome-plan project=zedbsd record=ws335-p022 -->

# ws335-p022: 公開の X509（d2i_X509 ほか）と内部の store の生成

Status: 実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1927-i01。cleared は Q1。依存: p020、p021）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-api.h（x509 の部分）`、`include/libc/compat/crypto/x509.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - OpenSSL 3.5 の API の文書（互換の振る舞いの正）: /home/awe/zedBSD-claude1/build/distfiles/openssl-3.5.8.tar.gz（SHA-256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2、展開は自分の build/tmp の新しい dir に。`doc/man3`・`doc/man7` の .pod は振る舞いの仕様として読み、文・code を写さない）: doc/man3/d2i_X509.pod、X509_STORE_add_cert.pod、X509_VERIFY_PARAM_set_flags.pod、X509_STORE_CTX_get_error.pod。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/api-x509.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-api-x509.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- d2i_X509 の *in の進め方は OpenSSL の約束どおり。
- X509_VERIFY_PARAM_set1_ip_asc は IPv4 と IPv6 の text（RFC 4291 2.2）。
- `cryc_verify_code_to_x509`: CRYC_VERIFY_REJECTED は `X509_V_ERR_CERT_REJECTED`（28）、`X509_verify_cert_error_string(28)` は "certificate rejected"（OpenSSL 3.5.8 `crypto/x509/x509_txt.c` の文の値。data として）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh api-x509     # この module
sh plan/ws335/tests/run-unit.sh --win32 api-x509     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 10:49 JST）Sonnet 5.5、Queue q1927-i01。worktree s-ws335-p022（main a6a0be590）。

作った物: `userland/base/libcrypto-compat/api-x509.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-api-x509.c`。header・試験の道具・他の module・Makefile は変えていない（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）。

実行した command と最後の行:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh api-x509`（ASan・UBSan・LeakSanitizer） | `test-api-x509: PASS (507799 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 api-x509`（x86_64・Wine） | `test-api-x509: PASS (507796 checks) (win32)` / `run-unit: PASS`（3 件の差は 64 bit の long でだけ流す check） |
| `sh plan/ws335/tests/run-unit.sh all`（posix、他の run-unit と重ねずに流した） | 53 行が PASS、最後の行 `run-unit: PASS`（test-api-x509 507799 を含む） |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS` |
| `git diff --check` | 出力なし |

run-ct は流していない（この module は秘密を扱わない: 証明書・host 名・address だけ。ct-api-x509 も作っていない）。

試験の内容:

- 証明書は derwriter.h で RFC 5280 4.1・4.2.1.6 から組み立てる（署名は placeholder。この module は署名を見ない）。自己発行の証明書は store に入れれば自分が trust anchor になる（cryc-chain.h）ので、署名器なしで「公開の呼び出し → 検証の verdict → X509_V_ の code と文」までを通す。
- 判定の期待値: verdict 23 個と X509_V_ の code（OpenSSL の x509_vfy.h の数値を手で並べた）、文 26 個（x509_txt.c の文）、-2000..2000 の全ての未知の code が汎用の文、LONG_MAX・LONG_MIN、下位 32 bit だけが既知の 64 bit の値（cut されない）。誤りの code は library を 23 bit 左に詰める layout から手で計算した値（0x058c0102 など）。
- d2i_X509: 基本（*in の進み、複写、parse した field）、out の約束（NULL の *out・既存の *out の置き換え・失敗で *out を free して NULL・別の参照が残る場合）、誤り（長さ 0・負・LONG_MIN、in が NULL、*in が NULL、1〜size-1 の全ての切れ方、先頭 octet の 256 通り、SEQUENCE だが証明書でない物、40 回積んで 16 件で打ち切り）、後ろの余分な bytes と 2 枚続き、長さの形（短・1 octet・2 octet・3 octet）、外側の長さが 0xffffff ちょうどの証明書（16 MiB）と 0x1000000（4 octet 長）、全 octet × 4 通りの変更（1048 通り: 461 拒否・587 受理）、乱数 3000。全ての入力は大きさちょうどの heap の buffer（ASan が読み過ぎを見る）。
- cryc_x509_from_der: 受理、NULL・0・0x1000005・0x7fffffff・余分な 1 octet・切れ。最大の大きさ（0xffffff + 5）の証明書の受理と、1 octet 多い入力の拒否（大きさの検査と parse の誤りの区別）。
- 参照数: X509 と X509_STORE を 2 参照にして最初の free で残り、2 回目で解放。NULL の free。
- X509_STORE_add_cert: NULL 引数、複写（entry の DER は別の buffer）、同じ物の重複（同じ object と別 object の同じ bytes）は成功して増えない、追加の順、caller の X509_free の後も store の複写が残る、4096 枚（上限）の次の 1 枚は 0 と UNSUPPORTED、上限での重複は成功、4 thread × 100 枚＋共有 50 枚の同時の追加で 450 枚・参照数が戻る。parse できない bytes になった object は INTERNAL_ERROR で断る（作れる経路が他に無い防御）。
- X509_VERIFY_PARAM_set1_host: 小文字化・末尾の dot・長さ指定（先頭 n 文字だけ・0 は C 文字列）・label 63/64・名前 253/254・末尾の dot つき 254・255・NUL を含む・255 文字で NUL が無い buffer（ちょうどの heap で読み過ぎを見る）・空と NULL は host を消す・不正な名前 17 通りは変えない。
- X509_VERIFY_PARAM_set1_ip_asc: IPv4 8 通り・IPv6 29 通り（完全形・大文字・:: の各位置・IPv4 を含む形）の期待 octet を RFC 4291 2.2 から手計算、拒否 130 通り超（先頭 0・桁・dot の位置・:::・:: 2 回・8 group と ::・zone・括弧・prefix・G などの文字）。別の作りの参照 reader（colon で先に分ける）と、乱数・変異の 40 万の text で受理と octet が一致（受理 80107・拒否 319893・差 0）。
- 公開の呼び出しの連携: 自己署名の server が store に無ければ 18（"self-signed certificate"）、追加して 0、host 名の不一致 62・大文字小文字・末尾 dot・親の名前、address の不一致 64、IPv6 の別表記の一致、拒否された設定は前の設定のまま。
- 外の corpus: ca-certificates の 121 枚を d2i_X509 で読み store に入れる（parser の直読みと field が一致）、x509-limbo の証明書 39936 枚（受理 39923・拒否 13）が parser の判定と全て一致。

わざと壊す確かめ（`build/tmp/p022-mut/` の中、api-x509.c の複写だけを壊す。1 つの mutant ごとに build して全試験を流す。1 件ごとに CPU 400 秒・file 64 MB・150 秒・RSS 6 GB の上限、並列 4）:

- 初回 767 通り（定数の ±1・比較と算術の演算子・文字定数・条件の否定・文の削除・return の 0 と 1・verdict の対応表と文の表の各行・define）。結果: 試験の失敗 528・sanitizer の中断 177・timeout（無限 loop）2・build の誤り 12・見逃し 48。
- 見逃しのうち試験で塞いだ物: 失敗した d2i が *out を free しない（長さ 0 と、証明書でない SEQUENCE の 2 経路）、has_host が 1 でない値、大文字 G を 16 進の数字として受ける、parse できない object の add_cert（INTERNAL_ERROR の経路）。再実行で全て FAIL。
- build の誤り 12（for の条件の行や unbrace の本体の削除）は、文を `;` に、for の条件を `0`・`1` に替えた 17 通りに作り直し、全て FAIL か timeout。
- 最終: 784 通り（767＋作り直し 17）のうち build できない 12 を除く 772 通りで、試験の失敗か sanitizer か timeout で検出 732、見逃し 40。見逃し 40 の内訳: メモリ不足の経路 14（store・certificate・add_cert の calloc・malloc・mutex 作成の失敗: ASan 下で失敗させられない）、等価または他と重なる防御 26（calloc の個数 1→2・配列の大きさ +1・sizeof の添字・return 1→2・has_dot 等の flag 1→2・hex_value の ':'（colon は token に入らない）・v4 の桁数 >3 と値 >255 の重なり・field_count の >= と >・v4 の bytes の memset が呼び側 memset と重なる・v6 の out の memset が呼び側 memset と重なる・digit_count の初期化の削除（初期化されない値を ASan・UBSan が見ない））。

自分で決めた点（header が黙っている所）:

- 失敗は常に 1 件だけ誤りを積む: 引数の NULL は X509 の library の PASSED_NULL_PARAMETER、引数の不正は PASSED_INVALID_ARGUMENT（長さ ≤ 0・host 名・address の text）、メモリ不足は MALLOC_FAILURE、store が 4096 枚は UNSUPPORTED、parse できない証明書は ASN1 の WRONG_TAG、先頭が SEQUENCE でないと d2i は WRONG_TAG、SEQUENCE の長さが入力に収まらないと HEADER_TOO_LONG。成功は何も積まない。set1_host・set1_ip_asc の失敗も 1 件積む（header は積むと書いていないが、他の公開関数と揃えた）。
- cryc_verify_code_to_x509: 部分集合に専用の code が無い verdict（NO_PEER_CERTIFICATE・MALFORMED・WORK_EXCEEDED、表に無い値）は X509_V_ERR_UNSPECIFIED。UNABLE_TO_GET_ISSUER は 20（local issuer）、header の一覧にある 2 は使わない。**1 つの verdict が 2 つの X509_V_ に当たる物は 1 つに決めた**: KEY_USAGE（中間の keyCertSign 無しと leaf の digitalSignature 無し）は 32 (KEYUSAGE_NO_CERTSIGN)、NAME_CONSTRAINT_VIOLATION（permitted の外と excluded の中）は 47 (PERMITTED_VIOLATION)。区別を伝えるには cryc_verify_result に深さと種別が要る（p035 の header の見直しか p032 で扱うか、Q1 の判断。39 と 48 は文だけ持つ）。
- X509_verify_cert_error_string: header の 26 個の code の文は x509_txt.c の文（data）。それ以外は "unknown certificate verification error"。long をそのまま比べる（OpenSSL の `(int)` への切り詰めはしない）。
- d2i_X509 は DER を厳格に読む（cryc-der.h と同じ: BER の不定長は断る）。SEQUENCE の外側の長さを読んで、その大きさちょうどを cryc_x509_from_der に渡す。*in は成功の時だけ進む。*out は成功で既存を X509_free してから置き換え、失敗で（引数の誤りでも）X509_free して NULL にする。
- cryc_x509_from_der は 0xffffff + 5 を超える大きさを parse の前に断る（DER の長さの上限と 5 octet の header）。複写は呼び側の bytes に依らない。
- X509_VERIFY_PARAM_set1_host: 空の名前と NULL は host を消す（OpenSSL の文書。header は NULL だけ書いている）。長さが 0 の時は C 文字列で、255 文字までしか読まない（長い名前は不正）。名前の中の NUL は不正。set1_host は address を、set1_ip_asc は host を消す（1 つの identity だけ照合、cryc_verify_params の約束）。NULL の host は address を消さない。失敗は何も変えない。IP の text を set1_host に渡しても host 名として扱う（cryc_name_host_normalize が許す形。照合は dNSName にだけ当たり、iPAddress には当たらない）。
- IPv4: 先頭の 0 を拒む（"01.2.3.4" は 8 進と読まれうる曖昧さを避ける）。IPv6: "::" は 1 つ以上の group を表す（8 group と "::" は不正）、group は 1〜4 桁、末尾の IPv4 は 2 group、zone・括弧・prefix は不正。
- store の複写の所有: X509_STORE_add_cert は DER を複写する（cryc_store_add_der）ので、caller の X509_free の後も使える。X509 は参照数 1 で作り、`X509_up_ref` は公開しない（header に無い）。
- 内部の補助は static の x509_fail・d2i_forget・hex_value・ip_parse_v4・ip_parse_v6、表は verdict_codes・code_texts。

phase.md と header の食い違い: 無し（check-skeleton の BASE は Q1 の指定どおり 4eae22020。phase.md の例の b7dfd3952 のままだと header 以外の道具の変更で FAIL する）。

未実施・できなかった事:

- メモリ不足の経路（store・certificate・add_cert の MALLOC_FAILURE、mutex 作成の失敗）の試験: ASan 下では malloc を失敗させられない。読んで確かめた。
- run-ct（秘密を扱わない）、aarch64 の Windows（check-skeleton の build だけ）、実機・QEMU。
- X509_V_ の区別（32 と 39、47 と 48）を呼び側に渡す口（header の変更が要る、上の判断の点）。

