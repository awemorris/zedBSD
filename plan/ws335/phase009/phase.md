<!-- awesome-plan project=zedbsd record=ws335-p009 -->

# ws335-p009: Ed25519

Status: 実装済み・試験 PASS（2026-11-02 01:20 Sonnet q1876-i01。cleared は Q1。依存: p008）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-ed25519.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8032.txt`（SHA-256 `ed63657ff389301282b169b0abde9b5dd2c7e4d524fdfa5da6ff3094fc93c4c3`）: 5.1（5.1.1〜5.1.7）、7.1（試験の値）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: wycheproof-ed25519_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-ed25519.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-ed25519.c`。定数時間の試験: `plan/ws335/tests/unit/ct-ed25519.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 点は拡張 Edwards 座標（5.1.4）。署名の固定の基点の倍は表の全部を読む選択。
- 確かめは S < L（5.1.7 step 1）を必ず。cofactor の無い式（[S]B = R + [k]A）。
- SHA-512 は cryc-sha2。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh ed25519     # この module
sh plan/ws335/tests/run-unit.sh --win32 ed25519     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh ed25519
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 01:18 JST）Sonnet 5.5（q1876-i01）。worktree s-ws335-p009（main 400efaaee）。

作った物: `userland/base/libcrypto-compat/cryc-ed25519.c`、`plan/ws335/tests/unit/test-ed25519.c`、`plan/ws335/tests/unit/ct-ed25519.c`。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh ed25519` | `test-ed25519: PASS (7289 checks)` / `run-unit: PASS`（ASan・UBSan、約 70 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 ed25519`（x86_64、Wine） | `test-ed25519: PASS (7289 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh ed25519` | `ct-ed25519: PASS (12 checks)` / `run-ct: PASS`（valgrind memcheck） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |
| `sh plan/ws335/tests/run-unit.sh all`（test-ed25519 が 7287 checks だった途中の版で流した。最終版の ed25519 は上の行で単独に流し直した） | 26 本が PASS、FAIL 0 本、最後の行 `run-unit: PASS`（posix。ed25519 以外の module の .c は触っていない） |

実装の中身（RFC 8032 の 5.1 だけ、Ed25519ctx・Ed25519ph は無い）:

- 点は拡張 Edwards 座標。加算は 5.1.4 の完全な式（倍加の格好でも使える）、倍加は 5.1.4 の式。加算・減算の結果は必ず `cryc_f25519_carry` で reduced に戻してから次の加減に使い、`cryc_f25519_mul` が loose より緩い入力を受けない。
- スカラー倍は 4 bit 固定窓（上から。窓ごとに倍加 4 回と表の 1 点の加算）。表は [0]P〜[15]P を呼び出しごとに加算で作り、読む時は 16 点の全部を `cryc_f25519_cswap` と `cryc_ct_mask_eq32` の mask で選ぶ（添字で番地が変わらない）。署名の [s]B・[r]B と確かめの [S]B・[k](-A') が同じ関数を使う（基点の定数の表は持たない）。
- スカラーの算術（mod L）: 512 bit の整数を上の bit から 1 bit ずつ「2 倍して bit を足し、L 以上なら mask で L を引く」で reduce（分岐・除算なし）、S = (r + k·s) mod L は 8×8 limb の学校算（k を先に reduce、s は 255 bit のまま）+ r を足して reduce。
- 署名: seed の SHA-512 → 剪定したスカラー s と prefix、A は seed からその場で導出（呼び出し側の A は受け取らない）、r = SHA-512(prefix || M) mod L、R = encode([r]B)、k = SHA-512(R || A || M) mod L、S = (r + k·s) mod L。秘密を持つ配列は全部 `cryc_wipe`。
- 確かめ: S < L（5.1.7 step 1）を最初に、A を 5.1.3 で decode（y ≥ p・平方根が無い・x = 0 で x_0 = 1 は失敗）、k = SHA-512(R || A || M) は 512 bit の整数のまま（L で reduce しない。小さな位数の成分を持つ key で結果が変わるため、step 2 の文面のとおり）、[S]B − [k]A' を求めて**その encode を R の 32 byte と比べる**（R が decode できる事と正準な encode である事は同じなので、decode に失敗する R・y ≥ p の R・x = 0 で符号 bit の立った R は一致せず、cofactor の無い式 [S]B = R + [k]A' と同じになる）。誤りは全部 EBADMSG。

試験の中身（7289 checks）:

- RFC 8032 7.1 の 5 本（TEST 1・2・3・1024・SHA(abc)）: seed から公開鍵、seed と message から署名、公開の署名の確かめ。値は RFC の本文から script で取り出し、同じ節 6 の Python で再計算して一致を確かめてから試験に置いた（1023 byte と 64 byte の message が全部読めたことも check）。
- 節 6 の Python の例（`secret_to_public`・`sign`）から作った 26 本（message の長さ 0・1・2・31〜33・63〜65・95〜97・111〜113・127〜129・191〜193・255〜257・1000・4096、message は byte i = (i·7+3) mod 256、seed は固定の文字列の SHA-256）。出典の comment 付きで data として試験の中に置いた（vectors.py の変換は無い形式）。
- Wycheproof ed25519_test の 210 件の全部: 117 valid は全部 0、52 invalid（64 byte の署名）は全部 EBADMSG、12 件は署名が 64 byte でない（短い・長い・garbage）ので固定長の口に渡せず skipped（file はどれも invalid と言うので check した）、29 件の acceptable（小さな位数・混合位数の key）は、節 5.1.7 step 3 の式（k を reduce しない）を Python の例の点の関数で評価した 8 件の tcId（152・161・162・165・172・175・176・177）を試験の中の表にして、それ以外は EBADMSG と固定した（file は両方の答えを許す。k を L で reduce する実装は 29 件を全部通し、この表で落ちる）。
- 性質: 署名が決定的（同じ seed・message で同じ 64 byte、署名先の buffer の中身に依らない）・公開鍵が同じ、長さ 0〜300 の全部の往復（長さごとに新しい key）、長さ 0〜999 の乱数の message と乱数の seed 40 組、1 MiB + 3 byte の往復と最後の byte の変更、署名・message・key の 1 bit ずつの変更（3 通りの長さ 0・1・129 で署名 512 bit・message の全 bit・key 256 bit）と 1 byte 短い・長い message が全部 EBADMSG、S + k·L（k = 1〜8、式は成り立ち範囲だけが拒む）と S の上位 3 bit・全部 1 が EBADMSG、key を単位元にした署名で S の境（S = 0 と R = 単位元は通る、S = L・L + 1・1 は拒む、S = L − 1 と R = −B は通る、R = B は拒む、L − 1 でない S は拒む）、正準でない encode（y = 1 を p + 1 で書いた R・key、y = p・2^255 − 1、符号 bit の立った単位元・(0, −1)）が全部 EBADMSG（y = p の key は mod 4 の位数 4 の点に読まれると k が 4 で割れる message で通るので 64 message で試した）、位数 2 の点 (0, −1) の key は message により通る・通らないが両方起きる、出力は書く場所だけ（公開鍵 32 byte・署名 64 byte の前後 16 byte の見張り）、seed・message・key・署名は変わらない、message が NULL で size 0、乱数の 300 組（署名・key・message）が全部 EBADMSG、実の key への乱数の署名 100 組が全部 EBADMSG、乱数の seed の公開鍵の符号 bit が両方出る。
- 期待の値は RFC の本文・表・節 6 の Python・Wycheproof の file だけ。他の実装の出力は使っていない。
- header は固定長の入力だけなので、途中で切れた入力は渡せない（Wycheproof の短い・長い署名は skipped として file の invalid を確認した）。

定数時間（ct-ed25519.c）: seed を CT_SECRET にして公開鍵の導出と署名（4 通りの message の長さ）を流し、出力と戻り値を呼び出しの後で CT_PUBLIC にする。確かめは公開の値だけなので印を付けない。`ct-ed25519: PASS (12 checks)`。

わざと壊す確かめ（git の外。`build/tmp/p009/brk.py` が実装を `build/tmp/p009/brk/unit-N/` に複写して 1 箇所を書き換え、非 sanitizer の -O1 で test-ed25519 を build して流す。実装 file は触らず、最後に sha256 が変わっていないことを script が確認）。判定は build の誤り・crash・試験の失敗（最後の行が `test-ed25519: FAIL`）を区別した。69 通り（定数・L の limb、範囲の確かめ、decode の各段、確かめと署名の組立、剪定、encode、加算・倍加・符号反転、表の選択、スカラー倍、mod L の reduce と積和、hash の連結、limb の読みの各 1 箇所）のうち 65 通りが試験の失敗（`test-ed25519: FAIL (...)`）、build の誤りは 0（最初の実行では未使用変数の -Werror で build が落ちた 5 通りがあり、`(void)` で使う形に直して流し直した）、crash 0、見逃し 4 通り。その後 `ct-ed25519` を同じ形で 4 通り壊して（表を添字で読む、reduce の mask を分岐に、0 の窓を飛ばす、秘密のスカラーの bit で分岐）、全部が memcheck の誤りで落ちる（build の誤りは最初の実行で 2 通りあり、直した）。

見逃しの 4 通りは、観測できる違いが無い（等価）か、口からは見えない:

1. 倍加の E = h − sum を sum − h にする: E の符号が変わると結果が −2P になるが、窓ごとの倍加が 4 回（偶数）なので符号が打ち消し合い、結果は同じ（等価）。
2. mod L の reduce の borrow を `difference >> 40 & 1` にする: 引き算が wrap した時の上位 bit が全部 1 なので bit 40 も 1、wrap しなければ 0（等価）。
3. 署名の R のスカラー倍を 63 窓にする: r は L 未満（2^253 未満）で最上位の窓が 0（等価）。
4. decode で平方根が無い場合を受ける（`is_square == 0 && is_minus == 0` の確かめを外す）: 曲線の外の点になり、その key で式 [S]B = R + [k]A が成り立つことは無く、どの入力でも EBADMSG（誤りの値が同じ）で、口からは見えない。

自分で決めた点（header が黙っていた所）:

1. 4 bit 固定窓のスカラー倍を署名と確かめで共有し、表は呼び出しごとに作る（基点の定数の表を持たない。header の「固定の基点の倍は表の全部を読む」は選択の全点を mask で読む事で満たす）。署名は 1 回に基点のスカラー倍を 2 回（A と R）、確かめは 2 回（[S]B と [k](−A'）。1 回ごとに表の 14 回の加算を足す。
2. 確かめは R を decode せず、結果の点の encode を R の byte と比べる（上述。同じ式で、decode に失敗する R・非正準の R を同時に拒む）。k は reduce しない（上述）。
3. 確かめの誤りは全部 EBADMSG（header の約束どおり）。S の範囲の確かめを最初に、A の decode をその次に。
4. static の補助: 加減（reduced に戻す）、点の加算・倍加・符号反転・選択・スカラー倍・encode・decode、mod L の reduce・積和・範囲、limb の読み書き、seed の展開、3 つの連結の SHA-512。名前は全部 `ed_`、公開の名前は header の 3 つだけ。
5. 定数（d・√−1・基点の x と y・L）は 32 byte の little-endian か limb の表で、RFC 8032 Table 1 の 10 進を RFC の Python（節 6）で変換した。√−1 は 5.1.1・5.1.3 step 3.2 の 2^((p−1)/4) で、実行時に二乗が −1 になる事は decode の試験（Wycheproof・RFC の key）が通ることで確かめられている。

header と phase.md の食い違い: 無し（header は変えていない。phase.md の「署名の固定の基点の倍は表の全部を読む選択」は上の 1 のとおり）。

未実施: 性能の測定、32 bit の size_t の機械・aarch64 Windows の実行（build のみ）、鍵の導出・署名の中で seed 由来の値が 1 つずつ memcheck に見える形の細かい確認（seed を CT_SECRET にして全体を流した）、確かめの定数時間（公開の値だけなので対象外）、実機・QEMU（範囲外）。

commit: 7fb4fc0167（実装・試験・この節。この行の追記は次の commit）
