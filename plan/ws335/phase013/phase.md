<!-- awesome-plan project=zedbsd record=ws335-p013 -->

# ws335-p013: ML-KEM の多項式・符号化・sampling

Status: 実装済み・試験 PASS（2026-11-02 01:45 P6 q1881-i01。cleared は Q1）。前: planned（2026-11-01 p001 が作成。起動は Q1。依存: p003）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-mlkem.h（多項式の関数）`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-203.pdf`（SHA-256 `fe1f12f32a7e44ec9fdebbf400cda843a40b506dee676725234dc6f7923b6cac`、印刷の頁 N は PDF の頁 N+9）: 2.3・2.4、4.1（式 (4.1)〜(4.6)）、4.2.1（Algorithm 3〜6、式 (4.7)・(4.8)）、4.2.2（Algorithm 7・8）、4.3・4.3.1（Algorithm 9〜12）、付録 A（zeta の表）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-mlkem-poly.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-mlkem-poly.c`。定数時間の試験: `plan/ws335/tests/unit/ct-mlkem-poly.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- zeta の表は data。付録 A の表と 17^BitRev7(i) mod q の計算の 2 通りで照合。
- 還元は乗算と mask（% と / を秘密に使わない）。
- NTT の試験: 試験の側に R_q の素朴な積（O(n^2)、X^256 + 1 で還元）を書き、NTT^-1(MultiplyNTTs(NTT(f), NTT(g))) と比べる。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh mlkem-poly     # この module
sh plan/ws335/tests/run-unit.sh --win32 mlkem-poly     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh mlkem-poly
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 01:45 JST）P6（Sonnet 5.5）、Queue q1881-i01。worktree p6-ws335b（main ad65fbe3f）。

