<!-- awesome-plan project=zedbsd record=ws335-p004 -->

# ws335-p004: AES（bitsliced、定数時間）と counter mode

Status: cleared（2026-11-01 20:55 Q1、main adf13ed5c。main で test-aes 28516 checks・ct-aes PASS を流し直した。P6 が決めた 5 点（schedule の配置、S-box の回路、NULL key の EINVAL、size 0、counter は公開の値）は妥当と判断。壊す確かめは 24 通りが FAIL、試験の失敗と build の誤りの区別は 5 通りだけ）。前: 実装済み・試験 PASS（2026-11-01 20:55 P6。依存: p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-aes.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `nist-fips-197-upd1.pdf`（SHA-256 `62c86eb567f13edb8f71826e985da870b04ef6381634f303cdb16e84d47becd1`、印刷の頁 N は PDF の頁 N+8）: 3.4、4.2（積）、4.4（逆元）、5.1（Algorithm 1）、5.1.1（式 (5.3)、Table 4）、5.1.2〜5.1.4、5.2（Algorithm 2、Table 5）、付録 A・B・C（試験の値）。
  - `nist-sp-800-38d.pdf`（SHA-256 `d99f3921ccebca049e7522426553aba071dae14ec3d5b6041e8c111a6cb57bba`、印刷の頁 N は PDF の頁 N+8）: 6.2（inc32）、6.5（GCTR）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-KAT_AES-ECB*（GFSbox・KeySbox・VarKey・VarTxt、128/192/256）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-aes.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-aes.c`。定数時間の試験: `plan/ws335/tests/unit/ct-aes.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 表引きの S-box は禁止（header の冒頭）。S-box は GF(2^8) の逆元（b^254）と affine を bit-slice の論理演算で。試験で 256 個全部を Table 4 と照合（Table 4 は頁の画像でも照合）。
- 4 block を uint64_t の 8 語に bitslice する形を推す（header の CRYC_AES_SCHEDULE_WORDS = 120 はその大きさ）。
- AES-192 も（CAVP にある）。逆の暗号は作らない。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh aes     # この module
sh plan/ws335/tests/run-unit.sh --win32 aes     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh aes
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-01（host の `date` は 20:53 JST）P6（Sonnet 5.5）。worktree p6-ws335b（main c4f4fa7a7）。

