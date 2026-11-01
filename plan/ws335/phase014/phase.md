<!-- awesome-plan project=zedbsd record=ws335-p014 -->

# ws335-p014: K-PKE と ML-KEM-768

Status: cleared（2026-11-02 02:04 Q1、main f533fd4cd。main で test-mlkem 8135・ct-mlkem PASS。Wycheproof 4 file 全部、暗黙の拒否も一致）。前: 実装済み・試験 PASS（2026-11-02 02:05 P6 q1894-i01。cleared は Q1）。前: planned（2026-11-01 p001 が作成。起動は Q1。依存: p013）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-mlkem.h（鍵の生成・encaps・decaps）`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-203.pdf`（SHA-256 `fe1f12f32a7e44ec9fdebbf400cda843a40b506dee676725234dc6f7923b6cac`、印刷の頁 N は PDF の頁 N+9）: 5（Algorithm 13〜15）、6（Algorithm 16〜18）、7（Algorithm 19〜21、7.2・7.3 の確かめ）、8（Table 2・3）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: wycheproof-mlkem_768_keygen_seed_test・mlkem_768_encaps_test・mlkem_768_semi_expanded_decaps_test・mlkem_768_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-mlkem.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-mlkem.c`。定数時間の試験: `plan/ws335/tests/unit/ct-mlkem.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- decaps の c と c' の比較と K と K-bar の選択は定数時間。
- ek の modulus check、dk の hash check。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh mlkem     # この module
sh plan/ws335/tests/run-unit.sh --win32 mlkem     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh mlkem
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 02:02 JST）P6（Sonnet 5.5）、Queue q1894-i01。worktree p6-ws335b（main 290dfb1d0）。

