<!-- awesome-plan project=zedbsd record=ws335-p012 -->

# ws335-p012: RSA の署名の確かめ（PKCS#1 v1.5・PSS）

Status: cleared（2026-11-02 01:07 Q1、main 9aa6bae4d。test-rsa 31057 PASS）。前: 実装済み・試験 PASS（2026-11-02 00:50 P6。cleared は Q1。依存: p007、p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-rsa.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8017.txt`（SHA-256 `1e72dc473d18df3fc5598cdc12795a9f18f36f1aef15abc23a55eb0d58151d11`）: 4.1・4.2、5.2.2、8.1.2、8.2.2、9.1.2、9.2（と注 1 の DigestInfo）、B.2.1。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: cavp-186-3rsatestvectors-SigVer15_186-3・SigVerPSS_186-3（2048・3072、SHA-256/384/512）、wycheproof-rsa_signature_*・rsa_pss_*。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-rsa.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-rsa.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- PKCS#1 v1.5 は期待の EM を作って全体を比べる（parse しない）。DigestInfo の接頭辞は data として出典。
- 公開の値だけなので定数時間は要らない。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh rsa     # この module
sh plan/ws335/tests/run-unit.sh --win32 rsa     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 00:50 JST）P6（Sonnet 5.5）。worktree p6-ws335b（main 160442a06）。

作った物: `userland/base/libcrypto-compat/cryc-rsa.c`、`plan/ws335/tests/unit/test-rsa.c`。header・試験の道具・他の module は変えていない（ct の試験は作っていない: 公開の値だけを扱う）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh rsa` | `test-rsa: PASS (31057 checks)` / `run-unit: PASS`（ASan・UBSan、約 2 分） |
| `sh plan/ws335/tests/run-unit.sh --win32 rsa`（x86_64、Wine） | `test-rsa: PASS (31057 checks) (win32)` / `run-unit: PASS`（約 40 秒） |
| `sh plan/ws335/tests/run-unit.sh all` | 全 24 本 PASS（test-rsa 31057 checks を含む。最後の行 `run-unit: PASS`、約 7.5 分。cryc-rsa.c の comment と if の分割だけを直す前の実行。直した後は rsa のみ posix・win32 を流し直し PASS） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |

実装の中身（`cryc-rsa.c`、RFC 8017）:

- 共通の static 関数 `rsa_public_operation` が引数・key・hash の確かめと RSAVP1（5.2.2: s < n の確かめ、`cryc_bn_mont_init`・`to_mont`・`mont_exp`・`from_mont` で m = s^e mod n、`cryc_bn_encode` で EM = I2OSP(m, k)）を行う。n の bit 長は先頭 byte から数える。
- PKCS1 v1.5（8.2.2、9.2）: DigestInfo の接頭辞 19 byte（NULL 付きの形だけ。SHA-256・384・512 の 3 つ）を表（9.2 note 1 を出典の comment、block ごと）で持ち、期待の EM = 00 01 FF… 00 T を作って全体を memcmp（parse しない）。
- PSS（8.1.2、9.1.2、B.2.1）: emBits = modBits - 1、emLen = ceil(emBits/8)。emLen が k - 1 の時は m の先頭 byte が 0 であることを確かめて EM を 1 byte ずらす。9.1.2 の step 3〜14 を順に（salt_size の上限を先に見て和の桁あふれを避ける、末尾 0xbc、上位の未使用 bit、MGF1（同じ hash、counter 4 byte big-endian）、DB の padding と 0x01、H' の比較）。

試験の中身（31057 checks）:

