<!-- awesome-plan project=zedbsd record=ws335-p008 -->

# ws335-p008: GF(2^255-19) と X25519

Status: cleared（2026-11-01 09:45 Q1 の判定、main 7ebb5367e。Q1 も run-unit f25519 x25519 を流して PASS）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-f25519.h`、`userland/base/libcrypto-compat/cryc-x25519.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc7748.txt`（SHA-256 `279ca0ecc5e92e2962e27b846986aeb74729d9dd34bd4a04a362f80dcb596ad3`）: 4.1、5（ladder、cswap、decode）、5.2（試験の値）、6.1（試験の値）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: wycheproof-x25519_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-f25519.c`、`userland/base/libcrypto-compat/cryc-x25519.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-f25519.c`、`plan/ws335/tests/unit/test-x25519.c`。定数時間の試験: `plan/ws335/tests/unit/ct-x25519.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 負の値の shift の禁止（header）。limb の「loose」と「reduced」の約束を守る。
- 全零の結果は EINVAL（out は書く）。
- cryc_f25519_pow_p58 も（Ed25519 が使う）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh f25519 x25519     # この module
sh plan/ws335/tests/run-unit.sh --win32 f25519 x25519     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh x25519
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

実装済み・試験 PASS（2026-11-01 09:50 担当、Sonnet。cleared は Q1）。host の再起動で一度中断した前の attempt の cryc-f25519.c をそのまま使い（変更なし）、cryc-x25519.c と試験を書いて流した。

### 実行した command と最後の行

WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors で:

- `sh plan/ws335/tests/run-unit.sh f25519 x25519`: `test-f25519: PASS (22903 checks)`、`test-x25519: PASS (4538 checks)`、`run-unit: PASS`（ASan・UBSan）。
- `sh plan/ws335/tests/run-unit.sh --win32 f25519 x25519`（flock /tmp/zedbsd-wine.lock 内）: 同じ件数で `(win32)`、`run-unit: PASS`。
- `sh plan/ws335/tests/run-unit.sh all`: test-chacha 52156、test-f25519 22903、test-hash 12965、test-sha2 14092、test-sha3 629616、test-tools 18、test-util 169160、test-x25519 4538、
  ct-chacha 6、ct-hash 37、ct-sha3 1、ct-util 33、ct-x25519 4、最後 `run-unit: PASS`。
- `sh plan/ws335/tests/run-ct.sh x25519`: `ct-x25519: PASS (4 checks)` / `run-ct: PASS`。
- `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`（flock 内）: `check-skeleton: PASS`。
- `git diff --check`: 出力なし、終了 0。

### 試験の内容

- test-f25519.c: 参照は 16 bit digit の big number（学校算の加算・減算・乗算、2^256 = 38 で畳む還元、p を引く）と、element の値を sum(limb * 2^位置) で出す関数と、
  p - 2・2^252 - 3 の square-and-multiply。入力は範囲内の乱数の limb（reduced は各 limb が 2^幅 以下、loose は 3 * 2^幅 未満。0・最大・最大 - 1 に偏らせる）と境界（0、1、p - 1、p、p + 1、2^255 - 1、全 limb 最大）。
  decode（top bit 無視、p 以上の符号も受理、結果は reduced）、encode（reduced・loose、p と 2p を表す limb で 0、p ± 1）、add・sub・neg の値と範囲（loose）、carry（loose から reduced）、mul・square（reduced と loose の入力、全 limb 最大、out が a・b に重なる）、
  mul_small（0・1・2・65536・121665・2^17 - 1・乱数）、invert（0 は 0、a * a^-1 = 1）、pow_p58、cswap（0・1）、is_zero（p・2p の limb も）、is_negative、1000 段の連鎖（結果を次の入力に）。
- test-x25519.c: RFC 7748 5.2 の 2 つの値と繰り返し（1 回・1000 回。100 万回は run time のため省く）、6.1 の Alice・Bob の公開鍵と共有秘密（両側）、
  Wycheproof x25519_test の 518 件全て（共有秘密が全零の件は EINVAL かつ out が全零、他は 0 と一致）、小さい位数の u（0・1・p・p - 1）、
  性質（scalar の clamp される bit が結果に影響しない、保たれる bit は影響する、u の top bit の無視、p + k と k が同じ点、入力が不変、public が u = 9 の一般関数と同じ、乱数 40 組の DH の一致）、
  keygen（良い source は private が source の byte で public が X25519(private, 9)、失敗する source は EIO で両方が全零）。
- ct-x25519.c: scalar を CT_SECRET にして cryc_x25519（良い u と小さい位数の u。戻り値は呼び出しの後に CT_PUBLIC）、public、秘密の乱数 source からの keygen、秘密の element への全ての体の演算（encode・is_zero・is_negative を含む）。

### わざと壊す確かめ

実装を一時的に書き換え（backup は scratchpad、戻した後に backup と cmp、git diff が空である事で確認）、`run-unit.sh f25519 x25519` を流した。

- FAIL した（21 通り）: twice_p の limb 0 の定数、mul の 19 を 18、canonical の 19 加算を 18、cswap の mask（0 - swap を swap）、decode の top bit の位置（31 を 30）、limb_mask[2]、is_negative の mask（1 を 3）、
  canonical の p 判定（+19 を +18）、invert の連鎖の 2 乗回数、pow_p58 の 2 乗回数、carry_strict の折り返し（19 を 18）、clamp の低 3 bit・bit 254、a24（121666）、全零の EINVAL を消す、
  ladder の開始 bit（253）と終了 bit（> 0）、swap = bit を swap = 0、base point の 9 を 8、keygen の EIO を EINVAL、keygen の失敗時の wipe を消す。
- 定数時間: ladder の cswap を `if (swap != 0U)` で包むと run-ct.sh が FAIL（memcheck の Conditional jump）、戻して PASS。
- PASS のまま（等価な変更で、見逃しではない）: ① `k[31] &= 127` を消す: ladder は bit 254 から下しか読まず bit 255 は結果に効かない。② 最後の cswap を消す: clamp で k の bit 0 は 0 なので最後の swap は常に 0。
- 試験で拾えない（要約）: 全零の検査の loop を 31 byte に切る変更は、最後の byte だけが非零で他が全零になる結果が作れないため（確率 2^-248）試験できない。コードの読みで確認した。

### 自分で決めた点（header が黙っている所）

- cryc-f25519.c（前の attempt）: 10 limb、全 limb を非負に保つ（reduced は各 limb が 2^幅 以下、loose は 3 * 2^幅 未満、sub は 2p を足す）。square は mul(a, a)。
  invert・pow_p58 は x_k = a^(2^k - 1) の連鎖（x250 を共有、p - 2 = (2^250 - 1) * 32 + 11、(p - 5) / 8 = (2^250 - 1) * 4 + 1）。encode は厳密な carry 2 回の後 v + 19 の carry で p 以上を判定し 19 を足して 2^255 の bit を捨てる。
  is_zero・is_negative は canonical な encode を使う。
- cryc-x25519.c: 内部の ladder 関数 1 つに cryc_x25519 と public を載せる。scalar は local に複写して clamp。255 段（bit 254 から 0）。
  全零の検査は 32 byte の OR を cryc_ct_mask_zero32 に通し、`mask & EINVAL` を分岐せずに返す。local の scalar と全ての中間値を cryc_wipe。
  keygen は乱数の失敗で private・public の両方を cryc_wipe して EIO。private は clamp せずに source の byte のまま（header の「32 random bytes as the private scalar」）。
- 試験の参照は x25519 自体の参照 ladder を持たず、RFC と Wycheproof の値、性質（clamp・top bit・DH の一致）、f25519 の参照との照合で支える。

### 未実施

- RFC 7748 5.2 の 100 万回の繰り返し（run time のため）。
- aarch64 Windows の実行（arm64 の Wine が無い。check-skeleton で compile と link のみ）。
- QEMU・実機での確認（この Phase の範囲外）。
