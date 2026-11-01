<!-- awesome-plan project=zedbsd record=ws335-p023 -->

# ws335-p023: 公開の EVP の digest・ChaCha20・Poly1305

Status: cleared（2026-11-02 06:09 Q1、main で test-api-evp 190733・ct PASS。鍵の解放の 36 checks は p024 の後に流し直す）。前: 実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1908-i01。cleared は Q1）。前: planned（2026-11-01 p001 が作成。依存: p021、p006、p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `include/libc/compat/crypto/evp.h（digest・cipher・MAC）`、`userland/base/libcrypto-compat/cryc-api.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - OpenSSL 3.5 の API の文書（互換の振る舞いの正）: /home/awe/zedBSD-claude1/build/distfiles/openssl-3.5.8.tar.gz（SHA-256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2、展開は自分の build/tmp の新しい dir に。`doc/man3`・`doc/man7` の .pod は振る舞いの仕様として読み、文・code を写さない）: doc/man3/EVP_DigestInit.pod、EVP_EncryptInit.pod、EVP_MAC.pod、doc/man7/EVP_CIPHER-CHACHA.pod、EVP_MAC-Poly1305.pod。
  - `rfc8439.txt`（SHA-256 `25bef70fbf7a07ff45c2fe4cb7c6ce954eac687413d8610603268b4e4415324c`）: 2.3.2・2.4.2・2.5.2（値）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/api-evp.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-api-evp.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- EVP_chacha20 の IV の 16 byte = 状態の語 12〜15 の LE。counter は語 12・13 の 64 bit（header の注）。
- EVP_MAC_init は同じ ctx で何度でも。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh api-evp     # この module
sh plan/ws335/tests/run-unit.sh --win32 api-evp     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

- WS336 の契約の試験（ssh が呼ぶ分の受け入れ）: `/home/awe/zedBSD-worktrees/p5-ws336/plan/ws336/tests/crypto-contract.c`（main に入っていれば `plan/ws336/tests/crypto-contract.c`）を、
  自分の worktree の `build/tmp/` に libcrypto-compat の .c（win32 を除く）から作った静的な archive と `-I<dir の compat/ が include/libc/compat を指す物>` で build して流す
  （`SSH_CRYPTO_INCLUDE=… SSH_CRYPTO_LIB=… sh plan/ws336/tests/crypto-contract.sh` と同じ形。WS336 の worktree には書かない）。p024 の後に全件 PASS（p023 の時点では digest・cipher・MAC の分）。

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 04:36 JST）Sonnet 5.5、Queue q1908-i01。worktree s-ws335-p023（main 4a63d314a から）。実装と試験の commit は 9e76acd722（以降の直しの commit は Q1 への報告に記す）。

作った物: `userland/base/libcrypto-compat/api-evp.c`、`plan/ws335/tests/unit/test-api-evp.c`、`plan/ws335/tests/unit/ct-api-evp.c`（鍵・平文・message の秘密の扱い）。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh api-evp`（ASan・UBSan・LeakSanitizer） | `test-api-evp: PASS (190733 checks)` / `run-unit: PASS` |
| `LC_ALL=C.UTF-8 sh plan/ws335/tests/run-unit.sh --win32 api-evp`（x86_64・Wine） | `test-api-evp: PASS (190733 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh api-evp`（valgrind） | `ct-api-evp: PASS (31 checks)` / `run-ct: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 test と全 ct の 46 行が PASS（test-api-evp 190733・ct-api-evp 31 を含む）、最後の行 `run-unit: PASS` |
| `BASE=641a2f195 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（cc・clang・zedBSD の clang・llvm-mingw の 2 arch の warning 0、名前の範囲を含む） |
| `git diff --check` | 出力なし |
| WS336 の契約の試験（main の `plan/ws336/tests/crypto-contract.c` を libcrypto-compat の全 .c と ASan・UBSan で build して実行。`build/tmp/p023-contract/`） | `test-crypto-contract: FAIL (3 of 66 checks)`: 失敗は 3 件とも EVP_PKEY の生成（Ed25519・X25519 の `EVP_PKEY_new_raw_private_key` と ML-KEM の `EVP_PKEY_CTX_new_from_name`）で p024 の範囲。digest・ChaCha20・Poly1305・乱数・wipe の check は全部 PASS |

