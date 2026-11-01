<!-- awesome-plan project=zedbsd record=ws335-p007 -->

# ws335-p007: 固定幅の整数と Montgomery の算術

Status: cleared（2026-11-01 22:26 Q1、main 3666e69f0。main で run-unit all・run-ct bn PASS を流し直した）。前: 実装済み・試験 PASS（2026-11-01 22:21 P6。cleared は Q1）。前: planned（2026-11-01 p001 が作成。起動は Q1。依存: p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-bn.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-186-5.pdf`（SHA-256 `fbb9c7c2ba442f03c57b63b43c888311903c9d0f29f89b06efdebd9b619140c5`、印刷の頁 N は PDF の頁 N+9）: B.1（逆元）、B.2（変換）。
  - `rfc8017.txt`（SHA-256 `1e72dc473d18df3fc5598cdc12795a9f18f36f1aef15abc23a55eb0d58151d11`）: 4.1・4.2（I2OSP・OS2IP）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-bn.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-bn.c`。定数時間の試験: `plan/ws335/tests/unit/ct-bn.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- REDC は語ごとの形（header の冒頭）。最後の引き算は mask で必ず。
- mont_exp は指数（公開）の bit だけで時間が決まる。
- 期待の値: 小さな数の手の計算、P-256 の p と n での性質（to_mont と from_mont の往復、(a·b)·c = a·(b·c)、a·a^-1 = 1 を mont_exp(p-2) で）。他の実装の出力を使わない。
- limb の数 1 から 256（8192 bit）まで。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh bn     # この module
sh plan/ws335/tests/run-unit.sh --win32 bn     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh bn
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-01（host の `date` は 22:21 JST）P6（Sonnet 5.5）。worktree p6-ws335b（main 069619dbc）。

作った物: `userland/base/libcrypto-compat/cryc-bn.c`、`plan/ws335/tests/unit/test-bn.c`、`plan/ws335/tests/unit/ct-bn.c`。header・試験の道具・他の module は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh bn` | `test-bn: PASS (186097 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 bn`（x86_64、Wine） | `test-bn: PASS (186097 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 19 本 PASS（test-bn 186097、aes 28516、chacha 52156、f25519 22903、gcm 235197、hash 12965、sha2 14092、sha3 629616、tools 18、util 169160、x25519 4538、と ct-* 8 本（ct-bn 7 checks を含む））/ `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh bn` | `ct-bn: PASS (7 checks)` / `run-ct: PASS`（valgrind memcheck） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

試験の中身（186097 checks）:

- 参照: 試験の中に、定義だけから 16 bit の digit の数（和・差・積、剰余は 1 bit ずつ取り込んで modulus 以上なら引く）を書いた。Montgomery の積は定義の性質（r < m かつ r·R ≡ a·b mod m、m が奇数なので解は 1 つ）で、冪は平文の square-and-multiply で照合した。他の実装の出力は使っていない。
- 手計算: modulus 7（R = 2^32 = 4、R² = 2、-7⁻¹ = 0x49249249、to_mont(3) = 5、mont_mul(5, 6) = 30·2 = 4、3^5 = 5、3^6 = 1、指数 0 は R mod 7 = 4、mod_add・mod_sub・reduce_once の値）と modulus 2^32 + 1（2^32 ≡ -1 なので R = 1、R² = 1、m0inv = 0xffffffff、2^32·2^32 = 1、2^32·3 = m - 3）。
- init: 0・1・偶数（各幅）・NULL と size 0 が EINVAL、3 と 0xffffffff と 2^32+1 が成功、8192 bit（全て 1、先頭に 1000 byte の 0 を付けた物、1025 byte で先頭が 0 の物）が成功、8193 bit（2^8192 + 1、全て 1 の 1025・1030 byte）が ENOTSUP、幅が過ぎた偶数は EINVAL（不正が先）、エラーでは構造体全体が 0（0x5a で埋めてから）、乱数の byte 列 400 本（大きさぴったりの malloc）で 0・EINVAL・ENOTSUP のどれかと、成功なら -m⁻¹ の性質・上位 limb が非 0。成功の時は limbs・bits・modulus・上位の 0・m0inv・R² mod m（定義から）・R mod m を照合。
- decode・encode: byte の位置を手で（01 02 03 04 05 が [0x02030405, 1]）、幅に収まらない値（EINVAL、out が 0、他の位置は不変）、幅の上の 0 の byte、size 0・幅 0、各 byte 位置の 1 つの非 0、幅 1〜9 limb × size 0〜幅+3 byte の往復、encode の padding と切り詰め。
- lt・is_zero・select: 幅 0、各 limb だけが違う組（両向き）、乱数と参照、単 bit の is_zero、mask 全 1・0、out が a か b。
- 算術: 幅 1〜9・12・16・24・32・64・128・256 limb（最大 8192 bit）、modulus の種類 5（上位 bit が立つ乱数、上位 limb が小さい乱数、全て 1、2^(32(n-1)) + 1、上位 limb が 1）、幅 16 以下は各幅で 12 個の乱数 modulus を足す。値は 0・1・m-1・m-2・m に近い物・小さい物・乱数で、mod_add・mod_sub・reduce_once（a < m、m と m + a、2m - 1）・mont_mul・to_mont・from_mont・mont_one・mont_exp（指数 0 の 1 byte、0x010001 を 3 byte、先頭に 0 の byte を付けた乱数）を参照と照合し、out が a・b・a=b の別名でも同じ値、結果が m 未満、出力の sentinel を上書き。buffer は大きさぴったりの malloc（ASan）。往復 to_mont→from_mont、a·b と from_mont(to_mont(a)·to_mont(b))。
- P-256・P-384 の p と n（hex。p は定義の式 2^256 - 2^224 + 2^192 + 2^96 - 1 と 2^384 - 2^128 - 2^96 + 2^32 - 1 から作った値とも一致）: Fermat の a^(p-1) = 1（底 2・3・5・7・11・13 と乱数）、a·a^(p-2) = 1、a → aR → a、(a·b)·c = a·(b·c)、a·(b+c) = a·b + a·c、(p-1)² = 1。合成数 15 で 2^14 ≠ 1（Fermat が合成数を落とす確認）。

定数時間（ct-bn.c、256 bit と 2048 bit の modulus）: 値・底・decode の byte・encode の数を CT_SECRET にして mod_add・mod_sub・reduce_once・to_mont・mont_mul・from_mont・lt・is_zero・select・mont_exp（指数は公開）・decode（収まる場合と収まらない場合）・encode（幅内、広い、狭い）を流し、結果と mask と error を呼び出しの後で公開に。

わざと壊す確かめ（git の外で .c を書き換え、run-unit.sh bn（または run-ct.sh bn）を流し、`cmp` で元に戻ったことを毎回確かめた）。判定は「test-bn: FAIL (build)」なら build の誤り、「FAIL (exit; ...)」なら試験の失敗として区別した。
- 単体試験: 62 通りのうち 60 通りが試験の失敗で落ちた（`FAIL (exit; ...)`）。m0inv の符号、Newton 反復の回数（4 step を 3 に）、R² の倍化の回数（-32・+1）と開始 bit、init の最大幅の比較（>=、>）・偶数の判定・modulus 1 の判定（定数、削除）・bit 長・limb 数の丸め・zero の判定・エラーで構造体を 0 にしない、mod_add の mask（carry を落とす・carry と borrow の and・borrow の向き・select の入れ替え）、mod_sub の mask の反転・常に足す・足さない、reduce_once の向きと等号、mont_mul の最上位 word を無視・borrow の向き・factor（m0inv 以外）・列の開始・carry の shift・最上位 word を 0・最上位への carry を落とす・行を 1 つ飛ばす・shift の位置・reduction の carry、to_mont に R² 以外・from_mont の 1 を 2・mont_one を from_mont、mont_exp の上位 bit（7 を 6）・bit の取り出し・平方を飛ばす・開始を 0・別名（base でなく out）・最後の byte を飛ばす、decode の幅の境（<=）・excess を OR でなく代入・keep を適用しない・error の反転・shift・byte の順、encode の shift・幅の境・byte の mask、lt の向き・borrow の bit・「以下」、is_zero の OR でなく代入・最初の limb を飛ばす、select の入れ替え・b を落とす、bn_add の carry shift、bn_sub の borrow を伝えない。
- 見逃し 2 通りはどちらも等価: (1) Newton 反復を 5 回から 4 回に（元の 5 回は余分で、4 回で 48 bit 正しいので動作が同じ。実装を 4 回に直し、3 回に壊すと落ちる）。(2) init の `significant > 1025` を `> 1026` に（size_t の乗算の溢れを避ける早い判定だけで、後の bits の判定が同じ結果を返す）。
- 最初の実行で build の誤りになった 5 通り（未使用変数の警告）は数に入れず、書き直して試験の失敗で落ちることを確かめた（上の 60 通りに含む）。
- 定数時間の壊し（run-ct.sh bn）: 10 通りが `run-ct: FAIL`（memcheck）で、戻して PASS。mod_add の mask への branch、mont_mul の mask への branch、mont_mul の a の word が 0 の時に飛ばす、mont_exp が底の bit で平方の回数を変える、lt の早い return、is_zero の早い return、decode の byte が 0 の時に飛ばす、encode の limb が 0 の時の branch、select の branch、mod_sub の borrow での branch。（mont_exp の最初の壊しは同じ作業を両枝に置いたので compiler が枝を一つにまとめ memcheck が報告せず PASS した。壊し方を直して落ちることを確かめた。）

自分で決めた点（header が黙っていた所）:

1. `cryc_bn_mont_init` の誤りの順序: modulus が 0（size 0・全て 0）、偶数、1 は EINVAL を先に、その後に幅が MAX を越える物を ENOTSUP にした（偶数で幅も過ぎる物は不正が先で EINVAL）。header は EINVAL と ENOTSUP の条件を書くだけで両方に当たる時の順を書いていない。size_t の溢れを避けるため、先頭の 0 を除いた byte 数が 1026 を越える物は bits を計算せずに ENOTSUP。
2. R² は 2^(bits-1) から始め（m は奇数で 3 以上なので 2^(bits-1) < m）、mod_add による倍化を 64·limbs - (bits - 1) 回行う（modulus は公開なので変動時間でよい）。m0inv は Newton 反復（初期値 m の下位 limb、4 回）の符号を反転。
3. `cryc_bn_decode` は out を先に 0 で埋め、収まらない時の判定は branch でなく mask（EINVAL & ~mask）で行い、out を mask で 0 にする。header の「どちらの結果を返すかを除き時間は size と limbs だけで決まる」を、結果の分岐も秘密の byte に依らない形にした（ct の試験で秘密の byte のまま通る）。size > 4·limbs の位置も size と limbs だけで決まる branch。
4. `cryc_bn_reduce_once` は a が幅の中（limbs 個）に収まる値だけを扱う（2m が幅を越える modulus では a < 2m の全てが収まるとは限らず、header は「a < 2m」と書くだけ。収まる範囲では正しい）。借り（borrow）だけで選ぶ。
5. `cryc_bn_mont_mul` は語ごとの REDC（積の 1 行を足してから、m の倍数を足して 1 語落とす）。局所の total は limbs + 2 語、最後の引き算は total の最上位語と borrow から作った mask で select。値が m 以上の入力でも範囲外を読み書きしない（結果の値は定義しない）。出力の別名のため局所の buffer に作ってから最後に out へ書く。局所の total・diff・addend・mont_exp の power と factor は使った分を cryc_wipe。
6. `cryc_bn_mont_exp` は上の bit から平方と掛け算（最初の 1 の bit まで平方を飛ばす。指数は公開なので bit で分岐してよい）。base の複製を持つので out が base でもよい。size 0 と全て 0 の指数は R mod m。
7. 局所の buffer は CRYC_BN_MAX_LIMBS 語の配列（1 KiB）を、mod_add・mod_sub・reduce_once・mont_mul・mont_exp（2 つ）・to_mont の経路で使う（stack は最大で約 3 KiB）。限られた stack の機械では確認していない。
8. header と phase.md の食い違い: 見つからなかった。header の `cryc_bn_reduce_once` の comment の「FIPS 186-5 6.4.2 step 3's E」は、骨格の .c の comment では step 4 の e と書いてあり、どちらが正しいかは未確認（どちらも comment のみ。header を直す Phase p035 に持ち越し。仕様の確認は未実施。実装の comment には出典の節を書いていない）。

未実施: 性能の測定（mont_mul は O(limbs²)、mont_exp は 2048 bit で 17 bit の指数のみ測らず）、長い指数の mont_exp の 2048 bit 以上での参照との照合（参照の剰余が遅いため、24 limb 以上は指数 8 byte 未満（24〜64 limb は 2 byte の乱数、128・256 limb は 2 byte の乱数と 0x010001）。P-256・P-384 の p・n の p-2 は 32〜48 byte の指数で Fermat の性質のみ）、`significant` が size_t の溢れに近い大きさの init、32 bit の size_t の機械での実行、aarch64 Windows の実行（build のみ、check-skeleton）、実機・QEMU（範囲外）。

commit: 下の ws.md の表と同じ commit。

### Q1 の判断（2026-11-01 22:26）

- header の cryc_bn_reduce_once の comment（FIPS 186-5 6.4.2 の step 3 の E か step 4 の e か）は p035 の header の持ち越しの直しで仕様と照らして直す。

