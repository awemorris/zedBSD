<!-- awesome-plan project=zedbsd record=ws335-p003 -->

# ws335-p003: SHA-3・SHAKE（Keccak）

Status: cleared（2026-11-01 09:04 Q1 の判定、main b4c57515e。08:55 Sonnet 実装済み・試験 PASS、Q1 も run-unit sha3 を流して PASS）
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

実装済み・試験 PASS（2026-11-01 08:55 担当、Sonnet。cleared は Q1）。

### 実行した command と最後の行

WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors で:

- `sh plan/ws335/tests/run-unit.sh sha3`: `test-sha3: PASS (629616 checks)` / `run-unit: PASS`（ASan・UBSan）。
- `sh plan/ws335/tests/run-unit.sh --win32 sha3`（flock /tmp/zedbsd-wine.lock 内）: `test-sha3: PASS (629616 checks) (win32)` / `run-unit: PASS`。
- `sh plan/ws335/tests/run-unit.sh all`: test-hash 12965、test-sha2 14092、test-sha3 629616、test-tools 18、test-util 169160、ct-hash 37、ct-sha3 1、ct-util 33、最後 `run-unit: PASS`。
- `sh plan/ws335/tests/run-ct.sh sha3`: `ct-sha3: PASS (1 checks)` / `run-ct: PASS`。
- `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`（flock 内）: `check-skeleton: PASS`。
- `git diff --check`: 出力なし、終了 0。

### 試験の内容

- 参照: FIPS 202 の Algorithm 1〜7 をビット配列で書いた permute（rho の offset は経路 (x,y)→(y,(2x+3y) mod 5) と (t+1)(t+2)/2 から、round 定数は Algorithm 5・6 の rc から計算。実装の表とは独立）と、B.2 の padding を byte 列に当てた sponge。
  cryc_keccak_permute を 0・全 1・各 lane の最下位と最上位の 1 bit・乱数 40 個・0 から 10 回連鎖で照合。
  4 関数の長さ 0..2*rate+3 を、一括・分割 absorb・分割 squeeze（出力 1 byte から 3*168+5 byte まで、q=1・2・>2）で照合（SHA-3 を XOF として使う場合も）。100000 byte の message も。
- 公式 vector（NIST CAVP byte 版）: SHA3-256 ShortMsg 137・LongMsg 100、SHA3-512 ShortMsg 73・LongMsg 100、SHAKE128 ShortMsg 337・LongMsg 100・VariableOut 1126、SHAKE256 ShortMsg 273・LongMsg 100・VariableOut 1246
  （各 record を 16 通りの absorb の分割と出力の分割で）。Monte: SHA3-256・SHA3-512 各 100 checkpoint（1000 回の連鎖）、SHAKE128・SHAKE256 各 100 checkpoint（message は前の出力の先頭 16 byte、次の長さは最後の 2 byte の big-endian を範囲で割った余り）。
  vector の record は計 3992 件（件数は test が各ファイルで完全一致を CHECK）。
- 誤りの入力: NULL の sponge の全関数、NULL の data（size>0、size=SIZE_MAX）、squeeze 開始後の absorb（構造体不変）、final・clear 後の全呼び出し（構造体は全 0、squeeze は 0 を出力、final は何も書かない）、
  SHAKE への final（何も書かず wipe）、squeeze 開始済みの SHA-3 への final、out が NULL の final と squeeze、size 0 の最初の squeeze（入力を閉じる）、構造体の複写後の続行、init が汚れた構造体を完全に初期化すること、
  rate が不正・position が範囲外の壊れた構造体（rate 16 通り × position 多数）と完全に乱数の構造体 3000 個（ASan・UBSan で落ちない。使えない物は absorb・squeeze で不変、final は常に wipe）。
- 定数時間: 秘密の message・seed（SHA3-256・SHA3-512・SHAKE128・SHAKE256、block の境をまたぐ分割）と秘密の permute の状態を CT_SECRET、出力を CT_PUBLIC。

### わざと壊す確かめ（実装を git の外で壊し、FAIL を確かめ、cmp で戻した）

全て run-unit.sh sha3 が FAIL（13 通り）: 5 番目の round 定数 0x...808b→0x...808a、rho の offset 36→37、pad の 0x80 を rate-1→rate-2、absorb の block 満杯の判定 `>=`→`>`、chi の ~ の位置の入れ替え、SHAKE の suffix 0x1f→0x0f、
squeeze の block 使い切りの判定 `>=`→`>`、theta の回転 1→63、使える sponge の判定 `position > rate`→`>=`、rotate_left の `% 64` を外す（UBSan）、SHA3-512 の digest を 32 byte に、lane 単位の xor-in で load を 1 bit ずらす、pi の index。
定数時間: permute の末尾に `lanes[0] & 1` の分岐（volatile への加算）を足すと run-ct.sh sha3 が FAIL（memcheck が cryc-sha3.c の該当行の条件分岐を報告）。戻した後 `cmp` で元と一致、run-ct.sh sha3 が PASS に戻った。
見逃しは無く、試験の追加は無し。

### 自分で決めた点（header が黙っている所）

- 表の data: rho の offset は FIPS 202 Table 2 の値を x+5y の順に並べ直した 25 個（回転は 64 で割った余り）。round 定数は Algorithm 5・6 を ir=0..23 について評価した 24 個を data として置いた（試験の ref_rc が独立に再計算して照合）。block ごとに "Values referenced" の出典を書いた。
- permute の作業用の配列は終わりに cryc_wipe。lanes が NULL なら何もしない。
- 使えない sponge（rate が 72・136・168 以外＝未起動・wipe 済み・壊れた物、または position が rate を越える物）への absorb・final は何もしない。squeeze は out に 0 を書く。sponge が NULL なら全関数が何もしない。
- absorb は data が NULL で size>0 なら何もしない。squeeze 開始後の absorb は何もしない。block が満ちたらすぐ permute（absorb の終わりで position < rate）。
- final は SHA-3（suffix 0x06）で未 squeeze の sponge だけ digest を書く（rate 136 なら 32 byte、rate 72 なら 64 byte）。それ以外は何も書かず、どの場合も sponge を wipe する。
- shake_squeeze の最初の呼び出しは size 0 でも padding と permute をして squeezing=1 にする。out が NULL で size>0 でも padding までは行い、書き込みはしない。
- SHA-3 の sponge への shake_squeeze も定義された動作（domain bits 01 の XOF）にした（試験が参照で照合）。header は SHAKE の sponge への使用だけを述べる。
- padding は suffix の byte を position に、0x80 を rate-1 に XOR（同じ byte なら 0x86 / 0x9f になる。B.2 Table 6）。

### 未実施

- aarch64 Windows は check-skeleton の compile・link のみ（実行なし）。
- coding-style §14 の見直しは自分の diff の目視まで（規約の全文の見直しの Phase で改めて）。