作った物: `userland/base/libcrypto-compat/cryc-mlkem-poly.c`、`plan/ws335/tests/unit/test-mlkem-poly.c`、`plan/ws335/tests/unit/ct-mlkem-poly.c`。header・試験の道具・他の module は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh mlkem-poly`（ASan・UBSan） | `test-mlkem-poly: PASS (10117 checks)` / `run-unit: PASS`（約 5 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 mlkem-poly`（x86_64、Wine） | `test-mlkem-poly: PASS (10117 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh mlkem-poly` | `ct-mlkem-poly: PASS (1 checks)` / `run-ct: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 28 本 PASS（`test-mlkem-poly` を含む）、最後の行 `run-unit: PASS`（約 7 分 40 秒） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

実装の中身（FIPS 203）:

- 還元: `mlkem_reduce`（32 bit の値を Barrett の固定乗数 floor(2^32/q) = 1290167 と mask の 1 回の引き算で [0, q-1] へ。% と / は使わない）。係数の読み込み `mlkem_load` は 16 bit の保存値を符号なしにして還元（範囲外の値でも定義された結果）。
- NTT（Algorithm 9）・NTT^-1（Algorithm 10、最後に 3303 を掛ける）・MultiplyNTTs（Algorithm 11・12）。zeta の表 `mlkem_zeta[128]`（付録 A の 1 つ目の配列）を data として block の直前に出典の comment 付きで置いた。付録 A の 2 つ目の配列（gamma = zeta^(2 BitRev7(i)+1)）は 1 つ目の 64 + i/2 番の値で、i が奇数なら q から引いた値（付録 A の 2 つの配列の関係を試験の中で照合）。
- ByteEncode・ByteDecode（Algorithm 5・6）: bit の蓄積器。d = 12 の ByteDecode は 12 bit の値（2q 未満）を mask の 1 回の引き算で還元し、全部が q 未満なら 1 を返す。
- Compress・Decompress（式 (4.7)・(4.8)）: Compress は floor((2^d x + 1664) / q) mod 2^d を、固定乗数 ceil(2^35/q) = 10321340 の掛け算と shift で（分子が 2^23 未満の全部で正確。python で全数確認し、試験でも全 x・全 d を照合）。Decompress は (q y + 2^(d-1)) >> d。
- SampleNTT（Algorithm 7）: SHAKE128 の出力を 168 byte の block ずつ読み、3 byte の組を順に処理。SamplePolyCBD（Algorithm 8）: eta 2 と 3。

試験の中身（10117 checks）:

- NTT: 定義（f を X^2 - 17^(2 BitRev7(i)+1) で割った余り。17 のべきは試験の側で計算）との比較を、256 個の delta・q-1 の delta・定数・乱数 150・範囲外の乱数 40 で。X^2 の NTT の偶数番が付録 A の 2 つ目の配列（128 値を試験の中に data として転記、出典の comment 付き）と一致。X・1 の NTT。NTT^-1: delta 256 個と乱数の NTT^-1 を定義の NTT に通すと元に戻る、NTT^-1(NTT(f)) = f と逆、範囲外の値。
- 積: Algorithm 12 の式との比較（範囲外の入力を含む 120）、NTT^-1(NTT f * NTT g) と素朴な R_q の積（X^256 = -1）の比較 40、単項式の積（X^255 * X = -1 など）、1 と 0 の積、h が f・g・両方と同じ場合、交換法則、入力が変わらないこと。
- 符号化: 手計算の例（d = 1・4・10・11・12、最後の係数、境界 3328・3329・4095）、全 d の乱数往復と bit ごとの素朴な Algorithm 5・6 との比較（exact-size の malloc で範囲外の読み書きを ASan が拾う）、q 以上の値を 1 つ入れた時の flag と還元、不正な d（0・13・16・32・100・0xffffffff）、範囲外の係数。
- Compress・Decompress: 全 x と全 d を整数の除算の式と比較、Decompress 後の Compress が元に戻る、(4.9) の誤差の上限、手計算の値（Compress_1 の境界 832・833・2496・2497、Decompress_1(1) = 1665 など）、範囲外の 16 bit の乱数を全 d で、不正な d で変化なし。
- SamplePolyCBD: 手計算の例（eta 2: 0x03・0x0c・0x41・0x25、全 0・全 0xff・0x33・0xcc、eta 3: 0x07・0x38・bit 6 をまたぐ例）、byte 0 の全 256 値、乱数 eta 2・3 を bit ごとの素朴な Algorithm 8 と比較、不正な eta で 0 に。SampleNTT: Algorithm 7 を 3 byte ずつ読む形で試験の側に書いた物との比較（構造のある seed と乱数の 200 個）、全係数が q 未満、index byte が違うと別の多項式。
- 公式 vector: Wycheproof ML-KEM-768 を、試験の側で K-PKE.KeyGen（Algorithm 13）と K-PKE.Encrypt（Algorithm 14）をこの module の関数から組んで通す。keygen_seed 100 件（ek と dk の 1152 byte・H(ek)・z）、mlkem_768_test の seed つき valid（100 件超、ek）、encaps の valid 133 件（c と K を G(m || H(ek)) から）、ModulusOverflow 12 件（ByteDecode12 の flag が 0）、keygen の ek 100 件が clean に往復。
- 定数時間（ct-mlkem-poly）: NTT・NTT^-1・MultiplyNTTs・Compress・Decompress・ByteEncode・ByteDecode（全 d、flag も）・SamplePolyCBD を、秘密を CT_SECRET にして memcheck で確認。SampleNTT は公開の seed なので対象外。

わざと壊す確かめ（git の外。`build/tmp/p013/brk.py`、.c を `build/tmp/p013/brk/m-N/` に複写して書き換え、-O2・sanitizer 無しで build して流す。実装 file は触らず最後に sha256 が変わっていないことを script が確認）。111 通り。判定は build の誤り（BUILD-ERROR）、試験の失敗（`test-mlkem-poly: FAIL (...)`）、crash、見逃し（PASS）を区別した。build の誤りは 0、crash は 0。

- 単体試験の対象 100 通り: 93 通りが試験の失敗（`FAIL (n of 10117 failed)`）で落ちた（内訳: 定数と表 11（Barrett の乗数 -4000、Compress の乗数 -1、3303、zeta の 5 か所、SampleNTT の定数 2、q）、load・zero 2、reduce 4、NTT 10、NTT^-1 8、積 9、encode 6、decode 12、Compress 9、Decompress 5、SampleNTT 8、CBD 9。件数は brk2.log の行を数えた値）。sanitizer 付きの run-unit では、さらに 2 通り（decode の refill が 1 byte 先を読む、SampleNTT の d2 が 256 番を書く）が ASan の範囲外の検出で落ちる（sanitizer 無しの build では結果が同じで見逃し。run-unit は常に ASan・UBSan 付き）。1 つ（Compress の入力を還元しない）は最初の試験を通ったため、範囲外の 16 bit の乱数を全 d で Compress・Decompress する試験を足して落ちるようにした。
- 見逃し 5 通り（観測できる違いが無い物）: (1) Barrett の乗数 -1（1290166）: 到達する入力は 2.3 千万未満で、剰余の推定が 2q 未満のまま（等価）。(2) Compress の割り算の乗数 +1（10321341）: 到達する分子（還元した入力から作る 2^23 未満）で floor が同じ（等価）。(3) 積で a1 の読み込みの還元を外す: 後の乗算が 32 bit に収まり同じ値（等価、還元は桁あふれの防御）。(4) encode で `value << (count & 7)`: その時点で count は 8 未満（等価）。(5) SampleNTT の 64 block に達した時に f を 0 にしない: 確率 2^-261 未満で到達できず、試験から作れない。
- 定数時間の確かめの感度（valgrind、11 通り）: 秘密の係数・byte で表を引く（NTT・NTT^-1・積・Compress・Decompress・CBD、volatile の sink で最適化が消さない形）と、秘密で分岐する（encode・decode・CBD）の 10 通りが memcheck で落ちた。reduce の最後の引き算を `while` にした物も落ちた。decode の還元を `while (value >= q)` に替えた 1 通りは見逃し（値が 4095 以下と分かり optimizer が分岐の無い形にする。memcheck で見えない）。最初に試した `if` に替えた形は optimizer が cmov にして全部見逃したので、上の形に作り直した。

自分で決めた点（header が黙っていた所）:

1. 不正な bit 幅: ByteEncode は d が 1〜12 の外なら何も書かない、ByteDecode は d が外なら f を 0 にして 0 を返す、Compress・Decompress は d が 1〜11 の外なら f を変えない（header は「d below 12」とだけ書く）。
2. 範囲外の係数（負・q 以上の 16 bit の値）: NTT・NTT^-1・MultiplyNTTs・Compress は先に q で還元して定義された結果にする、ByteEncode は下位 d bit を取る、Decompress は入力の下位 d bit を取る。未定義の動作は無い。
3. ByteDecode の d が 12 未満の時の戻り値は常に 1。
4. SampleNTT の while の上限: 168 byte の block を最大 64 個（付録 B の下限 280 回に対して 3584 回）。超えたら f を全 0 にする（付録 B の「中間の値を破棄」）。戻り値の無い関数なので誤りは返さない。
5. SamplePolyCBD は eta 2 に加えて eta 3（標準の 2 値）も受け、他の eta は f を 0 にする。
6. NTT の各 butterfly で毎回完全に還元する（lazy reduction は使わない。単純さと定数時間の確認のため）。MultiplyNTTs の gamma は付録 A の 2 つ目の配列を持たず、1 つ目の配列から 64 + i/2 番を取り、奇数の i で符号を変える（i は公開の値なので分岐してよい）。

phase.md と仕様の食い違い・header の不備: 見つからなかった。header の「d for the polynomial functions」の細部（不正な d の扱い）は上の 1 のとおり決めた（規約の見直しの Phase で header の comment に足してよい）。

未実施: p014 が使う K-PKE 全体と ML-KEM の関数（試験の側の薄い再実装で Algorithm 13・14 を確認したのみ。Wycheproof の decaps・暗黙の拒否の vector は p014）、vectors.py が変換しない形式の vector は使っていない（今回の範囲では不要）、性能の測定、32 bit の size_t の機械・aarch64 Windows の実行（build のみ）、実機・QEMU（範囲外）。

commit: dcc00c9634（実装・試験・ct 試験）、5cf3ef3cec（Compress の範囲外入力の試験の追加と、この結果の節）。この行を足す commit が最後。
