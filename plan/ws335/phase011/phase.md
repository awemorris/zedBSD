<!-- awesome-plan project=zedbsd record=ws335-p011 -->

# ws335-p011: ECC: 鍵の生成・ECDH・ECDSA の確かめ

Status: 実装済み・試験 PASS（2026-11-01 23:42 P6。cleared は Q1。依存: p010）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-ecc.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-186-5.pdf`（SHA-256 `fbb9c7c2ba442f03c57b63b43c888311903c9d0f29f89b06efdebd9b619140c5`、印刷の頁 N は PDF の頁 N+9）: 6.4.2、A.2.2、B.2.1。
  - `nist-sp-800-56a-r3.pdf`（SHA-256 `6b315e3a91012981f9a88ad62c97ea7d4b170a7746e713f34dd29538ee46f403`、印刷の頁 N は PDF の頁 N+11）: 5.7.1.2。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-ecccdhtestvectors-*（P-256・P-384）、cavp-186-4ecdsatestvectors-SigVer（P-256・P-384 × SHA-256/384/512）、wycheproof-ecdsa_secp256r1_sha256/512・secp384r1_sha384/512_test（DER の署名は p018 まで r・s を試験の側で取り出す）、wycheproof-ecdh_secp256r1_ecpoint_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-ecc.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-ecc.c`。定数時間の試験: `plan/ws335/tests/unit/ct-ecc.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- hash の左端の bits(n) bit を E にする（B.2.1）。
- 鍵の生成の棄却は mask の比較で。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh ecc     # この module
sh plan/ws335/tests/run-unit.sh --win32 ecc     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh ecc
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-01（host の `date` は 23:42 JST）P6（Sonnet 5.5）。worktree p6-ws335b（main 7fbbd867d）。

作った物: `userland/base/libcrypto-compat/cryc-ecc.c`、`plan/ws335/tests/unit/test-ecc.c`、`plan/ws335/tests/unit/ct-ecc.c`。header・試験の道具・他の module は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh ecc` | `test-ecc: PASS (22888 checks)` / `run-unit: PASS`（ASan・UBSan、約 60 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 ecc`（x86_64、Wine） | `test-ecc: PASS (22888 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh ecc` | `ct-ecc: PASS (17 checks)` / `run-ct: PASS`（valgrind memcheck） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

`run-unit.sh all` はこの Phase では未実施（変えた物は cryc-ecc.c だけで、他の module の .c は触っていない。Q1 が main で流し直す）。

実装の中身:

- 鍵の生成（FIPS 186-5 A.2.2）: 1 回の draw で `ec->size` byte（bits(n) bit）を乱数源に要求し、c を整数に読み、n - 2 との比較（`cryc_bn_lt` の mask）で棄却、d = c + 1（limb の加算）、Q = d·G（`cryc_ecp_mul_base`）、encode。棄却の判定は mask で、loop を抜ける分岐に使う（捨てる候補の 1 bit は公開、comment に書いた）。64 回の棄却か乱数の失敗は EIO で、秘密の出力 2 つを 0 に。encode の失敗（d が範囲内なら起きない）は mask で扱い、d から導いた値で分岐しない。
- ECDH（SP 800-56A 5.7.1.2）: 相手の点を先に decode（誤りはそのまま返す）。秘密鍵は読んで 1 <= d <= n - 1 を mask で確かめ、`cryc_ecp_mul` と `cryc_ecp_affine_x` を必ず実行し、2 つの失敗（範囲外・無限遠）を 1 つの誤り値に mask で束ね、出力は mask で 0 にする。秘密鍵の値で分岐しない（ct-ecc で確認）。
- ECDSA 検証（FIPS 186-5 6.4.2）: hash_size 0 は EINVAL、公開鍵の decode の誤りをそのまま返し（署名の誤りより先）、r・s を [1, n-1] で確かめ（EBADMSG）、E は hash の左端 min(hash_size, ec->size) byte を整数に読み `cryc_bn_reduce_once` で mod n、w = s^-1 を n - 2 乗で（Montgomery）、u1 = E·w・u2 = r·w は `cryc_bn_mont_mul` に Montgomery 形の w を渡して平文の積、R = u1·G + u2·Q（`cryc_ecp_mul_add_public`）、R が無限遠なら EBADMSG、x を mod n（reduce_once、x < p < 2n）して r と比べる。

試験の中身（22888 checks、P-256・P-384 の両方）:

- 鍵の生成: 台本の乱数（候補 0 → d = 1 で Q = G（SP 800-186 の値を試験の中に再び書いた）、n-2 → d = n-1 で Q = -G、n-3、n-1・n・n+1・全部 1 の棄却と後の受理（draw 数）、j limb が全部 1 の候補で d = 2^(32j)（繰り上がりが全 limb を走る）、中の 1 byte だけ 0xff、63 回の棄却の後の受理（64 回目の draw）、64 回の棄却で EIO（draw はちょうど 64 回、出力 2 つが 0、出力の外の byte は触らない）、1 回目・4 回目の乱数の失敗（EIO、0、draw 数）、乱数源 NULL・未初期化の曲線は EINVAL）。毎回の要求の大きさが ec->size であることと、台本を越えて draw しないこと。CAVP KeyPair の各 10 件（d - 1 を候補にして d と Q が出る）。
- ECDH: CAVP の ECC CDH primitive の P-256・P-384 各 25 件の全部（出典の comment 付きで試験の中に data として置いた。peer の点・dIUT・ZIUT と、dIUT から鍵の生成で出る QIUT も確認）、Wycheproof ecdh_secp256r1_ecpoint の全 355 件（有効・acceptable で decode が通る 330 件は shared が一致、残り 25 件は誤り・圧縮形の ENOTSUP で出力が 0）、誤り（秘密鍵 0・n・n+1・全部 1 は EINVAL で出力が 0、n-1 と 1 は成功して x(G)、peer の長さ 0・圧縮形 0x02/0x03 を長さ 1〜total+1 の全部で ENOTSUP・先頭 byte 0x00/0x05・長さ total±1・y を変えた点・座標 = p はどれも誤りで出力が 0、未初期化の曲線）、生成した鍵の組の一致・x(dQ) = x((n-d)Q)・x(dG) = 公開鍵の x、x の先頭 byte が 0 の点（P-256 は d = 379、P-384 は d = 197）で shared が ec->size byte 全部で先頭が 0。
- ECDSA 検証: CAVP SigVer の P-256・P-384 × SHA-256・384・512（各曲線 9 件の P と 36 件の F。F は key が曲線の外なら decode と同じ誤り、他は EBADMSG）、Wycheproof の ecdsa_secp256r1_sha256（174 valid・75 invalid を verify、221 件は DER が壊れ、14 件は整数が曲線より広い）・sha512（243・77・220・14）・secp384r1_sha384（194・75・221・14）・sha512（231・75・221・15）（DER は試験の側で厳密に読む。valid は全部 0、invalid は DER の形が正しければ EBADMSG、壊れた DER・広い整数は file が invalid と言っている事を確認）。試験の側の署名器（FIPS 186-5 6.4.1 を書いた）で、hash の大きさ 1・20・32・48・64・100 の往復、(r, n-s) も通る、hash の使う byte の先頭・中・末を変えると EBADMSG、長い hash は左端だけが効く（それより後を変えても通る）、短い hash は左を 0 で埋めても同じ・右を 0 で埋めると別（EBADMSG）、r・s の byte を変える・入れ替える・key の x・y を変える（EINVAL）・別の key（EBADMSG）、E = 0（hash が全部 0・n）、n+1 が E = 1、~n と全部 1、R が無限遠（E = -(r·d) mod n で EBADMSG）、r・s の範囲外（0・n-1（範囲内だが誤り）・n・n+1・全部 1）、key の誤りの全部（長さ 0・圧縮形を長さ 1〜total+1・0x00・0x06・長さ total±1・y の変更・座標 = p）が署名の誤りより先に返ること、hash_size 0 は EINVAL、未初期化の曲線。
- x 座標が n 以上の R（`check_x_above_order`、各曲線 3 点）: x = n + t（小さい t）で x^3 - 3x + b が平方になる点 R を探し（p ≡ 3 mod 4 なので a^((p+1)/4)）、u1・s を選び E = u1·s、u2 = r/s、Q = u2^-1·R - (u1/u2)·G を作って、検証側の u1·G + u2·Q がちょうど R になるようにした。r = t は通り、r = x（n 以上）と r + 1 は EBADMSG、r = t に第 2・中・最上位の limb の 1 bit を足した 3 通りも EBADMSG（比較が全 limb を見る）。x を mod n しない実装・比較の limb が足りない実装はこの試験でだけ落ちる。

定数時間（ct-ecc.c）: ECDH の秘密鍵を CT_SECRET にし、成功・秘密鍵 0・全部 1（EINVAL）を流し、結果と誤りの値を呼び出しの後で公開に。鍵の生成は、候補の棄却が候補への分岐（設計どおり、捨てる候補の 1 bit は公開）なので、乱数源が公開の byte を出す形で流し、得た秘密鍵を ECDH の秘密にして使う（生成の中で d が秘密のまま `cryc_ecp_mul_base` を通る所は ct-ecp が見ている。生成の中の d を秘密として memcheck に見せる形は作れていない）。

わざと壊す確かめ（git の外。`build/tmp/p011/brk.py`、.c を `build/tmp/p011/brk/unit-N/` に複写して書き換え、非 sanitizer の -O2 で test-ecc を build して流す。実装 file は触らず、最後に sha256 が変わっていないことを script が確認）。判定は build の誤り（BUILD-ERROR）、試験の失敗（`test-ecc: FAIL`）、crash を区別した。50 通り: 43 通りが試験の失敗（`test-ecc: FAIL (...)`）、1 通りが crash（NULL の乱数源の check を外し、NULL を呼んで segfault）、build の誤りは 0、見逃しは 6 通りで全部等価か冗長:

- 試験の失敗 43: 鍵の生成 17（draw の回数 2、比較の向き、limit の n-1・n-3、繰り上がり 2、borrow の amount を戻さない、要求の大きさ、乱数の失敗・棄却の尽きた時の wipe 4 とその誤りの値 2、公開鍵・秘密鍵を c から作る 2）、ECDH 9（範囲の比較の向き、範囲の検査の脱落、誤りの mask の反転、誤りの時に出力を消さない、peer の x を返す、decode の誤りの値、初めの wipe の脱落、size の検査、d を書き換える）、検証 17（hash_size 0 を受ける・1 を拒む、左端 byte に切らない 2、s ≥ n を受ける、u1・u2 の取り違え 3、x を mod n しない、比較の limb が足りない・低い limb だけ（2 通りとも r に第 2・中・最上位の limb の 1 bit を足す試験で落ちる。最初の実行では見逃したので `check_x_above_order` にその 3 通りを足した）、key の誤りの値、w = s にする・指数 n-1、R が無限遠で 0 を返す、結果の反転、r を s から読む）。
- 見逃し 6（等価か冗長で、観測できる違いが無い）: (1) n - 2 の borrow の脱落: n の最下位の limb が両曲線で 2 以上なので borrow が起きない（等価）。(2) ECDH で d = 0 の mask の脱落: d = 0 は積が無限遠になり affine_x の誤りで同じ EINVAL（冗長な検査）。(3) 検証で E を reduce_once しない: Montgomery 積は a < R で正しく畳むので出力が同じ（等価。契約の上では reduce が要るので残す）。(4)(5)(6) 検証で r ≥ n・r = 0・s = 0 の検査を脱落: v は mod n で n 未満・0 でない（s = 0 では u1 = u2 = 0 で R が無限遠）ため、後の比較か無限遠の判定で必ず EBADMSG になり、結果が同じ（冗長な検査）。s ≥ n の脱落は落ちる（s = n + k が s = k と同じ剰余になるため）。
- 定数時間の壊し（valgrind memcheck、ct-ecc を build し直し）4 通りが全部 memcheck の誤りで落ちる: 出力の wipe を分岐に、d = 0 で早く返す、範囲外で早く返す、積の誤りを分岐に（最後のは最初 `product_error = EINVAL` の形にしたら compiler が分岐の無い命令にして見逃したので、compiler が畳めない形（関数 `cryc_wipe` を分岐の中で呼ぶ）に直して落ちることを確認）。

自分で決めた点（header が黙っていた所）:

1. 鍵の生成の 1 回の draw は `ec->size` byte を乱数源に要求する（bits(n) = 8 × size が両曲線で成り立つ）。
2. 乱数源が NULL なら EINVAL（header に無い誤り値だが、NULL の呼び出しは呼び出し側の誤り）。曲線が未初期化（size 0・limbs が size/4 でない・MAX 超）なら 3 つの関数とも EINVAL（出力は触らない）。
3. ECDH は peer の decode を先にし、秘密鍵の範囲外・無限遠の積は後で mask に束ねる（両方が誤りなら peer の誤りが返る）。秘密鍵の範囲を分岐で見ると定数時間にならないため。範囲外の鍵でも `cryc_ecp_mul` は実行される（結果は捨てる）。
4. ECDSA 検証は hash_size 0 → 公開鍵の decode → r・s の範囲 の順（key の誤りが署名の誤りより先）。hash が曲線の size より短い時は、短い整数として読む（右に詰めない）。
5. 鍵の生成の失敗（EIO）では private_key を ec->size byte、public_key を 1 + 2 × ec->size byte だけ 0 にする（それより後は触らない）。
6. static の補助は `ecc_curve_usable`、`ecc_order_minus_two`、`ecc_add_one`。n - 2 は limb の引き算で作り、検証の逆元の指数（byte）も同じ値から encode する。

header と phase.md の食い違い: header の「CRYC_ECC_KEYGEN_TRIES 回の棄却」は、台本で 64 回目の draw が受理される場合を成功とし、64 回とも棄却で EIO とした（header の文面どおり）。他は見つからなかった。

未実施: `run-unit.sh all` の全 module 通し（Q1 が main で）、性能の測定、P-384 の ECDH の CAVP を rec から読む形（CAVP の ECC CDH は .txt で vectors.py が .rsp だけ変換するため rec が無い。P-256・P-384 各 25 件の全部を試験の中に出典の comment 付きで置いて確認した。rec からの読みは vectors.py の .txt の変換の後）、P-384 の ECDH の Wycheproof（file が無い）、鍵の生成の中で d を秘密として memcheck に見せる形、ECDSA の署名の生成（p037 の試験の側の署名器は別）、32 bit の size_t の機械・aarch64 Windows の実行（build のみ）、限られた stack での実行、実機・QEMU（範囲外）。

commit: dc4adcd353（初版）、63d76158af（最終版: `check_x_above_order` の r の 4 通り、`ecc_curve_usable` の呼び出しを条件の外へ、この結果の節）。

- 注意（2026-11-01 Q1、p010 の P6 の報告から）: CAVP の rec の file は record ごとに section の key が累積する。曲線を絞るには最後の section の key を使う（ECDSA SigVer・KeyPair も同じ形）。
