<!-- awesome-plan project=zedbsd record=ws335-p005 -->

# ws335-p005: AES-GCM

Status: 実装済み・試験 PASS（2026-11-01 21:29 P6。cleared は Q1）。前: planned（2026-11-01 p001 が作成。依存: p004）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-gcm.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-sp-800-38d.pdf`（SHA-256 `d99f3921ccebca049e7522426553aba071dae14ec3d5b6041e8c111a6cb57bba`、印刷の頁 N は PDF の頁 N+8）: 5.2.1、6.3（Algorithm 1）、6.4（Algorithm 2）、6.5（Algorithm 3）、7.1（Algorithm 4）、7.2（Algorithm 5）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-gcmtestvectors-gcmEncryptExtIV128/192/256・gcmDecrypt128/192/256（FAIL の件は EBADMSG）、wycheproof-aes_gcm_test。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-gcm.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-gcm.c`。定数時間の試験: `plan/ws335/tests/unit/ct-gcm.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- GHASH の積は Algorithm 1 を mask で（H と data で分岐しない）。
- open は tag を照合してから平文を出す。失敗の時は out を 0 で埋める。
- Wycheproof の tag が 16 byte でない件は「範囲の外」として数えて飛ばす（件数を結果に）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh gcm     # この module
sh plan/ws335/tests/run-unit.sh --win32 gcm     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh gcm
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-01（host の `date` は 21:29 JST）P6（Sonnet 5.5）。worktree p6-ws335b（main e2cf3023a）。

作った物: `userland/base/libcrypto-compat/cryc-gcm.c`、`plan/ws335/tests/unit/test-gcm.c`、`plan/ws335/tests/unit/ct-gcm.c`。header・試験の道具・他の module は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh gcm` | `test-gcm: PASS (235197 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 gcm`（x86_64、Wine） | `test-gcm: PASS (235197 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 17 本 PASS（test-gcm 235197、aes 28516、chacha 52156、f25519 22903、hash 12965、sha2 14092、sha3 629616、tools 18、util 169160、x25519 4538、ct-gcm 21、ct-aes 4、ct-chacha 6、ct-hash 37、ct-sha3 1、ct-util 33、ct-x25519 4）/ `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh gcm` | `ct-gcm: PASS (21 checks)` / `run-ct: PASS`（valgrind memcheck） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0。cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲。途中で `SIZE_MAX` が llvm-mingw の `-Wlong-long`（`ULL` の定数）で落ちたので `(size_t)-1` に直した） |
| `git diff --check` | 出力なし |

試験の中身（235197 checks）:

- GF(2^128) の積: 手計算（0 倍、単位元 0x80..00 の左右、x^1・x^1 = x^2、x^1・x^127 = x^128 = x^7+x^2+x+1 = 0xe1 00..00、x^127・x^127 = x^254 を展開して 0xe6 08 .. 03 の bit 位置で）、out が x または y と同じ場合、Algorithm 1 を 1 bit 1 要素の配列で書いた参照（試験の中）と乱数 200 組、可換・分配・結合。
- GHASH: 空入力（NULL も）、size 0〜80 の参照との一致と端数を 0 で埋めた形との一致、block の境での分割が 1 回と等しい。
- CAVP（gcmEncryptExtIV と gcmDecrypt、鍵 128・192・256、IV 8・96・1024 bit）: encrypt 23625 件（鍵 3 種 × 7875）の全件で ciphertext と tag（tag が 16 byte 未満の件 20250 は先頭の taglen/8 byte）を照合、うち tag 16 byte の 3375 件は open の往復（in-place も）・tag の 1 bit 変更・ciphertext の 1 byte 変更・AAD の 1 byte 変更・IV の 1 byte 変更が EBADMSG で out が 0 になることまで。decrypt は tag 16 byte の 3375 件（FAIL の件は EBADMSG で out が 0、他は平文が一致）。tag が 16 byte 未満の decrypt 20250 件は範囲の外として数えて飛ばした。
- Wycheproof aes_gcm_test: 316 件全部（tag は全件 128 bit、範囲の外は 0 件）。valid 229 件（CounterWrap 36 件、IV 8〜2056 bit を含む）は seal の ciphertext・tag と open の平文が一致、invalid 87 件のうち IV 0 byte の 6 件は seal・open とも EINVAL で何も書かない、残りは EBADMSG で out が 0。
- 参照との比較（試験の中に SP 800-38D の 7.1 を単独で書いた: J0、GCTR を block ごと、GHASH、tag）: 鍵 3 種 × IV 8 通り（1・8・11・12・13・16・17・60 byte）× AAD 4 通り（0・1・16・33）× 平文 12 通り（0〜1000 byte、block の境と 64・256 の前後）を、大きさぴったりの malloc の buffer（ASan）で、seal・open・in-place の往復と、seal が鍵の構造体を変えないこと。
- 偽造: 16 byte の鍵で IV 1・12・16 byte のそれぞれ、ciphertext 40 byte の全 bit・tag の全 bit・AAD 20 byte の全 bit・IV の全 bit の変更、AAD・ciphertext を 1 byte 短く、IV を 1 byte 短く、AAD と ciphertext の取り違え、乱数の tag と乱数の ciphertext 50 回が、全部 EBADMSG で out が 0（in-place も）。
- 範囲の外: 空の IV（EINVAL、out・tag 不変）、平文が上限（2^32-2 block）を 1 byte 越える・IV か AAD が 2^61 byte・size が (size_t)-1（EINVAL、out・tag 不変。上限の確認は size_t が 64 bit の時だけ）、size 0 で in・out・aad が NULL。init は key_size 0〜64 と (size_t)-1（16・24・32 以外は EINVAL で構造体が全部 0）、key が NULL（EINVAL で全部 0）、H が AES の 0 block の暗号と一致、clear で全部 0。

わざと壊す確かめ（git の外で .c を書き換え、`run-unit.sh gcm` を流し、`cmp` で元に戻ったことを毎回確かめた）。54 通り（最初の 55 通りのうち 1 通りは build の誤りだったので書き直した、下記）のうち:

- 50 通りは試験の失敗で落ちた。`test-gcm: FAIL (exit; ...)` で、`(build)` の失敗ではないことを全件の出力で区別した（build の誤りは `test-gcm: FAIL (build)` と出る）。R の定数、carry の shift 量、x の bit 順、select・reduce の mask の反転と取り違え、`|=`、128 step を 127 に、store の取り違え、GHASH の block の進み・xor を or に・padding を消す、H の入力、init の誤りの戻り値、seal・open の inc32 を消す、inc32 の +2・読む位置、J0 の 96 bit の counter 値・IV の bit 長（*4・位置 0）・長さ block を消す、tag の長さの入れ替え・bit でなく byte・AAD の長さ・GHASH の順、E(J0) の mask を消す・別の block から、tag を平文から、open の比較を 15 byte（先頭からと末尾から）、mask の反転・error の反転・平文を mask しない・mask を下位 7 bit に、chunk の進み 255・上限 255・in と out の offset を消す、空の IV を許す、上限の定数 +16 byte、AAD・IV の 2^61 の上限を消す、counter の増分（全 block 同じ・IV を 11 byte copy・xor を 1 byte 短く・端数 block を落とす・pass ごとに 1・chunk 63 byte）、clear が H を残す、tag の最後の 1 byte を書かない、E(J0) を length block から作る。
- 見逃し 4 通りは、いずれも試験で区別できない（等価）か、この host で実行できない:
  - init の最初の wipe を消す: 失敗の経路が最後にもう一度全部を wipe し、成功の経路は全部を上書きするので、動作が同じ（等価）。
  - 平文の上限の比較を `>` から `>=` に: 上限ちょうど（2^36 byte、約 64 GiB）の呼び出しを試験で作れず、区別できない（上限 +16 byte に緩める壊しは落ちる）。
  - size が SIZE_MAX - 16 を越えるかの検査を消す: 64 bit の機械では上の平文の上限が先に拒むので動作が同じ。32 bit の size_t の機械でだけ効く（この host では実行できない）。
  - counter を pass の最後に 4 進める（block 数でなく）: 端数の pass は呼び出しの最後にしか来ず、戻る counter は使わないので動作が同じ（等価）。
- BUILD-ERROR になった 1 通り（tag の mask を別の block から作る、未使用引数の警告）は数に入れず、E(J0) を length block から作る形に書き直して、試験の失敗で落ちることを確かめた（上の 50 通りに含む）。
- 定数時間の壊し（run-ct.sh gcm）: 乗算の x の bit で branch（GHASH の鍵 H が秘密）、reduce の mask を branch に（volatile の副作用つき）、GHASH の状態への branch、open の tag の判定結果での branch、の 4 通りが `run-ct: FAIL`（memcheck）で、戻して PASS。別の 3 通り（mask を `if` に直しただけで副作用が無いもの、memcmp の結果を `== 0` で使うもの、平文 byte の branch で副作用が無いもの）は compiler が分岐を消した（cmov・setcc）ため memcheck が報告せず、PASS のままだった（試験の限界）。

自分で決めた点（header が黙っていた所）:

1. GHASH の積は Algorithm 1 を 64 bit の 2 つの半分（上位・下位）で、128 回の loop。x の bit の取り出しの index は公開の loop 変数。out が x・y と同じでも、入力を全部読んでから書く。
2. `cryc_gcm_ghash` の端数は 16 byte の局所 buffer に 0 で埋めて使う（data を読み過ぎない）。GHASH の入力は暗号文・AAD・IV で公開のため、この buffer は wipe しない。
3. IV が 12 byte 以外の時の J0 は、IV を GHASH し、続けて「0 の 64 bit と IV の bit 長」の block を GHASH（7.1 の step 2）。IV と AAD の長さを bit にして 64 bit に入れるため、IV・AAD が 2^61 byte 以上は EINVAL（header は空の IV と平文の上限だけを書いていた。64 bit の機械では実際に作れない大きさ）。
4. 平文の上限は header どおり 2^32-2 block。それに加えて size が `(size_t)-1 - 16` を越えると EINVAL（32 bit の size_t の機械向け、header の注の「SIZE_MAX - CRYC_GCM_TAG」）。
5. counter mode は `cryc_aes_ctr32_xor` を使わず、cryc-gcm.c の中に `gcm_counter_xor`（static）を置いた。理由: 12 byte でない IV の時、J0 の下位 32 bit は秘密の H に依存する値で、`cryc_aes_ctr32_xor` の counter block を作る loop（trip count が block 数）を compiler が counter の値を induction variable にした形にし、ct-gcm が memcheck の「条件分岐が未初期化値に依存」で落ちた（p004 の結果の 7 番と同じ現象。timing の差は無い偽陽性だが、ct の試験を通せない）。`gcm_counter_xor` は counter block を常に 4 個（固定の回数）作り、使う block 数だけ `cryc_aes_encrypt_blocks` に渡す。AES の module（p004）は変えていない。header の「counter mode は cryc-aes.h の ctr32_xor」という言い方は header の comment には無いので矛盾しない。
6. `cryc_gcm_open` は tag を先に作り、平文は 256 byte ずつ局所 buffer に作って、good_mask（tag が合えば全 bit 1）との AND で out に書く。tag が合わなければ out は 0 で、tag の検査の結果で分岐しない（返す値も `EBADMSG & ~mask` の算術）。ct の試験は、結果と平文を呼び出しの後に公開に印す（戻り値が呼び出し側に tag の結果を知らせるため）。副作用: out が in と同じでも、tag が不正なら暗号文が 0 に潰れる（header は「out は 0 で埋める」と書いている）。
7. `cryc_gcm_open` と `cryc_gcm_seal` は先頭で大きさを検査し、EINVAL の時は out・tag に書かない。`cryc_gcm_init` は最初に構造体全部を wipe し、key が NULL の時も EINVAL（p004 と同じ扱い）。
8. coding-style の「関数の最後は success の return」について、`cryc_gcm_open` は最後が `return error;`（0 か EBADMSG を算術で作った値）。tag の判定を分岐にしないため、成功の return を別に分けていない（コメントで理由を書いた）。

header・phase.md と仕様の食い違い: 見つからなかった。

未実施: 性能の測定（GHASH は 1 block あたり 128 回の loop で、表引きの形より遅い見込み）、32 bit の size_t の機械での実行（`(size_t)-1 - 16` の検査）、上限ちょうどの 64 GiB の呼び出し、aarch64 Windows の実行（build のみ、check-skeleton）、実機・QEMU（範囲外）。

commit: 下の ws.md の表と同じ commit。