`BASE=b7dfd3952` では `check-skeleton` が FAIL する（`run-unit.sh` が b7dfd3952 から変わっているため。p021 と同じ）。BASE を 641a2f195 にして PASS（header と試験の道具の変更が無いことの照合）。

試験の内容:

- SHA-256: NIST の byte 単位の vector（ShortMsg 65 件、LongMsg 64 件、Monte 100 点。1 件ごとに EVP_Digest 1 回・update 1 回・長さ 0 を含む乱数の chunk・途中の `EVP_MD_CTX_copy_ex` の snapshot（コピー先が別の計算の途中の場合も、元に 1 byte を足しても copy が変わらない事）を、1 つの ctx を使い回して）、FIPS 180-4 の例（空・"abc"・448 bit・百万の a を 1 回と 1000 回の update）。
- 状態と誤り: 作った直後・final の後・start の後・refused の後の各関数の返り値と queue の 1 件の error（reason を全部）、失敗した呼び出しが ctx と出力 buffer を変えない事、final の後の ctx の hash の wipe と mode・md、`size` の NULL、`data` NULL と count 0。
- 署名・検証の ctx が持つ鍵の解放（free・start・copy の宛先）: `EVP_PKEY_free` が p024 の骨格（何もしない）の間は 36 件の check を飛ばす（`note:` を出す）。有効になる事は、mutation の道具の側の複写（参照を数える `EVP_PKEY_free`）で確かめた（baseline 190769 checks PASS）。
- ChaCha20: RFC 8439 の 2.3.2・A.1（5 件）・2.4.2・A.2（3 件）・2.6.2・A.4（3 件）を、IV = 32 bit の counter + 12 byte の nonce（状態の語 12〜15）で EVP 経由。参照の keystream は cryc-chacha.h の block 関数から 64 bit の counter ごとに作り（counter の上位 32 bit を語 13 に）、11 通りの counter（0・1・0xfffffffe・0xffffffff・2^32・2^32 の手前・2^64 の手前 など）× 23 通りの長さ × 3 つの使い方（update の out of place・in place・EVP_Cipher）× chunk の大きさ（1〜1100）と、乱数の 300 回で比べた。counter の範囲の終わり（2^64−1 の block を使える・次は拒否・拒否は何も書かず位置を変えない・新しい IV と鍵で再開）、設定の各経路（cipher だけ・IV だけ・鍵だけ・順序・direction 値・拒否が ctx を変えない・IV 無しは零の IV）、EVP_Cipher の返り値（0・長さ・`INT_MAX`・`0x80000000`）、消費した keystream の wipe。
- Poly1305: RFC 8439 の 2.5.2 と A.3（11 件）を 1 つの ctx を使い回して、1 回の update・乱数の chunk（空の update を含む）・途中で捨てて再開。fetch の名前（大文字小文字・近い名前の拒否）、各 key の長さ 0〜64 と `(size_t)-1`、拒否が進行中の計算を変えない事、final の room 0〜15 の拒否・大きい room・size の問い合わせ、final の後の state の wipe。
- AEAD（RFC 8439 2.8 を EVP の関数だけで組む）: 2.8.2、A.5、one-time key（2.8.2）、Wycheproof の chacha20_poly1305_test（96 bit の iv・128 bit の tag・256 bit の鍵の 316 件を封じて ciphertext と tag を照合、invalid は tag が合わない事。9 件は別の寸法で skip）。
- ssh の chacha20-poly1305@openssh.com の packet（nonce = 64 bit の BE の sequence 番号、block 0 が Poly1305 の鍵、block 1 から payload、別の鍵で 4 byte の長さ、tag は暗号化した長さと payload）を、cryc_chacha20_xor64 と cryc_poly1305 を直に呼んだ値と比べ、6 通りの sequence 番号 × 13 通りの大きさ。復号の往復も。

