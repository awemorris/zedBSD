<!-- awesome-plan project=zedbsd record=ws335-p002 -->

# ws335-p002: 土台（約束・定数時間の補助・byte 順）、SHA-2、hash の選択・HMAC・HKDF

Status: 実装済み・試験 PASS（2026-11-01 06:58、Sonnet。cleared は Q1）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc.h`、`userland/base/libcrypto-compat/cryc-sha2.h`、`userland/base/libcrypto-compat/cryc-hash.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-180-4.pdf`（SHA-256 `0455b406d89648d20cbde375561e19c245b9815e894164c2670772e3d54deb82`、印刷の頁 N は PDF の頁 N+5）: 4.1.2・4.2.2（定数）、5.1.1・5.1.2（padding）、5.3.3〜5.3.5（初期値）、6.2・6.4・6.5（計算）。
  - `rfc2104.txt`（SHA-256 `64d5245a9101929025336e470e3737f118704001249d503c85a86e19fe9fbb01`）: 2。
  - `rfc4231.txt`（SHA-256 `72178527ce93500e730bc8eb182b857e583096d652b64ece0879c52ba1df973b`）: 4（試験の値）。
  - `rfc5869.txt`（SHA-256 `7a40eb3835b35fc947eb12a2ed614db079d43b26e50dbc537c31fba16397089c`）: 2、付録 A（試験の値）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-shabytetestvectors-SHA256ShortMsg ほか SHA256/384/512 の ShortMsg・LongMsg、wycheproof-hmac_sha256/384/512_test、wycheproof-hkdf_sha256/384/512_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-util.c`、`userland/base/libcrypto-compat/cryc-sha2.c`、`userland/base/libcrypto-compat/cryc-hash.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-util.c`、`plan/ws335/tests/unit/test-sha2.c`、`plan/ws335/tests/unit/test-hash.c`。定数時間の試験: `plan/ws335/tests/unit/ct-util.c`、`plan/ws335/tests/unit/ct-hash.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 定数の表（K、初期値）は block ごとに「Values referenced: FIPS 180-4 4.2.2」の形で出典。64 bit の定数は CRYC_U64(上位, 下位)。
- cryc_ct_* は分岐・表引きなしで（ct-util で確かめる）。cryc_wipe は volatile の pointer で。
- SHA-384 は SHA-512 と同じ圧縮で初期値と出力の長さが違う。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh util sha2 hash     # この module
sh plan/ws335/tests/run-unit.sh --win32 util sha2 hash     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh util hash
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

実装済み・試験 PASS（2026-11-01 06:58、Sonnet）。cleared の判定は Q1。

### 作った物

- `userland/base/libcrypto-compat/cryc-util.c`（wipe、定数時間の比較・mask・選択、byte 順）、`cryc-sha2.c`（SHA-256・384・512）、`cryc-hash.c`（hash の選択、HMAC、HKDF）。header は変えていない。
- 単体試験 `plan/ws335/tests/unit/test-util.c`・`test-sha2.c`・`test-hash.c`、定数時間の試験 `ct-util.c`・`ct-hash.c`。

### 実行した command と最後の行

- `sh plan/ws335/tests/run-unit.sh util sha2 hash`: `test-util: PASS (169160 checks)`、`test-sha2: PASS (14092 checks)`、`test-hash: PASS (12965 checks)`、`run-unit: PASS`。
- `sh plan/ws335/tests/run-unit.sh --win32 util sha2 hash`: 同じ 3 つが `(win32)` 付きで同じ件数の PASS、`run-unit: PASS`。
- `sh plan/ws335/tests/run-unit.sh all`: 上の 3 つと `test-tools: PASS (18 checks)`、`ct-hash: PASS (37 checks)`、`ct-util: PASS (33 checks)`、`run-unit: PASS`。
- `sh plan/ws335/tests/run-ct.sh util hash`: `ct-util: PASS (33 checks)`、`ct-hash: PASS (37 checks)`、`run-ct: PASS`。
- `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`: `check-skeleton: PASS`（-Werror の build で warning 0）。
- `git diff --check`: 出力なし。

### 試験の中身と件数

- 公式の vector: CAVP の SHA-256 ShortMsg 65・LongMsg 64、SHA-384 ShortMsg 129・LongMsg 128、SHA-512 ShortMsg 129・LongMsg 128、Monte 各 100（計 743 record）。各 record を一括・1〜1000 byte の細切れ・途中の複製の 3 通りで確かめる。RFC 4231 の case 1〜7（SHA-256・384・512、case 5 は 128 bit に切った MAC）、RFC 5869 付録 A.1〜A.3 の PRK と OKM、Wycheproof hmac 各 174 件、hkdf は sha256 86・sha384 83・sha512 83 件（無効の件は EINVAL を確かめる）。
- NIST の例の値（"abc"、448 bit の文、a を 100 万個）は FIPS 180-4 の元の版に付く例として知られる値。CAVP で確かめた実装と一致することで裏付けた（他の実装の出力ではない）。
- 長さの欄: 全ての total（2^32 を越える値、2^61 手前まで）と使用中の byte 数で、padding の最後の block を手で組み、同じ状態から圧縮した結果と final の出力を比べる。
- 境界・誤り: 長さ 0・1・block の境（CAVP の全 byte 長と 0〜299 byte の乱数）、NULL と長さ 0、不明な id（0・4 以上・-1）の ENOTSUP、prk の短さ・出力の過大（255 * HashLen + 1、(size_t)-1）の EINVAL と出力が書かれないこと（前後の canary）、HMAC の鍵の長さ 0〜block+3、RFC 2104 の式の手の組み立て（鍵が短ければ 0 詰め、長ければ digest）。
- 定数時間: 比較・mask・選択・byte 順（ct-util）、3 つの hash、HMAC（鍵長 0・20・128・300）、HKDF の extract・expand（ct-hash）を Valgrind で。

### わざと壊す確かめ

作業木のコピー（`build/tmp/p002/mut/`、git の外）で 38 通りを壊して試験を流し、毎回本物の source から書き戻した。壊した所: K の定数 2、padding の境界 2、長さの shift 2、SHA-384 の IV と出力長、sigma の rotate、Maj、update の block 境、final で消さない、ipad・opad、長い鍵の境界、HMAC の連結、counter の始まり、最大 block 数、前の block の連鎖、zero salt、prk_size の検査、out_size 0、hash の長さ・id、hmac_clear、wipe の無効化と 1 つ足りない場合、ct_equal の `>>`・ループの端・早期 return、mask の向き・反転、選択の反転、copy_if の mask 無視、load/store の byte の位置。最初の 1 巡で 34 件が FAIL（捕まえた。早期 return の 1 件は run-ct が FAIL）、4 件を見逃した。試験を足して 2 件を捕まえ、残りは試験で見えない物と等価な変更で、結局 36 件を捕まえ 2 件は見えない物:

- 最初の試験は SHA-512 の length の上位 64 bit（total >> 61 を >> 60 に）を見逃したので、2^60 以上 2^61 未満の total を足し、再実行で FAIL を確かめた。
- zero salt を HashLen より 1 byte 短くする変更は、HMAC の鍵が 0 詰めされるので結果が同じ（等価な変更。試験の対象から外し、代わりに zero salt の値を 1 にする変更を入れて FAIL を確かめた）。
- hmac_init の鍵 block を最後に消さない変更は、関数の局所変数の消去なので試験から見えない（review で確かめる）。
- `cryc_ct_mask_eq32` を `if` に替える変更は run-ct が見逃した: -O2 の compiler が分岐の無い命令列にするため（生成 code が定数時間なので memcheck が何も出さない）。
- 戻した確かめ: 壊した copy を毎回 repo の source から書き戻して内容の一致を確かめ、最後に `cmp` で copy の 3 つの .c が repo の物と一致することを確かめた。repo の source は壊していない。

### 自分で決めた点（header が黙っている所）

- 不正な引数: `data == NULL` で size > 0 の update は何もしない（読まない）。`cryc_wipe(NULL, n)` は何もしない。`cryc_ct_equal`・`select_bytes`・`copy_if` の NULL（size > 0）は読み書きしない（equal は 0）。`cryc_hmac_init` で key == NULL かつ key_size > 0、`cryc_hkdf_extract` で ikm == NULL かつ ikm_size > 0、`cryc_hkdf_expand` で prk == NULL または info == NULL かつ info_size > 0 は EINVAL（header は ENOTSUP だけを書くが、黙って別の MAC を返すより安全として追加。HKDF extract の NULL salt は header どおり零）。
- `used` が block 長以上など壊れた構造は、update は何もせず、final は空の block とみなす。`out_size` が 64 を越える構造は 64 に切る（範囲外を書かない）。
- `cryc_hash_init` は不明な id でも構造を 0 で埋めてから ENOTSUP を返す。`cryc_hmac_init` は失敗時に hmac 全体を消す。id が無い構造の update・final は何もしない（final は出力を書かない、構造は消す）。
- `cryc_ct_select_bytes` の mask は下位 8 bit だけを使う（0 または 0xffffffff の約束の範囲では header と同じ）。
- HKDF-Expand は PRK で HMAC を一度だけ鍵付けし、各 block はその複製から始める。prk_size が HashLen を越える時は PRK 全体を HMAC の鍵にする（RFC 5869 の 2.3）。out_size 0 は検査の後に 0 を返す。
- SHA-2 の内部は sigma・Ch・Maj を static 関数に分け、rotate は関数（C89 に inline が無いため）。圧縮の message schedule は終わりに消す。

### 環境の問題（Wine）

`run-unit.sh --win32` の最初の実行は、この host で Wine の prefix の作成（`wineboot -i`）が約 5 分で失敗した（`wine: could not load kernel32.dll, status c0000135`。syswow64 が空のまま。DISPLAY が無く、Xvfb を付けても同じ）。壊れた prefix は `build/tmp/ws335-wine-broken-1`〜`-4` に退避した（削除は Q1 へ）。別の担当（ws334 の p019）の完成した prefix を `build/tmp/ws335-wine` に複写して `.ws335-ready` を置いたところ、`--win32` は 29 秒で PASS した。スクリプトは変えていない。他の担当が新しく prefix を作るなら同じ問題に当たりうる。

### 未実施

- 2^61 byte を越える total の挙動（header が呼び出し側の誤りとする範囲）。実際に 4 GiB 以上を流す試験（長さの欄は状態の直接設定で確かめた）。
- 実機・QEMU は対象外。