- 公式の vector: CAVP SigVer15（PKCS1 v1.5）162 件（SHA-256・384・512 の 2048・3072 bit の 54 件ずつ相当、うち valid 18 件。1024 bit の 108 件は PKCS1 と PSS で ENOTSUP と確認）、CAVP SigVerPSS 162 件（valid 18 件）、Wycheproof の rsa_signature 5 本と rsa_pss 3 本の計 1689 件の全部（valid 297 は 0、invalid は EBADMSG、acceptable 5 件は 0 か EBADMSG で、MissingNull の 5 件は header どおり EBADMSG）。CAVP の F は e が使えない値なら EINVAL、他は EBADMSG。
- 試験の側の署名器（RFC 8017 の EMSA-PKCS1-v1_5-ENCODE（DigestInfo は derwriter.h で OID から組む）、EMSA-PSS-ENCODE、MGF1、RSASP1）: CAVP の PSS の valid 18 件（2048・3072 bit、SHA-256・384・512）を、その key の d と SaltVal で署名して NIST の S と byte 単位で一致することを確認（署名器の検証）。
- key: CAVP の 2048・3072 bit の key（d 付き）と、Mersenne 素数 2^a-1 の積で作った modulus（bit 長 2057・2074・2067・2292・2077・2054・2095・2120 = modBits mod 8 が 1〜7 と 0、と 8192 bit の上限）。d は phi から小さい割り算で求め、不正なら往復が落ちる。
- 往復: PKCS1・PSS で hash 3 種、salt の大きさ（hash 長・最大・0・1）、正しい署名・別の hash・署名の 1 bit・別の方式・s + n（s ≥ n）・長さ 0〜k+8 の全部・s = 0・1・n-1・n・n+1・全部 1・乱数の署名。先頭 byte が 0 の署名を k-1 byte・k+1 byte で送ると EBADMSG。
- 偽造（わざと誤った EM に署名）: PKCS1 は構造の各 byte（先頭 4 byte、PS の最後〜DigestInfo 全部〜hash の先頭、末尾、hash の中ほど）の 1 bit、block type 00・02、NULL 無しの DigestInfo、別の hash の OID、DigestInfo の後ろの garbage と PS 1 byte 減、長形式の長さ（DER でない）。PSS は masked DB の先頭・padding の前後・0x01・salt・H の先頭・中・末・trailer の bit 変更（0x80 の変更で上位の未使用 bit も）、別の message の hash、未使用 bit の設定、EM が 1 byte 短い key（modBits ≡ 1 mod 8）で m の先頭 byte が非 0、DB が全部 0（marker 無し）で salt の大きさ 9 通り。
- 引数: NULL の key・n・e・hash・signature（EINVAL）、hash_id（NONE・4・-1・7 は ENOTSUP）、hash_size の 0・31・33・48・他の hash（EINVAL）、検査の順（key → hash → 署名の長さ）、e の 0・1・2・4・65536 相当（EINVAL）、e = 3 と 0xffffffff（受理されて EBADMSG）、5 byte の e（低い 32 bit が有効でも EINVAL）、e と n の先頭の 0 byte（同じ意味で valid の署名が通る）、n の 0・偶数（短い物・長い物も EINVAL）、n の bit 長 1・1024・2040・2047（ENOTSUP）・2048・8191・8192（受理）・8193（ENOTSUP）、1025 byte。
- salt の大きさ: SIZE_MAX・SIZE_MAX-1・SIZE_MAX-33〜35・k・k+1・2^63 で EBADMSG（桁あふれ・範囲外の読みなし）。
- 乱数の key・hash・salt・署名 400 組（ASan・UBSan で落ちず、EBADMSG・EINVAL・ENOTSUP のどれか。EBADMSG が 20 件超）。

わざと壊す確かめ（git の外。`build/tmp/p012/brk.py`、.c を `build/tmp/p012/brk/m-N/` に複写して書き換え、sanitizer 無しの -O2 で test-rsa を build して流す。実装 file は触らず、最後に sha256 が変わっていないことを script が確認）。判定は build の誤り（BUILD-ERROR）、試験の失敗（`test-rsa: FAIL`）、crash（segfault）、見逃し（PASS）を区別した。97 通り: 84 通りが試験の失敗（`test-rsa: FAIL (...)`）、6 通りが crash（NULL の確かめの 5 つと、PSS の salt_size - 1 が salt 0 で桁あふれ）、build の誤りは 0、見逃し 7 通り:

