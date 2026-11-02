<!-- awesome-plan project=zedbsd record=ws335-p024 -->

# ws335-p024: 公開の EVP_PKEY（Ed25519・X25519・ML-KEM-768）

Status: 実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1913-i01。cleared は Q1。2026-11-01 p001 が作成。依存: p021、p009、p008、p014）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `include/libc/compat/crypto/evp.h（PKEY・DigestSign/Verify）`、`userland/base/libcrypto-compat/cryc-api.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - OpenSSL 3.5 の API の文書（互換の振る舞いの正）: /home/awe/zedBSD-claude1/build/distfiles/openssl-3.5.8.tar.gz（SHA-256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2、展開は自分の build/tmp の新しい dir に。`doc/man3`・`doc/man7` の .pod は振る舞いの仕様として読み、文・code を写さない）: doc/man3/EVP_PKEY_new.pod、EVP_DigestSignInit.pod、EVP_DigestVerifyInit.pod、EVP_PKEY_derive.pod、EVP_PKEY_CTX_new.pod、EVP_PKEY_keygen.pod、EVP_PKEY_encapsulate.pod、EVP_PKEY_decapsulate.pod、doc/man7/EVP_PKEY-ML-KEM.pod、EVP_PKEY-X25519.pod、EVP_SIGNATURE-ED25519.pod。
  - `rfc8032.txt`（SHA-256 `ed63657ff389301282b169b0abde9b5dd2c7e4d524fdfa5da6ff3094fc93c4c3`）: 7.1。
  - `rfc7748.txt`（SHA-256 `279ca0ecc5e92e2962e27b846986aeb74729d9dd34bd4a04a362f80dcb596ad3`）: 6.1。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: wycheproof-mlkem_768_test（EVP 経由で）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/api-pkey.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-api-pkey.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 参照の数は atomic。秘密の鍵は free で消す。
- EVP_PKEY_derive の全零は失敗。
- ML-KEM の私鍵は export しない。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh api-pkey     # この module
sh plan/ws335/tests/run-unit.sh --win32 api-pkey     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 09:37 JST）Sonnet 5.5、Queue q1913-i01。worktree s-ws335-p024（main 4eae22020 から）。

作った物: `userland/base/libcrypto-compat/api-pkey.c`、`plan/ws335/tests/unit/test-api-pkey.c`、`plan/ws335/tests/unit/ct-api-pkey.c`（鍵・seed・scalar・dk・message の秘密の扱い）。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh api-pkey`（ASan・UBSan・LeakSanitizer） | `test-api-pkey: PASS (40701 checks)` / `run-unit: PASS` |
| `LC_ALL=C.UTF-8 sh plan/ws335/tests/run-unit.sh --win32 api-pkey`（x86_64・Wine） | `test-api-pkey: PASS (40701 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh api-pkey`（valgrind） | `ct-api-pkey: PASS (20 checks)` / `run-ct: PASS` |
| `sh plan/ws335/tests/run-unit.sh api-evp ct-api-evp` | `test-api-evp: PASS (190769 checks)`（p023 の 190733 + 鍵の解放の 36 checks が飛ばずに流れた）、`ct-api-evp: PASS (31 checks)` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 test と全 ct の 50 行が PASS（test-api-pkey 40658・ct-api-pkey 20、後で 43 checks 足した）、最後の行 `run-unit: PASS` |
| `BASE=641a2f195 sh plan/ws335/tests/check-skeleton.sh` | `FAILED headers or test tools changed since 641a2f195: userland/base/libcrypto-compat/cryc-x509.h`（p019 の承認済みの 1 語の変更。この Phase の変更ではない） |
| `BASE=4eae22020 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（cc・clang・zedBSD の clang・llvm-mingw の 2 arch の warning 0、名前の範囲を含む） |
| `git diff --check` | 出力なし |
| WS336 の契約の試験（main の `plan/ws336/tests/crypto-contract.c` を libcrypto-compat の全 .c（win32 を除く）と ASan・UBSan で build して実行。`build/tmp/p024-contract/`） | `test-crypto-contract: PASS (108 checks)`（p023 の時は 66 checks のうち 3 件の EVP_PKEY の失敗） |

試験の内容:

- 生の鍵: 型・長さ・NULL・engine・名前（大文字小文字・近い名前の拒否）・libctx・propq の全ての拒否が 1 件の error（reason を全部）、失敗が出力 buffer と長さを変えない事、Ed25519 の seed（RFC 8032 7.1 TEST 1）と X25519 の scalar（RFC 7748 6.1）から公開鍵、unclamped な scalar がそのまま戻る事、公開鍵だけの鍵の往復（点でない値・全 0・全 0xff も）と私鍵の取り出しの拒否、size の問い合わせ・ちょうどの room・多い room・足りない room（0〜31 の全部）。
- 参照の数: 鍵を持つ全ての持ち主（EVP_PKEY_CTX_new・new_from_pkey、DigestSign/VerifyInit、derive の peer）が参照を取り、free・再 start・別の鍵・拒否・context の解放で正しく戻る事、同じ鍵や同じ peer の再設定、拒否が何も変えない事、鍵を先に free しても context の参照で生きる事。4 thread × 5000 回の取り・手放しで参照が釣り合う事。
- Ed25519: RFC 8032 7.1 の 5 vector と 6 の説明の python で作った 0〜1000 byte の message の署名（署名の byte が一致、検証できる、署名の 64 byte の全位置の 1 bit の反転と別の message で検証が 0）、Wycheproof ed25519_test の 210 件（valid 117・invalid 64（うち別の長さの署名 12）・acceptable 29 のうち header の式で受理される 8 を EVP_DigestVerify 経由で）、署名の長さ 0〜130 と `(size_t)-1`、NULL の組み合わせ、状態（未 start・署名の ctx で検証・検証の ctx で署名・digest の ctx からの切り替え）、署名の buffer の room 0〜63、message を NULL で長さ 0、署名が message と重なる場合、復号できない公開鍵、乱数の seed と 21 通りの message の長さ（SHA-512 の block の境を含む。100000 byte まで）で EVP の署名が primitive の署名と一致。
- X25519: RFC 7748 5.2 の 2 vector と iterated test（1 回・1000 回）、6.1 の Alice・Bob を EVP の context 経由で、Wycheproof x25519_test の 518 件（全零の結果は失敗・error 1 件・出力が零で length 不変）、状態（鍵の型・公開鍵だけ・未 init・peer 無し・peer の型違い）、room の全部、200 組の乱数の鍵の往復と primitive との一致。
- ML-KEM-768: Wycheproof の mlkem_768_test の 201 件（seed から作った鍵が published の ek を持ち、c を K に decapsulate（implicit rejection の物も）、ek を名前で import して encapsulate → EVP と primitive の decapsulate が一致、誤りの長さの ciphertext 20 件が拒否）、mlkem_768_encaps_test の 265 件（ek は 1184 byte で全 coefficient が q 未満の時だけ受理: 受理 133・長さで拒否 20・modulus で拒否 112）、mlkem_768_semi_expanded_decaps_test の 9 件（dk の長さが正しい 7 件のうち、valid 3 件が K に一致、長さの誤りの ciphertext 2 件が拒否）、EVP の keygen を 24 回（sizes、dk の hash check、dk の中の ek、2 つの鍵が違う、3 回の encapsulate で ciphertext が違い K が一致、ciphertext の 12 か所の bit 反転が error なしで別の K（primitive の implicit rejection と一致））、名前と各関数の拒否と room の全部、ek の import の check（384 個の 3 byte の group の全部で 3329 と 4095 の coefficient を拒否、全 0 と 3328 は受理、rho の部分は検査しない）、modulus の check に通らない公開鍵の object で encapsulate が 1 件の error。

期待の値は RFC 8032・7748 の表（RFC の本文 rfc7748.txt・rfc8032.txt の sha256 は phase.md の値と一致を確認）・Wycheproof・説明の python の署名・primitive（ML-KEM の implicit rejection と、EVP と独立の decapsulate の照合）だけから作った。他の実装の出力は使っていない。OpenSSL の man は振る舞いの確認にだけ読んだ（文・code は写していない）。

わざと壊す確かめ（git の外。`build/tmp/p024-mut/mut.py`: api-pkey.c の 1 行ずつを 1715 通りに書き換えた複写（比較・論理・if 条件の 0/1/反転・節の削除・return 値・定数 ±1・reason と型と mode と operation と大きさの定数の入れ替わり・文の削除・private と public の入れ替わり）、test の最初の失敗で終わる複写（check.h の複写を直し、api-pkey.o と試験の object を link）、ASan・UBSan、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB か 600 秒で kill、4 並列。最後に実装の file の sha256（0433d907…）が変わっていない事を確認。baseline は PASS）:

- 試験の check の FAIL 951、sanitizer の中断 134、見逃し 125、build の誤り（`-Werror`）505。
- build の誤り 505 は `-Werror` を外して書き直して再実行し: check の FAIL 249、sanitizer 36、見逃し 100、型の誤りで build できない 120（int の関数で `return pkey;`、宣言の削除など意味の無い物）。
- 合計: check の FAIL 1200、sanitizer の中断 170、見逃し 225、build できない 120（= 1715）。
- 見逃しの 225 のうち、試験を足して塞いだ物 3（公開鍵だけの鍵が private_key・private_size を持たない事 1、`EVP_PKEY_CTX_new`・`new_from_pkey` の context の keygen_type 2。塞いだ後 3 通りとも check の FAIL、baseline は PASS）。残る 222 は等価か実行できない経路: (a) calloc・malloc が NULL を返す経路（検査・error の積み・戻り値・free、40 余り）、(b) `return NULL` と `return 0` の取り違え（NULL は 0 の定数）と結果が NULL の時の `return pkey`・`return ctx`、(c) 同じ値の定数の入れ替え（`CRYC_X25519_SIZE`・`CRYC_MLKEM_SHARED` は共に 32、`CRYC_PKEY_ED25519` は 1）と大きすぎる stack の配列、(d) wipe の削除（free した memory・stack の消去は見る手段が無い）、(e) calloc が零にする field を零にする文の削除、(f) 乱数の失敗（`cryc_os_random` を失敗させる手段が無い）の分岐、(g) `ctx->key == NULL` と `ctx->operation` を落とす物（mode・peer が設定されていれば key・operation は必ず設定済み）、(h) 名前の folding の境の `'a'`・`'z'`（期待の名前に無い文字）、(i) `tbs` の NULL の置き換え（空の message に NULL を渡しても primitive は触らない）と `empty[]` の大きさと値、(j) 初期化しない `pkey_type`（未初期化の stack）。
- 手で作った constant-time の壊し 8 通り（鍵の seed・scalar・message・dk・共有鍵・書き出す private の byte で分岐、secret の添字の番地）は valgrind が全部検出（元は clean）。

自分で決めた点（header が黙っていた所。Q1 が確認する）:

1. 失敗は EVP library の error をちょうど 1 件積む（reason は null の引数 = `PASSED_NULL_PARAMETER`、NULL でなければならない引数・digest を渡した署名・長さの誤りの ciphertext = `PASSED_INVALID_ARGUMENT`、別の型・名前・鍵の型の違う操作 = `EVP_UNSUPPORTED_ALGORITHM`、鍵の長さ = `EVP_INVALID_KEY_LENGTH`、ek が modulus の check に通らない・公開鍵だけの鍵で私鍵が要る操作・全零の共有鍵 = `EVP_INVALID_KEY`、未 init = `EVP_OPERATION_NOT_INITIALIZED`、room 不足 = `EVP_BUFFER_TOO_SMALL`、peer の型違い = `EVP_DIFFERENT_KEY_TYPES`、ML-KEM の私鍵の取り出し = `UNSUPPORTED`、乱数の失敗 = `INTERNAL_ERROR`、メモリ無し = `MALLOC_FAILURE`）。成功は何も積まない。失敗は鍵・context・出力 buffer・length を変えない（例外は header の言う `EVP_PKEY_derive` の全零で、出力 32 byte を零にして length は変えない）。
2. 署名の検証が通らない事は error でなく判定（0・何も積まない）。署名の長さが 64 でない事も 0（message や NULL の検査より前）。未 start の ctx と NULL の ctx は -1 と error 1 件。署名が NULL で長さ 64、message が NULL で長さ 0 でない場合は 0 と `PASSED_NULL_PARAMETER`。
3. 署名・検証の ctx は鍵への参照を持ち、start で同じ鍵なら参照を先に増やしてから古い鍵を手放す。start は digest の state を wipe し `mode`・`md = NULL` にする。署名・検証の ctx は何度でも署名・検証できる。署名は中の buffer に作ってから書く（message と署名の buffer が重なっても安全）。`pctx` は成功で `*pctx = NULL`、失敗は触らない。`type` が NULL でないと拒否（EVP_sha256 でも）。Ed25519 でない鍵は署名・検証とも拒否。
4. `EVP_PKEY_new_raw_public_key`・`_ex` は Ed25519 の点として decode できるかを調べない（検証が 0 になる）。X25519 は任意の 32 byte（RFC 7748 5）。型の名前は ASCII の大文字小文字を区別しない（`ml-kem-768` 可）。`EVP_PKEY_new_raw_public_key` に ML-KEM の数値の型は無く拒否。
5. `EVP_PKEY_get_raw_private_key` は ML-KEM を（鍵があっても size の問い合わせでも）`UNSUPPORTED`、公開鍵だけの Ed25519・X25519 を `EVP_INVALID_KEY`、room 不足は `length` を変えずに拒否。
6. 参照の数: `EVP_PKEY_free` は `cryc_os_atomic_add(-1)` が 0 になった呼び出しが private_key を wipe して free し、object も wipe して free する。負の数（二重 free）は何もしない。
7. `EVP_PKEY_derive_init` は peer を手放す（毎回 set_peer が要る）。`EVP_PKEY_derive_set_peer` は X25519 だけ（`EVP_DIFFERENT_KEY_TYPES`）、新しい参照を先に取ってから古い peer を手放す。peer は公開鍵だけでよい。
8. `EVP_PKEY_keygen` は成功したら `*pkey` に新しい鍵を書き、失敗は `*pkey` を触らない。成功時に `*pkey` が以前持っていた pointer は手放さない（呼び出す側の持ち物）。一つの context で何度でも生成できる。生成した鍵は dk の hash check に通り、ek は dk の中の ek と同じ（試験で確認）。
9. `EVP_PKEY_encapsulate` は 2 つの length が常に必要。両方の buffer が NULL なら sizes の問い合わせ、片方だけ NULL は `PASSED_NULL_PARAMETER`。room が足りなければ乱数を使う前に拒否。ek が modulus の check に通らない鍵 object（インターフェース外で作った物）は `EVP_INVALID_KEY`。`EVP_PKEY_decapsulate` は key が NULL なら length だけを返し、ciphertext の検査は後（key が NULL の時は ciphertext を見ない）。ciphertext が 1088 byte でなければ `PASSED_INVALID_ARGUMENT`。dk の hash check は import が無いので decapsulate では繰り返さない。
10. `EVP_PKEY_CTX_new_from_name` で ML-KEM-768 以外は `EVP_UNSUPPORTED_ALGORITHM`（Ed25519・X25519 の名前も。keygen は ML-KEM だけ）。keygen_init は鍵を持つ context（new・new_from_pkey）を拒否。
11. constant-time の試験では、`EVP_PKEY_derive` の全零の判定が秘密から作った値での分岐（RFC 7748 6.1 で公開してよい）なので、valgrind に区別できず、derive は scalar を公開にして流す（primitive の arithmetic は ct-x25519）。keygen と encapsulate は OS の乱数（秘密の印を付けられない）から作るので ct-mlkem で primitive を見る。

header の不備: 止める物は見つからなかった。header の comment の持ち越し（規約の見直しの Phase へ）: (a) 失敗時の reason の割り当て、(b) 署名の検証の戻り値（長さの誤りは 0・判定は error を積まない・NULL の ctx は -1）、(c) `EVP_PKEY_keygen` が `*pkey` の古い pointer を手放さない事、(d) `EVP_PKEY_derive_init` が peer を手放す事、(e) `EVP_DigestSign` の size の問い合わせにも start が要る事、(f) 型の名前の大文字小文字、(g) `EVP_PKEY_encapsulate` の片方だけ NULL の拒否、(h) 全零の derive は `length` を変えない事。

未実施: zedBSD・QEMU・実機での実行（範囲外）、aarch64 Windows の実行（check-skeleton で build と link のみ）、実際の Windows での実行（Wine のみ）、malloc と乱数の失敗の経路（失敗させる手段が無い）、free した memory の wipe の確認（手段が無い）、`EVP_PKEY_derive` の全零の判定の constant-time（上の 11）、実装を足した 43 checks（公開鍵だけの鍵の private の field と context の field）の後で `run-unit.sh all` は流し直していない（api-pkey・win32・ct・api-evp は流し直した）。