期待の値は NIST の vector・RFC の表・Wycheproof・block 関数からの参照・上の構成だけから作った。他の実装の出力は使っていない。OpenSSL の man（EVP_DigestInit・EVP_EncryptInit・EVP_MAC・EVP_MAC-Poly1305・EVP_CIPHER-CHACHA）は振る舞いの確認にだけ読んだ（文・code は写していない）。RFC の hex の表は p006 の試験（test-chacha.c）の data を写した（p006 で RFC の文面と照合済み）。

わざと壊す確かめ（git の外。`build/tmp/p023-mut/mut.py`、api-evp.c を 1 行ずつ 940 通りに書き換えた複写（比較・論理・算術・shift・定数 ±1・return 値・文の削除・条件を 0 と 1・error の reason と library・状態の定数・鍵の取り違え）、試験の `-fsanitize=address,undefined` の build と実行、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB か 600 秒で kill、4 並列。最後に実装の file の sha256 が変わっていない事を確認）:

- 最終の run（実装の sha256 5c07bbe0…）: build の誤り（`-Werror`）186、試験の check の FAIL 559、sanitizer の中断 115、見逃し 80。build の誤りの 186 は `-Werror` を外して書き直して再実行し、check の FAIL 99・sanitizer 17・見逃し 57・型の誤りで build できない 13（`return NULL` を 1 に、など意味の無い物）。
- 合計: 940 通り = check の FAIL 658（559 + 99）、sanitizer の中断 132（115 + 17）、見逃し 137（80 + 57）、build できない 13。
- 見逃しの 137 は全部、等価か実行できない経路: (a) calloc が NULL を返す経路（検査・error の積み・return NULL、4 か所）、(b) static の補助の `return 1`（呼ぶ側は 0 かどうかだけ見る）、`return 0` と NULL の取り違え（pointer の返り値の関数）、(c) object の使わない field の定数（key_size・iv_size・tag_size）、(d) free の前の wipe（free した memory を見る手段が無い。`cryc_wipe` の NULL は何もしない）と start の前の hash の wipe（`cryc_hash_init` が全体を memset する）と in-place の block の前の memset（空の buffer は常に 0 の約束）、(e) `cryc_hash_init`・`cryc_chacha20_xor64` の失敗の経路（SHA-256 は失敗しない・事前の検査で EOVERFLOW は出ない）、(f) 境の等価（`'a'`・`'z'` は期待の名前に無い文字、`reuse > available` の等号、`whole > 0`、`next_counter < counter`（blocks ≥ 1 で等号無し）、counter の上位と下位の `|` を `^`）、(g) `UNUSED_PARAMETER`、`ctx->cipher` に別の algorithm の pointer を入れる（pointer を NULL かどうかでしか見ない）、`md_ctx_drop_key` の NULL の検査（`EVP_PKEY_free(NULL)` は何もしない約束）。
- 初めの run で見えた見逃しのうち試験を足して塞いだ物: final の後の `ctx->md`、IV だけの設定の後の update の拒否（`key_set` を強制的に 1 にする壊し）、`EVP_Cipher` の `INT_MAX` の境（buffer を NULL にして reason で区別）、16 の倍数で 64 の倍数でない request が部分 block を数える事（counter の終わりでの 80・16・32・48 byte）、2 回の呼び出しで使い切った block の keystream の wipe。
- ct: handcrafted の 4 通り（update の平文・MAC の鍵・digest の data・cipher の鍵に分岐を足す）を valgrind が全部検出、元は clean。

自分で決めた点（header が黙っていた所。Q1 が確認する）:

1. 失敗は EVP library の error をちょうど 1 件積む（reason は null の引数 = `PASSED_NULL_PARAMETER`、NULL でなければならない引数・負の長さ・counter の範囲を越す request = `PASSED_INVALID_ARGUMENT`、別の algorithm・名前 = `EVP_UNSUPPORTED_ALGORITHM`、set up されていない・finish 済み = `EVP_OPERATION_NOT_INITIALIZED`、MAC の鍵の長さ = `EVP_INVALID_KEY_LENGTH`、tag の room 不足 = `EVP_BUFFER_TOO_SMALL`、メモリ無し = `MALLOC_FAILURE`）。成功は何も積まない。失敗した呼び出しは ctx と出力を変えない。getter（`EVP_MD_get_size(NULL)` = -1）は積まない。
2. algorithm は定数の object の pointer で見分ける（同じ内容の別の struct は拒否）。`EVP_DigestInit_ex` の type が NULL は拒否（前の algorithm の再利用はしない）。
3. digest: final の後は mode を NONE にし hash を wipe。update・final・copy 元は digest が進行中の ctx だけ（署名・検証の ctx は拒否）。`EVP_DigestFinal_ex` の md が NULL は拒否し進行中のまま。copy は自分自身へなら 1 で何もしない。copy の宛先と start の前の ctx が署名・検証の鍵を持っていれば `EVP_PKEY_free` で手放す。`EVP_Digest` は data が NULL なら count 0 だけ許す。
4. cipher: IV を一度も与えない時は零の IV（calloc の零の counter・nonce）。IV を与えると keystream を最初からやり直す。鍵だけを与えると buffer の keystream を捨て counter と nonce は変えない。cipher を NULL にして ctx に cipher が無い設定は拒否。direction は無視（どの int も可）。update は長さ 0 なら buffer が NULL でも成功、`out_length` が NULL は拒否。counter は 64 bit の little-endian（IV の最初の 8 byte）、nonce は残りの 8 byte。
5. counter の範囲の終わり: block 2^64−1 まで使える。範囲を越える request は何も書かずに拒否（位置は変わらない）。最後の block を作った後は `key_set` を 2（`CIPHER_KEY_LAST`、cryc-api.h の field の意味に足した内部の値。buffer に残った byte は使える）にし、新しい block を要る request を拒否。新しい IV で通常に戻り、鍵だけを与えても戻る（counter は 0 に wrap した値のまま、鍵が変わるので安全）。
6. `EVP_Cipher` は長さ 0 なら 0 を返し、`INT_MAX` を越す長さは buffer を見る前に -1。
7. consumed な keystream は block を使い切った時に wipe。partial block は ctx の buffer の中で零を暗号化して作る（局所の零の配列を持たない）。
8. MAC: 名前は ASCII の大文字小文字を区別しない。fetch した algorithm は定数の object で `EVP_MAC_free` は何もしない。ctx は fetch した pointer だけから作る。init は毎回 32 byte の鍵が要る（鍵を保たない）。start 前の update・final は拒否。`EVP_MAC_final` は out が NULL なら `*out_length` に 16 を入れて 1 を返し、計算は進めない（start 前でも、`out_length` も NULL なら拒否）。out_size が 16 未満は拒否し計算は進行中のまま。成功したら state を wipe して started を 0 に。
9. constant-time の試験では IV（counter と nonce）を公開の値とする（counter の範囲の判断が IV で分岐するため。ssh の nonce は sequence 番号）。

header の不備: 止める物は見つからなかった。header の comment の持ち越し（規約の見直しの Phase へ）: (a) `EVP_MAC_final` の out NULL の問い合わせ、(b) `EVP_MAC_init` は毎回鍵を要る事、(c) `EVP_MD_CTX_copy_ex` の元が finish 済み・署名の ctx の場合の拒否、(d) 範囲の終わりの counter の拒否と新しい IV・鍵での再開、(e) IV 無しは零の IV、(f) `EVP_CipherInit_ex` の鍵だけ・IV だけの時の buffer の扱い。骨格の comment の `EVP_Cipher` の「returns 1 or 0」は header の -1 と食い違っていたので header に従い、api-evp.c の comment も header と同じにした。

未実施: zedBSD・QEMU・実機での実行（範囲外）、aarch64 Windows の実行（check-skeleton で build と link のみ）、実際の Windows での実行（Wine のみ）、malloc の失敗の経路（失敗させる手段が無い）、free した memory の wipe の確認（手段が無い）、複数 thread からの同時の使用（header は object を 1 thread ずつ使うと約束）。`EVP_PKEY_free` の参照を数える動作が本物（p024）になった後の key の解放の 36 check は、p024 の merge 後に main で一度流す価値がある（この worktree では mutation の道具の側の複写でだけ確認）。
