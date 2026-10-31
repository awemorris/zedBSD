<!-- awesome-plan project=zedbsd record=ws335-p003 -->

# ws335-p003: SHA-3・SHAKE（Keccak）

Status: in-progress（2026-11-01 06:59 q1820、Sonnet）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-sha3.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-202.pdf`（SHA-256 `1592607831ff0908cc590632ce371c6c95e94025bb1a0c8ae90a4d0ec1ed025e`、印刷の頁 N は PDF の頁 N+8）: 3.1・3.2（θρπχι、Table 2 の回転、Algorithm 5・6 の round 定数）、3.3（Algorithm 7）、4（sponge、Algorithm 8）、5.1（pad10*1）、6.1・6.2、B.1・B.2（byte の順と padding の byte）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-sha-3bytetestvectors-*（SHA3-256・SHA3-512）、cavp-shakebytetestvectors-*（SHAKE128・SHAKE256 の ShortMsg・LongMsg・VariableOut）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-sha3.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-sha3.c`。定数時間の試験: `plan/ws335/tests/unit/ct-sha3.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- round 定数と回転の量は data として出典。
- squeeze は何回でも続く 1 本の出力（FIPS 203 の XOF の使い方）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh sha3     # この module
sh plan/ws335/tests/run-unit.sh --win32 sha3     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh sha3
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

中断（host の再起動のため Q1 の指示で打ち切り。未完。cleared でも「実装済み・試験 PASS」でもない）。

### できた物

- `userland/base/libcrypto-compat/cryc-sha3.c`: 全関数を実装（Keccak-p[1600,24] の permute、4 つの init、absorb、final、shake_squeeze、clear）。
  確かめたのは単体の compile だけ: `cc -std=c89 -Wall -Wextra -Werror -pedantic -c cryc-sha3.c -I.` が警告 0。
  **試験は 1 本も流していない**（run-unit.sh・run-ct.sh・check-skeleton.sh・--win32 は全て未実施）。正しさは未確認。

### 自分で決めた点（header が黙っている所）

- 表の data: rho の offset は FIPS 202 Table 2 の値を x+5y の順に並べ直した 25 個（回転は 64 で割った余り）。round 定数は Algorithm 5・6 を
  host の python（build/tmp/p003/rc.py、git 外）で ir=0..23 について評価した 24 個を data として置いた。block ごとに "Values referenced" の出典を書いた。
- permute の作業用の配列は終わりに cryc_wipe。lanes が NULL なら何もしない。
- 使えない sponge（rate が 72・136・168 以外＝未起動・wipe 済み・壊れた物、または position が rate を越える物）への absorb・final は何もしない。
  squeeze は out に 0 を書く（古い memory を出さない）。sponge が NULL なら全関数が何もしない。
- absorb は data が NULL で size>0 なら何もしない（sha2 の update と同じ）。squeeze 開始後の absorb は何もしない。block が満ちたらすぐ permute（absorb の終わりで position < rate）。
- final は SHA-3（suffix 0x06）で未 squeeze の sponge だけ digest を書く（rate 136 なら 32 byte、rate 72 なら 64 byte）。それ以外は何も書かず、どの場合も sponge を wipe する。
  wipe 済みの sponge は rate 0 なので以後の呼び出しは全て何もしない。
- shake_squeeze の最初の呼び出しは size 0 でも padding と permute をして squeezing=1 にする。out が NULL で size>0 でも padding までは行い、書き込みはしない。
- SHAKE 用でない（suffix 0x06 の）sponge を squeeze しても定義された動作（SHA-3 の domain での XOF）にした。
- padding は suffix の byte を position に、0x80 を rate-1 に XOR（同じ byte なら 0x86 / 0x9f になる。FIPS 202 B.2 Table 6）。

### 未実施

- `plan/ws335/tests/unit/test-sha3.c`・`ct-sha3.c` は未作成。
- run-unit.sh sha3、--win32 sha3、run-unit.sh all、run-ct.sh sha3、check-skeleton.sh（BASE=b7dfd3952）、git diff --check: 全て未実施。
- わざと壊す確かめ、coding-style §14 の見直し、ws.md の行の更新: 未実施。

### 次の手順（そのまま再開できる形）

1. `plan/ws335/tests/unit/test-sha3.c` を書く。計画:
   - FIPS 202 の Algorithm 1〜7 を bit 配列（A[5][5][64]、S[64(5y+x)+z] は byte の bit z%8、B.1）で素直に書いた参照の permute を試験の中に持ち、
     cryc_keccak_permute を 0・全 1・乱数の状態（lane は little-endian の 8 byte）で照合。
   - Table 6（B.2）の padding を手で組んだ参照の sponge（block ごとに XOR → 参照 permute）で、4 関数の長さ 0..2*rate+3 を one-shot・分割 absorb・分割 squeeze で照合
     （q=1・2・>2 を含む）。
   - 公式 vector（`WS335_VECTORS/rec/`）: cavp-sha-3bytetestvectors-SHA3_256/512 の ShortMsg・LongMsg・Monte（Seed から MD(i)=SHA3(MD(i-1)) を 1000 回、100 checkpoint）、
     cavp-shakebytetestvectors-SHAKE128/256 の ShortMsg・LongMsg・VariableOut（Outputlen は bit。Len=0 の record は Msg=00 だが空の message）。SHAKE Monte は任意。件数を数えて結果に書く。
   - 誤りの入力: NULL の sponge、NULL の data（size>0）、squeeze 後の absorb（出力不変）、wipe 後・final 後の呼び出し（out 不変・squeeze は 0）、
     SHAKE の sponge への final（何も書かない）、position>rate や rate 不正の壊れた構造体と乱数の構造体（ASan・UBSan で落ちない）、init 後の各 field、clear 後が全 0、構造体の複写後の続行。
2. `plan/ws335/tests/unit/ct-sha3.c`（ct-hash.c の形。秘密の message・seed を CT_SECRET、出力を CT_PUBLIC）を書く。
3. §6 の command を全部流して最後の行を読む（`sh plan/ws335/tests/run-unit.sh sha3`、`--win32 sha3`、`all`、`sh plan/ws335/tests/run-ct.sh sha3`、`BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`、`git diff --check`）。
   Wine の prefix が無ければ /home/awe/zedBSD-worktrees/s-ws335-p002/build/tmp/ws335-wine を build/tmp/ws335-wine に複写して `.ws335-ready` を touch。
4. わざと壊す確かめ（round 定数 1 つ、rho の offset 1 つ、padding の 0x80 の位置、position の境界 `>=` を `>`、chi の ~ の位置）を git の外で行い、FAIL を確かめて戻し `cmp`。
5. この節を実績で書き直し、ws.md の表のこの行を「実装済み・試験 PASS」に。