- 試験の失敗 84: DigestInfo の表（各 hash の OID・長さ・NULL・長さの byte、SHA-256 に SHA-384 の接頭辞）、PKCS1 の EM の各 field（padding の長さ、先頭 byte・block type・PS の byte・separator・hash の長さ・比較の長さと向き）、key・引数（先頭 0 の除去 2、n 偶数、e の size 上限 2・値の下限 2・偶数・byte 順、bit 長の式・上限と下限の比較 4、max bytes の比較、hash の size の確かめ 2、署名の長さの確かめ 3、s < n の脱落と反転、指数の size・指数の取り違え、encode の size、from_mont の脱落）、PSS（emBits・emLen・EM の 1 byte のずれ 3、too-short の +3、trailer の値・位置・脱落、database の size・H の位置、未使用 bit の計算 2・脱落・反転・keep mask 2、xor を or に・最後の byte を飛ばす、MGF1 の seed 2・mask の size・counter の開始・byte 順・更新・block の切り詰め・seed と counter の size、padding の loop 3・size、marker の値・脱落、M' の 0 の数・hash の脱落・salt の位置・size・脱落、H の比較 4）。
- crash 6: key・n・e・hash・signature の NULL の確かめの脱落（NULL を使って segfault）、`salt_size - 1`。
- 見逃し 7（観測できる違いが無い物、または ASan でだけ見える物）: (1) n = 0 の確かめの脱落: 後の偶数の判定が n_bytes[-1] を読んで同じ EINVAL（ASan の build では範囲外の読みとして落ちることを `SAN=1` で確認）。(2) e の size 0 の確かめの脱落: 後の e < 3 の判定が同じ EINVAL（冗長）。(3) bit 長の上限 +1: 8193 bit は `cryc_bn_mont_init` が同じ ENOTSUP を返す（冗長）。(4) `n_size > RSA_MAX_BYTES` の脱落: `(n_size - 1) * 8` の桁あふれ防止だけで、実際の大きさでは bit 長の判定が同じ結果（桁あふれに届く大きさの buffer は作れない）。(5) `cryc_bn_mont_init` の誤りの無視: 先の確かめで誤りは起きない（到達しない）。(6)(7) `salt_size > message_size` の脱落と too-short の `+ 1`: 桁あふれ・負の長さを避ける防御。脱落させた版は padding の長さが桁あふれして範囲外の stack を読み始めるが、結果は読んだ byte が 0 でない限り同じ EBADMSG で、DB が全部 0 の EM（signature に署名して 9 通りの salt の大きさ）の試験を足しても、読む先が未初期化の stack のため決まった差にならない。

自分で決めた点（header が黙っていた所）:

1. 誤りの順: 引数が NULL（key・n・e・hash・signature。EINVAL）→ key（n が 0 か偶数、e が 3 以上 2^32-1 以下の奇数でない: EINVAL。次に n の bit 長が範囲の外: ENOTSUP）→ hash（hash_id が 3 つ以外: ENOTSUP、size が違う: EINVAL）→ 署名（長さが k でない、s ≥ n: EBADMSG）。偶数で短い（長い）n は EINVAL が先（cryc_bn_mont_init の前例と同じ）。
2. n と e の先頭の 0 byte は無視する（header は DER の sign byte を除くと書くだけ）。k は先頭の 0 を除いた byte 長で、署名の長さもそれに合わせる。
3. e の byte 数が 5 以上（先頭の 0 を除いて）は、低い 32 bit が有効でも EINVAL（上限 2^32 - 1）。
4. PKCS1 の署名の hash に NULL 引数（hash == NULL）は EINVAL とし、`signature == NULL` も EINVAL（長さ 0 でも）。
5. PSS: modBits - 1 が 8 の倍数で EM が k - 1 byte の時、m の先頭 byte が 0 でなければ EBADMSG（RFC 8017 8.1.2 step 2c の "integer too large"）。salt_size が EM の長さを越えたら EBADMSG（和の桁あふれの防御）。
6. stack: `rsa_public_operation` は mont（約 2 KiB）と 1 KiB の配列 4 つ、PKCS1 は 2 KiB、PSS は 1 KiB 超を持つ（合わせて約 10 KiB）。限られた stack の機械では未確認。

phase.md の指示との食い違い・header の不備: 見つからなかった。ただし CAVP の SigVerPSS では 3072 bit の SHA-1・224・256・512 の 72 件が `SaltVal = 00`（1 byte）で、salt の長さ 0 を表す（valid の case は salt 0 で verify でき、NIST の S を salt 0 の署名器で再現できる。salt 1 byte の 00 では verify できない）。試験はこの形を salt_size 0 と読む（3072 bit の SaltVal が 1 byte の 00 の時だけ）。

未実施: 性能の測定（8192 bit の署名は試験の側で約 10 秒）、CAVP の SigVer931（X9.31）・TruncatedSHAs（SHA-512/224・/256）・SHA-1・SHA-224（hash_id が無い）、SigGen の file、CAVP の SigVer15 の e が 2^32 を越える case（無かった）、Wycheproof の sha384 の PSS 等 vectors.py が変換しない file、2 素数の本物の RSA key（Mersenne 素数の積の multi-prime で代用。検証の式は同じ）、e = 3 での valid な署名の自作（Mersenne 素数 - 1 が 3 で割れるため d が無い。Wycheproof の e = 3 の group で valid・invalid は確認）、定数時間（公開の値だけ）、32 bit の size_t の機械・aarch64 Windows の実行（build のみ）、限られた stack での実行、実機・QEMU（範囲外）。

commit: 71495d412c（実装・試験・この結果の節。この行を足す commit が最後）