作った物: `userland/base/libcrypto-compat/cryc-mlkem.c`（骨格の置き換え）、`plan/ws335/tests/unit/test-mlkem.c`、`plan/ws335/tests/unit/ct-mlkem.c`。header・試験の道具・他の module は変えていない。実装の commit は 74838e0630。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh mlkem`（ASan・UBSan） | `test-mlkem: PASS (8135 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 mlkem`（x86_64、Wine） | `test-mlkem: PASS (8135 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh mlkem` | `ct-mlkem: PASS (4 checks)` / `run-ct: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 34 本（test 20 本と ct 14 本）全部 PASS、最後の行 `run-unit: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

実装（FIPS 203 のアルゴリズム 13〜21）:

- K-PKE.KeyGen は `cryc_mlkem768_keygen_internal` に inline。行列 A は保持せず、積に使う瞬間に rho から SampleNTT で作る（アルゴリズム 13・14 で各要素は 1 回しか使われない）。誤差 e[i] も使う直前に標本化（nonce は s の後の 3 から）。
- K-PKE.Encrypt は y を 3 本保持して NTT、u[i] = NTT^-1(sum_j A[j][i] y[j]) + e1[i] を圧縮して c1、v = NTT^-1(t^T y) + e2 + mu を圧縮して c2。nonce は y が 0〜2、e1 が 3〜5、e2 が 6。
- K-PKE.Decrypt は u'[i] を decode・decompress・NTT して s^T u' を蓄積、w = v' - NTT^-1(...)、Compress_1 して ByteEncode_1。
- Decaps は m' = Decrypt、(K', r') = G(m' || h)、K-bar = J(z || c)、c' = Encrypt(ek, m', r')、`cryc_ct_equal` と `cryc_ct_mask_eq32` と `cryc_ct_select_bytes` で K' か K-bar を選ぶ（暗黙の拒否、分岐なし）。
- `cryc_mlkem768_check_dk` は H(ek 部) と保存の h を `cryc_ct_equal` で比べ、結果を分岐なしの mask で 0 か EINVAL にする。`cryc_mlkem768_check_ek` は 3 本の ByteDecode12 の clean flag（公開の値なので最初の誤りで止める）。
- 秘密を載せる局所の配列と多項式は全ての return の前に `cryc_wipe`。keygen の乱数の失敗は ek・dk を、encaps の失敗（EINVAL・EIO）は shared・ciphertext を wipe。

試験（8135 checks）:

- 公式 vector: Wycheproof ML-KEM-768。keygen_seed 100 件（seed から ek・dk がバイトまで一致、dk の配置、check_ek・check_dk が 0）、encaps 133 件の valid（Encaps_internal と、m を返す乱数で Encaps の両方が c・K と一致）、encaps の invalid のうち ek が 1184 byte の 112 件（not reduced 100 と ModulusOverflow 12）は check_ek と Encaps が EINVAL で出力を wipe・乱数を引かない、長さだけの誤りの 20 件は呼び出し側の確認なので数えるだけ、general 161 件の valid（seed から鍵を作り ek 一致、check_dk、Decaps が K。うち random ciphertext と bit flip は暗黙の拒否の鍵で、J(z||c) を SHAKE256 から試験の側で計算した値と 20 件以上で一致）、general の長さの誤り 40 件は数えるだけ、semi-expanded decaps の valid 3 件（zeroed byte の MalleableCiphertext 2 件を含む）が K と一致、InvalidDecapsulationKey 2 件が check_dk で EINVAL、長さだけの 4 件は数えるだけ。件数は試験の中で CHECK している。
- 自作: 表 3 の大きさ、乱数つき KeyGen（d・z の順、2 回の呼び出し、どちらの失敗でも EIO と wipe、乱数が途中で尽きても EIO）、Encaps の乱数の失敗、check_ek（各多項式の先頭・2 番目・254・255 番の係数に q-1 は通り、q・q+1・4095 は EINVAL、rho は対象外）、check_dk（dk の全 2400 byte を 1 bit ずつ反転し、1152〜2367 だけが EINVAL）、暗黙の拒否（c の全 1088 byte を順に反転、最初と最後の byte の全 bit、全 0・全 0xff・乱数 30 件が全て J(z||c') と一致し K と違う）、壊れた dk（z を替えると有効な c は K のまま・不正な c は新しい z の鍵、dk の秘密部を 3 本それぞれ壊すと check_dk は通り復号が外れて拒否の鍵）、往復 40 件（Encaps_internal・Decaps・乱数つき Encaps、決定性、別の m で別の c・K、m が全 0・全 0xff、別の鍵での Decaps は拒否の鍵）。全て exact-size の malloc で ASan が範囲外を拾う。

定数時間（ct-mlkem、valgrind memcheck）: keygen_internal（z を秘密）、encaps_internal（m を秘密）、decaps（dk の dkPKE と z を秘密、有効な c と壊れた c の両方）、check_dk（dk 全部を秘密、結果は mask から作った値）が PASS。制約: keygen の seed d を秘密にすると rho（= ek の一部、公開）が SampleNTT の棄却の分岐に入り memcheck が報告する（cryc-mlkem-poly.c の sample_ntt で確認、公開の値の分岐で仕様どおり）ので、d は公開にして z だけ秘密にした。d から作る s・e の係数 NTT・CBD は p013 の ct と、decaps の再暗号化（m' 由来の y・e1・e2 を秘密として流す）で確認している。

わざと壊す確かめ（git の外。`build/tmp/p014/brk.py`、.c を `build/tmp/p014/brk/m-N/` に複写して書き換え、-O2・sanitizer 無しの build で流す。実装 file は触らず最後に sha256 が変わっていないことを script が確認。判定は BUILD-ERROR・CRASH・TEST-FAIL・CT-FAIL・MISSED を区別した）。123 通り、build の誤りは 0。

- 単体試験の対象 108 通り（定数 9、keygen 16、keygen の乱数 4、check_ek 5、check_dk 6、encaps_internal・encaps 10、decaps 11、J・G・H・noise 7、add・sub・行列の seed 9、encrypt 20、decrypt 11）のうち 103 通りが `test-mlkem: FAIL (n of 8135 failed)` で落ち、1 通り（DV 4→5）が crash（signal 6、build は成功、範囲外の書き込みで abort）。
- 見逃し 4 通りは全て等価な変異: (1) decrypt の `v - x` を `x - v` にする（Compress_1 が x と q-x に対し対称で、全入力で同じ bit。等価）、(2) mlkem_poly_sub の差に +1（復号の雑音の余裕の内、観測できない）、(3)(4) mlkem_poly_sub と decrypt の差から masked subtraction を外す（Compress が入力を先に還元するので同じ。sub の loop を 1 つ短くする変異は落ちる）。
- 定数時間の対象 15 通り（変異の番号 108〜122）のうち 14 通りが memcheck で落ちた: 秘密に依存する分岐（decaps の comparison の結果・m'・z・dk の byte、encrypt の m・r、keygen の z、check_dk の hash の byte）、秘密で引く table（m'、sub・add の係数）、sub の中の分岐、decaps の ciphertext 比較を `memcmp` に替えた形、check_dk と decaps の mask の volatile 書き込みの分岐。1 通り（check_dk の `if (equal == 0) error = EINVAL; else error = 0;`）は optimizer が分岐の無い形にするため見逃し（memcheck で見える差が無い。volatile の書き込みを入れた形は落ちる）。

自分で決めた点（header が黙っていた所）:

1. 行列 A を保持せず都度 SampleNTT。誤差 e も使う直前に標本化（nonce の順は仕様のまま）。
2. `cryc_mlkem768_keygen` の乱数は d と z を 2 回に分けて引く（アルゴリズム 19 の順）。どちらの失敗でも ek と dk を全部 wipe して EIO。
3. `cryc_mlkem768_encaps` は先に check_ek を行い、EINVAL なら乱数を引かず shared と ciphertext を wipe して返す（header は EIO のときだけ wipe と書く。EINVAL の wipe は安全側）。
4. `cryc_mlkem768_check_dk` は分岐なしで 0 か EINVAL を作る（header の「Constant time in dk」のため）。dk の 7.3 の hash check だけで、埋め込みの ek の modulus check はしない（header どおり）。
5. decaps は dk の ek が check 済みとして byte_decode の clean flag を見ない（秘密に依存する分岐を避ける）。
6. K-PKE の三つ（KeyGen・Encrypt・Decrypt）のうち公開の関数にするのは header の宣言の分だけ。Encrypt・Decrypt は static。

phase.md と仕様の食い違い・header の不備: 見つからなかった。

未実施: 性能の測定、32 bit の size_t の機械・aarch64 Windows の実行（check-skeleton で build のみ）、実機・QEMU（範囲外）、decaps の入力の長さの確認（header どおり呼び出し側）。
