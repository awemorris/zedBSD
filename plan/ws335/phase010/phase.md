<!-- awesome-plan project=zedbsd record=ws335-p010 -->

# ws335-p010: P-256・P-384 の群

Status: 実装済み・試験 PASS（2026-11-01 23:12 P6。cleared は Q1。依存: p007）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-ecp.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-sp-800-186.pdf`（SHA-256 `53c7cf3340528aaa0a7bbeaef947f2099568192367d4341a7d4abceb55f78432`、印刷の頁 N は PDF の頁 N+11）: 3.2.1.3・3.2.1.4（p・b・n・G）、A.1.1（群の法則）。
  - `secg-sec1-v2.pdf`（SHA-256 `54687189af3de645756a30a801b596fd14c6566992c13bfd58eede399f324a89`、印刷の頁 N は PDF の頁 N+6）: 2.2.1、2.3.3、2.3.4、3.2.2.1。
  - `nist-sp-800-56a-r3.pdf`（SHA-256 `6b315e3a91012981f9a88ad62c97ea7d4b170a7746e713f34dd29538ee46f403`、印刷の頁 N は PDF の頁 N+11）: 5.6.2.3.3。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-186-4ecdsatestvectors-PKV（P-256・P-384 の件）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-ecp.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-ecp.c`。定数時間の試験: `plan/ws335/tests/unit/ct-ecp.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- Jacobian の式は A.1.1 の affine の式から自分で導く（他の実装を写さない）。a = -3 の doubling の近道。
- 例外（無限遠・P = Q・P = -Q）は両方を計算して mask で選ぶ。
- 試験の側に、affine の素朴な式（逆元を cryc_bn_mont_exp で）で独立の参照を書き、mul の結果と比べる。
- 曲線の値は data として出典（SP 800-186 の表、頁の画像でも照合）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh ecp     # この module
sh plan/ws335/tests/run-unit.sh --win32 ecp     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh ecp
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-01（host の `date` は 23:12 JST）P6（Sonnet 5.5）。worktree p6-ws335b（main 612433e8b）。

作った物: `userland/base/libcrypto-compat/cryc-ecp.c`、`plan/ws335/tests/unit/test-ecp.c`、`plan/ws335/tests/unit/ct-ecp.c`。header・試験の道具・他の module は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh ecp` | `test-ecp: PASS (42510 checks)` / `run-unit: PASS`（ASan・UBSan、約 100 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 ecp`（x86_64、Wine） | `test-ecp: PASS (42510 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh ecp` | `ct-ecp: PASS (13 checks)` / `run-ct: PASS`（valgrind memcheck） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |
| `sh plan/ws335/tests/run-unit.sh all`（test-ecp が 40971 checks だった版で実行。その後 test-ecp にだけ構築点の確認を足した） | 全 21 本 PASS（既存の module の件数は p007 の結果と同じ）/ `run-unit: PASS` |

実装の中身: Jacobian 座標（Montgomery 形）。2 倍算は a = -3 の近道（M = 3(X-Z^2)(X+Z^2)、S = 4XY^2、X3 = M^2-2S、Y3 = M(S-X3)-8Y^4、Z3 = 2YZ）、加算は U1・U2・S1・S2・H・R の標準形。どちらも A.1.1 の affine の式に x = X/Z^2、y = Y/Z^3 を代入して分母を払って導いた（導出は関数の comment）。例外（左が無限遠・右が無限遠・P = Q・P = -Q）は一般の式と 2 倍算を両方計算し、mask で選ぶ（P = -Q は H = 0 で Z3 = 0 になり無限遠）。倍算は 4 bit の窓、16 点の表を全 entry 走査して mask で選ぶ。逆元は Fermat（p - 2）、`cryc_bn_mont_exp`。曲線の値は SP 800-186 3.2.1.3・3.2.1.4 の 16 進（頁の画像で目視、text の 16 進と 10 進の一致を script で、p は定義の式でも照合）。

試験の中身（42510 checks）:

- 参照: 試験の中に A.1.1 の affine の式（傾き、和、2 倍、例外の場合分け）と double-and-add を書き、逆元は `cryc_bn_mont_exp`（p - 2）。module の結果を encode して参照の encode と比べる。他の実装の出力は使っていない。
- 曲線の値: 試験の中に 16 進を再び書き、p は定義の式（2^256-2^224+2^192+2^96-1、2^384-2^128-2^96+2^32-1。limb の単 bit の加減算）、n と G は module の値との一致、G が曲線の上（decode が通る）、(n-1)G = -G（x が同じ、y = p - Gy）、参照で G + (n-1)G = 無限遠、module の mul_add_public で (n-1)G + G = 無限遠。
- 倍算: スカラー 0〜40、n-1〜n-6、単 bit（12 未満の全部、以降 11 おき、上位 4 つ）、交互の bit 列、乱数（G と乱数の点、点は Z = 1 と Z = λ の 2 通りの表現）、out が入力の点と同じ。参照との比較。n（範囲外）の倍算が無限遠、無限遠の点の倍算が無限遠。
- mul_add_public: 乱数（参照）、u1 + u2·d との代数的な恒等式（mod n）、小さい値（0〜3 の全組）と Q = G（G + G の 2 倍算の道）、Q = -G（P + (-P)）、u1 = n-1・u2 = 1、0 と 0、out が q。
- encode・decode・affine_x: 往復、Z の表現が違っても同じ encode、buffer が大きい時は余りを書かない、buffer が小さい（0〜total-1 の全部）時は ENOBUFS で何も書かない、無限遠は EINVAL で全体が 0（ENOBUFS が先）、decode の limbs の上が 0。
- decode の誤り: size 0、0x02・0x03（size 1・2・曲線の size・+1・total-1・total・total+1 の全部で ENOTSUP）、他の tag（0x00・0x01・0x05〜0x08・0x80・0x84・0xff。1 byte の 0x00 も）、長さ（1 から total+3 まで total 以外の全部）、x または y が p と 2^bits-1、x = p-1、全 byte の 1 bit 反転（EINVAL）、-G は有効、乱数の byte 列（大きさぴったりの malloc、300 本と、大きさ・tag も乱数の 300 本）。誤りの時は点が 0。
- 構築した点（check_constructed_points）: 小さい x（1〜24）で曲線の上の点を作り（p ≡ 3 mod 4 なので y = rhs^((p+1)/4)、平方で確認）、decode が通ること、x + p・y + p（桁があふれない時）が EINVAL（範囲の確認が曲線の式の確認より先に落とす）。曲線の式の右辺 +1 を各 limb ごとに作った点（Montgomery 形で 1 つの limb だけ違う）が EINVAL（比較が全 limb を見る）。
- 公開の vector: CAVP PKV の P-256・P-384 の全件（各 12 件: 有効 4、曲線の外または範囲外 4 で decode が EINVAL、座標が幅を越える 4 件は構造上範囲外で encode できず、結果が F であることだけを確認。有効の件は encode が入力と一致し、座標 + p も確認）、CAVP KeyPair の P-256・P-384 の各 10 件（Q = d·G）、Wycheproof ecdh_secp256r1_ecpoint の全 355 件（有効・acceptable で decode が通った 330 件は d·Q の x が shared と一致、invalid と未対応の圧縮形の 25 件は decode が拒否）。PKV・KeyPair の record は section の key が累積しているので、最後の section の key をその record の曲線とした。
- 初期化の誤り: CRYC_CURVE_NONE・3・255 が ENOTSUP で構造体が全部 0、2 回の init が同じ構造体を作る。

定数時間（ct-ecp.c）: スカラー・点の座標を CT_SECRET にして mul_base・mul・encode・affine_x（無限遠になる結果も）を流し、結果と error を呼び出しの後で公開に。

わざと壊す確かめ（git の外。.c を build/tmp/p010/brk/ に複写して書き換え、非 sanitizer の -O2 で test-ecp を build して流す。実装 file は触らず、script の最後で sha256 が変わっていないことを確認）。判定は build の誤りと試験の失敗を区別した（BUILD-ERROR と `test-ecp: FAIL`）。66 通りのうち 65 通りが試験の失敗（`test-ecp: FAIL (...)`）で落ち、build の誤りは 0 通り。内訳: 曲線の値 12（p・n・b・Gx・Gy の各 byte の 1 つ、p384 の最後の byte など）、decode 11（圧縮 tag の値 2、長さの比較、x・y の範囲の確認を飛ばす 2、b を足さない、3x を 2x、比較が 1 limb 足りない、tag の比較、size 0 の誤りの値）、encode・affine_x 7（size の境、無限遠を 0 にしない、無限遠の誤りの値を返さない 2、tag）、2 倍算 6（M の係数、S の係数、X3 の 2S、Y3 の 8Y^4、Z3 の 2、X-Z^2 の符号）、加算 12（S2 の Z、X3 の 2U1H^2、Z3 の H、Y3 の符号、同じ点の判定の and/or と h だけ、右・左の無限遠・同じ点の mask を 0 に、左の無限遠の判定を x に）、mul 8（窓の数、digit の mask、2 倍算 3 回、table の奇数、table の選択のずれ、table の偶数、digit の limb index、table[1]）、mul_add_public 4、逆元の指数、bit の取り出し、init（b を Montgomery 形にしない、未知の id の誤りの値、未知の id で構造体を 0 にしない）、select の y の入れ替え、基点の Z、decode の Z、affine の y の冪、affine の無限遠の判定。
- 最初の実行で見逃した 3 通り: x の範囲確認を飛ばす・y の範囲確認を飛ばす・曲線の式の比較が 1 limb 足りない。曲線上の点は x + p が 2^bits に収まる点（小さい x）でしか作れず、乱数の点では範囲の確認が曲線の式の確認と区別できなかったため。構築した点（上）を足して、3 通りとも落ちる（35・4・19 件の失敗）ことを確認した。
- 見逃し 1 通りは等価: affine_x が無限遠で出力を 0 にする mask を外す（無限遠では逆元が 0 で x がもともと 0 になるので出力が変わらない。誤りの値の mask は別に確認している）。
- 定数時間の壊し（valgrind memcheck、ct-ecp を build し直し）: 8 通りが memcheck の誤りで落ちる: table の選択を branch に、digit が 0 の時に加算を飛ばす、table を digit で直接引く、encode の戻りを branch に、加算の選択を branch に、無限遠で逆元を飛ばす、無限遠の 2 倍算を早く返す、スカラーが 0 の時に loop を抜ける。（最初の 2 つ（`?:` による選択と `if` で値を返す形）は compiler が分岐の無い命令にして memcheck が報告せず PASS したので、compiler が畳めない形（選択の関数を branch の中で呼ぶ、wipe を branch の中で呼ぶ）に壊し方を直して落ちることを確認した。）

自分で決めた点（header が黙っていた所）:

1. 窓は 4 bit（header の comment のとおり）、表は 0P〜15P の 16 点。偶数の添字は 2 倍算、奇数は前の点 + P。表の全 entry を走査して mask で選ぶ。最上位の窓から 4 回 2 倍し 1 回加える（最初の数回は無限遠の 2 倍算と加算）。
2. 無限遠の表現は X = Y = Z = 0 の全 0（header は Z = 0 だけを約束）。2 倍算も加算も Z = 0 の入力を扱い、結果の X・Y は無限遠では意味を持たない。`cryc_ecp_decode` の誤りの時の「*point is zeroed」も同じ表現（無限遠）。
3. 点の limbs の上（幅より上）は常に 0 にする（一時の配列を 0 で始め、結果は MAX_LIMBS 全体を書く・選ぶ）。試験が確認している。
4. `cryc_ecp_encode` は size が total（1 + 2·size）未満なら何も書かずに ENOBUFS（無限遠でも ENOBUFS が先）。それ以外は ちょうど total byte だけを書く（余りの buffer は触らない）。無限遠は全 total byte を 0 にして EINVAL。戻り値も mask で選ぶ（`(int)(EINVAL & mask)`）。
5. `cryc_ecp_decode` の誤りの順は header のとおり。coordinate の幅は size の一致の後なので、x・y は必ず `cryc_bn_decode` に収まる。on-curve の確認は変動時間（公開）。
6. `cryc_ecp_mul` は scalar が n 以上でも（契約の外）範囲外を読み書きせず、scalar の値 mod n の倍数を返す（窓の列は scalar の全 bit を見るため）。n の倍算が無限遠になることを試験が確認している。
7. `cryc_ecp_mul_add_public` は左から bit ごとに 2 倍算と、(bit1, bit2) に応じた G・Q・G+Q の加算（Shamir の方法）。加算は定数時間の完全な加算を再利用するので、Q = ±G の例外は同じ道。
8. 逆元は p - 2 を毎回 modulus から作る（encode して 2 を引く、public）。
9. stack: `cryc_ecp_mul` は 16 点の表（P-384 でも 2.3 KB）と蓄積などで約 3 KB。2 倍算・加算の一時は各 1 KB 未満。限られた stack の機械では未確認。

header と phase.md の食い違い: 見つからなかった。header の `cryc_ecp_mul_add_public` の comment は「FIPS 186-5 6.4.2 step 6」、骨格の .c の comment は「step 5」と書いてあり、どちらが正しいかは未確認（comment のみ。header の直しは p035 に持ち越し。実装の comment には節を書いていない）。

試験の vector の注意: CAVP の PKV・KeyPair の record file は、section の header を record ごとに累積して持つ（vectors.py が作る形）。他の phase が曲線ごとに絞る時は最後の section の key を使うこと（p011 の ECDSA の SigVer・KeyPair も同じ形）。

未実施: 性能の測定、P-384 の ECDH の公開 vector（rec には無い。ecdsa の secp384r1 は p011）、P-384 の「y が小さい点」（y + p が桁あふれしない点）での y の範囲確認（P-256 の Wycheproof の y = 1 の点では確認、同じコード）、32 bit の size_t の機械での実行、aarch64 Windows の実行（build のみ、check-skeleton）、限られた stack での実行、実機・QEMU（範囲外）。

commit: 下の ws.md の表と同じ commit。