作った物: `userland/base/libcrypto-compat/cryc-aes.c`、`plan/ws335/tests/unit/test-aes.c`、`plan/ws335/tests/unit/ct-aes.c`。header・試験の道具・他の module は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh aes` | `test-aes: PASS (28516 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 aes`（x86_64、Wine） | `test-aes: PASS (28516 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 15 本 PASS（test-aes 28516、chacha 52156、f25519 22903、hash 12965、sha2 14092、sha3 629616、tools 18、util 169160、x25519 4538、ct-aes 4、ct-chacha 6、ct-hash 37、ct-sha3 1、ct-util 33、ct-x25519 4）/ `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh aes` | `ct-aes: PASS (4 checks)` / `run-ct: PASS`（valgrind memcheck） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0。cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

試験の中身（28516 checks の内訳の主な物）:

- S-box 256 個全部: 鍵 64 通り（w[3] の 4 byte に 4*base+j）で鍵展開の最初の SubWord を読み、Table 4 と照合（実装の SubBytes は byte 毎の表を持たないので、内部の S-box を鍵展開の SubWord 経由で読む）。Table 4 は `pdftotext -layout` の text と、定義（逆元 b^254 と式 (5.3)）から python で作った表と、PDF の頁 22 の画像（先頭の行）で照合した。
- 付録 A.1・A.2・A.3 の鍵展開: 全ての w[i]（128 は w4〜w43、192 は w6〜w51、256 は w8〜w59）を、実装の schedule（.c の冒頭に書いた slice の配置）から読み戻して照合。4 lane の複製と、最後の round key より後の schedule の語が 0 であることも。
- 付録 B の cipher の例（3243f6a8... → 3925841d...）。
- CAVP の KAT_AES ECB（GFSbox・KeySbox・VarKey・VarTxt の 128・192・256 の 12 file）の全 record（ENCRYPT と DECRYPT の section の record 全部の平文→暗号文の向き）。
- 手書きの参照 cipher（FIPS 197 の Algorithm 1・2 を byte 単位で。表は Table 4）と、鍵 3 種 × 0〜9 block（各 5 回）と 64 block の乱数の入力で比較。ASan のために大きさぴったりの malloc、in と out が同じ buffer の場合、sentinel（書き過ぎ）。
- counter mode: 参照の GCTR（block 毎に単独の暗号 + inc32 を byte で）と、鍵 3 種 × size 0〜150 byte × counter 9 通り（0・1・2・0xfffffffe・0xffffffff・0xfffffff0・0x00ffffff・0x7fffffff・0x80000000、wrap を含む）、in と out が同じ場合、1000 byte（途中で wrap）。戻る counter（使わなかった次の block）と size 0 で変わらないこと。
- Wycheproof の AES-GCM の vector（IV 12 byte、result valid、鍵 128・192・256）の暗号文: counter IV || 00000002 から始めた ctr32_xor の出力と一致、戻る counter の末尾が 2 + ceil(size/16)。
- 誤り: key_size 0〜64 と (size_t)-1（16・24・32 以外は EINVAL で構造体が全部 0）、key が NULL（EINVAL・全部 0）、clear 後に全部 0、encrypt が構造体を変えない、count 0 で何も書かない（NULL の buffer でも）。

わざと壊す確かめ（git の外で .c を書き換え、run-unit.sh aes を流し、`cmp` で元に戻ったことを確かめた。24 通り全部 FAIL）: S-box の affine の定数（0x63 → 0x62）、二乗の式の 1 項、ShiftRows の mask（row 2）と row 3 の shift 量、MixColumns の XTIMES の 1 項と行の回転の量、round 数の off-by-one、既約多項式の定数（0x1b → 0x1a）、AES-256 の途中の SubWord の条件（% 4 → % 5）、Rcon の初期値、ctr の counter の進め方（+1 の誤り）・部分 block を落とす・counter の IV 部分の copy を 11 byte に・block の増分、XOR の index、init の key_size の条件（256 の 32 → 31）、init の最初の wipe を消す、NULL key の検査を消す、lane の複製の定数、encrypt_blocks の group の前進（16 → 15）、最後の round key の取り違え、clear が全部を消さない、乗算の折り返しの項、schedule の未使用の語に 1 を書く。そのうち 5 つ（init の wipe を消す・NULL key・clear・schedule の末尾・block の増分）は FAIL の理由が build でなく試験の失敗（`test-aes: FAIL (exit ...)`）であることを確かめた。見逃しは無かった。ct の確かめ: aes_store に秘密の byte での branch（`if (value == 0xa5) hits++`）を足すと `run-ct: FAIL`（memcheck）、戻して PASS。

自分で決めた点（header が黙っていた所）:

1. schedule の配置: `schedule[8 * k + i]` が round key k の slice i。slice の bit 16 * b + n は、block b（lane）の byte n の bit i。round key は 4 lane 全部に同じ物を複製。最後の round key より後の語は 0。.c の冒頭に書いた。test-aes.c は鍵展開の検証にこの配置を読む。
2. 状態の配置: 4 block を 8 語（slice）に。byte n = 行 r + 4 * 列 c（3.4）で、block は 16 bit の lane。端数の group（4 block に満たない）は残りの lane の byte を 0 で埋め、読み戻さない。
3. S-box: GF(2^8) の逆元を b^254 の加算鎖（2・3・12・15・240・252・254、乗算 4 回と二乗 7 回、二乗は線形な式）で、式 (5.3) の affine。鍵展開の SubWord は同じ回路に 4 byte を載せて使う（byte 毎の表は無い）。
4. 鍵展開は byte 単位でなく 32 bit の語で行い（Algorithm 2）、Rcon は x^(j-1) を XTIMES で作る（public の定数なので分岐してよい）。
5. `cryc_aes_init`: key が NULL の時は EINVAL（header は key_size の誤りだけを書いていた）。全ての誤りで構造体は全部 0（init の最初に wipe）。
6. `cryc_aes_ctr32_xor`: size 0 の時は counter を読まずに戻る（NULL の counter も許す。header は size 0 で counter 不変とだけ）。counter の下位 32 bit は uint32_t の wrap（定義された動作）で、counter の上位 12 byte は変えない。
7. 定数時間: counter（IV 由来で public）の下位 32 bit に依存する loop の条件を compiler が作った（counter を induction variable にする）ため、ct の試験では counter を秘密に印さず public とした（GCM でも counter block は IV から作られる公開値）。鍵・平文は秘密として印してある。header の「Constant time」は counter を秘密とは言っていないので矛盾しない。

header・phase.md と仕様の食い違い: 見つからなかった。phase.md の「Appendix C の試験の値」は、この版の FIPS 197 には本体が無く（NIST の web への参照だけ）、代わりに CAVP の KAT を使った（phase.md の試験の data の欄どおり）。

未実施: 性能の測定（4 block の bitslice の速さは未測定。S-box は 1 回に乗算 4 回の回路で、表引きより遅い見込み）、aarch64 Windows の実行（build のみ、check-skeleton）、実機・QEMU（範囲外）。

commit: 下の ws.md の表と同じ commit。
