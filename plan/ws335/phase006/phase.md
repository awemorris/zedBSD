<!-- awesome-plan project=zedbsd record=ws335-p006 -->

# ws335-p006: ChaCha20・Poly1305・AEAD（と 64 bit の counter の ChaCha20）

Status: cleared（2026-11-01 09:23 Q1 の判定、main d0e048532。Sonnet 実装済み・試験 PASS、Q1 も run-unit chacha を流して PASS）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-chacha.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8439.txt`（SHA-256 `25bef70fbf7a07ff45c2fe4cb7c6ce954eac687413d8610603268b4e4415324c`）: 2.1〜2.8（2.3.2・2.4.2・2.5.2・2.6.2・2.8.2 の試験の値）、付録 A。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: wycheproof-chacha20_poly1305_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-chacha.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-chacha.c`。定数時間の試験: `plan/ws335/tests/unit/ct-chacha.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- Poly1305 は 26 bit の 5 limb と 64 bit の積（128 bit の型を使わない）。最後の還元と tag の比較は mask。
- cryc_chacha20_xor64 の試験: counter が 2^32 未満なら、RFC 8439 の形で nonce12 = (counter の上位 32 bit の LE 4 byte) || nonce8 とした keystream と同じ（手で導いた関係）。counter が 2^32 を跨ぐ所も 2 回の block 関数の呼び出しと比べる。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh chacha     # この module
sh plan/ws335/tests/run-unit.sh --win32 chacha     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh chacha
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

実装済み・試験 PASS（2026-11-01 09:30 担当、Sonnet。cleared は Q1）。host の再起動で一度中断し、cryc-chacha.c は前の attempt の物をそのまま使い（変更なし）、試験を書いて流した。

### 実行した command と最後の行

WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors で:

- `sh plan/ws335/tests/run-unit.sh chacha`: `test-chacha: PASS (52156 checks)` / `run-unit: PASS`（ASan・UBSan）。
- `sh plan/ws335/tests/run-unit.sh --win32 chacha`（flock /tmp/zedbsd-wine.lock 内）: `test-chacha: PASS (52156 checks) (win32)` / `run-unit: PASS`。
- `sh plan/ws335/tests/run-unit.sh all`: test-chacha 52156、test-hash 12965、test-sha2 14092、test-sha3 629616、test-tools 18、test-util 169160、ct-chacha 6、ct-hash 37、ct-sha3 1、ct-util 33、最後 `run-unit: PASS`。
- `sh plan/ws335/tests/run-ct.sh chacha`: `ct-chacha: PASS (6 checks)` / `run-ct: PASS`。
- `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`（flock 内）: `check-skeleton: PASS`。
- `git diff --check`: 出力なし、終了 0。

### 試験の内容（plan/ws335/tests/unit/test-chacha.c、ct-chacha.c）

- RFC 8439 の値: 2.3.2（block）、2.4.2（cipher、32 bit と 64 bit の両方の形。64 bit の形は nonce12 の先頭 4 byte を counter の上位 32 bit に、後ろ 8 byte を nonce8 に写して同じ keystream になる事で照合）、
  2.5.2（Poly1305）、2.6.2（key 生成）、2.8.2（AEAD。nonce は 07 00 00 00 と IV。one-time key は block 0 の先頭 32 byte）、A.1（5 件）、A.2（3 件）、A.3（11 件）、A.4（3 件）、A.5（復号）。
  A.3 は全ての分割点の 2 分割、短い message は全ての 2 点の 3 分割、1 byte ずつ。2.3.2 の block を xor64 でも照合（counter = 0x0900000000000001、nonce8 = 00 00 00 4a 00 00 00 00）。
- Wycheproof chacha20_poly1305_test: file の 325 件のうち ivSize 96・tagSize 128・keySize 256 の 316 件を検査（件数を CHECK で完全一致）、他の nonce 長の 9 件は header の interface に無いので skip。
  valid（256 件）は seal が ct と tag に一致、open が msg に一致（out of place と in place）。invalid（60 件）は open が EBADMSG、out が全て 0、in は不変（in place は 0 で置換）。
- 参照 1（RFC 8439 2.5 の式そのもの）: 40 byte の big integer（加算・乗算・減算・P = 2^130 - 5 の還元）で acc = ((acc + block + 0x01) * r) % P、clamp、s の 2^128 での加算。
  RFC の vector で参照自身も一致する事を確認した上で、乱数の key と message（長さ 0〜299、全 0xff の message、全 0xff の r、1500 回）と 0xff の message の長さ 0〜130 を、一括・乱数の分割（0 byte の piece は NULL）で実装と照合。
- 参照 2（2.8 の message の構成）: block 0 から one-time key、block 1 から keystream、aad・padding・ct・padding・長さ 2 つを組んで参照 Poly1305 に通した tag と ct を、実装の seal と照合（boundary の長さ 23 通りと乱数の長さ、aad 長 13 通りと乱数、800 回）。
- keystream: 長さ 24 通り × 32 bit counter 7 通り（0・1・0x7fffffff・0xfffffff0 から 0xffffffff）と、64 bit counter 12 通り（2^32 の手前と向こう、2^64 - 1 の手前）で、block 関数を 1 block ずつ呼んだ参照と照合。
  counter が上限を越える組は EOVERFLOW と out が不変（sentinel）、in place は buffer が不変。size 0 は最後の counter でも 0。size_t が 64 bit の時、上限 +1 byte の巨大な size（counter 0・1・0x80000000）が EOVERFLOW。
- 誤りの入力: 全ての key（256 bit）・nonce（96 bit）・aad・ct・tag の 1 bit 反転が EBADMSG（out は size byte が 0、out の後ろの guard byte は不変、in は不変、in place は 0 で置換）、ct を 1 byte 切る・aad を 1 byte 切る・aad を空に・ct を空に。
  乱数の key・nonce・aad・ct の 1 bit 反転（800 回）。seal・open の size が (2^32 - 1) * 64 byte を 1 byte 越えると EOVERFLOW で何も書かれない（out・tag が不変）、size_t の最大でも同じ。
- 往復: seal の in place と out of place、open の in place と out of place、tag・out の後ろの guard byte。Poly1305 final の後に構造体が全 0。init が key を変えない。
- 定数時間（ct-chacha.c）: key・nonce・nonce8・plaintext・aad・Poly1305 の key を CT_SECRET（block、xor（32 bit と 64 bit の counter が 2^32 を跨ぐ）、Poly1305 を block の境で分割、seal、open の良い tag と悪い tag。open の戻り値は呼び出しの後に CT_PUBLIC して分岐）。

### わざと壊す確かめ（実装を git の外で壊し、FAIL を確かめ、cmp で戻した）

run-unit.sh chacha が FAIL した 25 通り: 定数語 1 つ、回転 7→8、対角の index、xor の境 `>`→`>=`、xor の上限を 1 block 少なく、xor64 の境 `>`→`>=`、xor64 の上位 counter の繰り上がりを外す、xor64 の nonce の位置、
clamp の byte 3 の mask（15→31）、clamp の byte 12 の mask（252→253）、Poly1305 の full block の bit（2^24→2^25）、最後の還元の mask の反転、h[4] からの繰り上がりの 5 倍を 4 倍、s の加算の繰り上がりを外す、
update の block 満杯の判定 `<`→`<=`、block の乗算後の繰り上がりの 5 倍を 4 倍、aad の padding を常に付ける、長さ 2 つの入れ替え、seal の counter を 1→0、
open の mask の反転、open の戻り値の反転、open が悪い tag でも平文を書く、tag の比較を 15 byte に、seal の size の上限 +1 block、open の size の上限 +1 block。
定数時間は run-ct.sh chacha が FAIL した 3 通り（open で computed[0] による分岐、block で key[0] による分岐、Poly1305 final の word[0] による分岐。memcheck が条件分岐を報告）。
全て戻した後 `cmp` で元（build/tmp/p006-mut/orig.c）と一致。

**見逃し 1 件（試験で検出できない）**: AEAD の size の上限 `AEAD_MAX_BLOCKS` を 0xffffffff から 0xfffffffe に変えても PASS。差が出るのは size がちょうど (2^32 - 1) * 64 byte（約 256 GiB）の時で、
そのような message を実際に seal・open する試験は現実的でない（上限 +1 byte が EOVERFLOW になる側だけを試験で確かめている）。試験の追加では解消できないので、そのまま残す。

### 自分で決めた点（header が黙っている所）

実装は前の attempt の物のままで、試験で確かめた内容の一覧（実装の comment にも書いてある）:

- open は tag の比較結果を分岐に使わず、`cryc_ct_equal` の 0/1 から mask を作り、復号の各 byte を `平文 & mask` で書く（不一致なら out は 0。平文は一度も書かれない）。返す値も mask から算術で作る（EBADMSG か 0）。
- seal・open の size の上限は blocks = ceil(size/64) <= 2^32 - 1。上を越えたら何も書かずに EOVERFLOW（open は tag の計算の前に判定）。
- xor64 の上限は ceil(size/64) - 1 <= 2^64 - 1 - counter。size 0 は常に 0。xor の上限は size <= (2^32 - counter) * 64。
- struct cryc_poly1305 の `used` が 16 以上の壊れた構造は防御していない（呼び出し側の不変条件）。
- Poly1305 の最後の還元は、limb 1 だけが 2^26 を越えうる前提で 1 周の carry の後に g = h + 5 - 2^130 を作り、mask で選ぶ。
- 試験側: Wycheproof の件数（316 と 9）を CHECK で固定した。壊す確かめの mutation script は build/tmp/p006-mut/mutate.py（git の外）。

### 未実施

- aarch64 Windows は check-skeleton の compile・link のみ（実行なし）。
- AEAD の size の上限ちょうど（約 256 GiB）の message の seal・open は現実的でなく未実施（上記の見逃し 1 件）。
- 実機・QEMU は本 Phase の範囲外。
- coding-style §14 の見直しは自分の diff の目視まで（規約の全文の見直しの Phase で改めて）。
