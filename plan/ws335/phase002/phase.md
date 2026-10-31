<!-- awesome-plan project=zedbsd record=ws335-p002 -->

# ws335-p002: 土台（約束・定数時間の補助・byte 順）、SHA-2、hash の選択・HMAC・HKDF

Status: planned（2026-11-01 p001 が作成。起動は Q1。依存: なし）
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
- header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh util sha2 hash     # この module
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh util hash
BASE=@BASE@ sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

（未着手）
