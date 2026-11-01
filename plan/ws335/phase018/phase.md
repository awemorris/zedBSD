<!-- awesome-plan project=zedbsd record=ws335-p018 -->

# ws335-p018: 署名の確かめ（algorithm ごと）

Status: 実装済み・試験 PASS（2026-11-02 Sonnet q1902-i01。cleared は Q1。依存: p017、p011、p012）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-sigver.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc5480.txt`（SHA-256 `593bf29fd0da2ff8b903c3ebf1c9d189a770039159e2ba46a0c3b91355037f26`）: 2.2。
  - `rfc3279.txt`（SHA-256 `6d3f19f18e17fa1c68da5aaf4021327748fabca840d7300443b77357a1fc1614`）: 2.2.3。
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 4.2.3。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: wycheproof-ecdsa_*（DER の署名を cryc_ecdsa_signature_decode 経由で）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-sigver.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-sigver.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- ca-certificates の自己署名の root の署名を確かめる（SHA-1 の物は ENOTSUP を数える）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh sigver     # この module
sh plan/ws335/tests/run-unit.sh --win32 sigver     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 04:21 JST）Sonnet 5.5（q1902-i01）。worktree s-ws335-p018（main 05982d71c）。

作った物: `userland/base/libcrypto-compat/cryc-sigver.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-sigver.c`。header・試験の道具・他の module・Makefile は変えていない。ct の試験は作っていない（公開の値だけを扱う）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`。最後の変更（`rsa_alg` の代入を if に直した体裁だけの直し）の後に流し直した）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh sigver` | `test-sigver: PASS (492859 checks)` / `run-unit: PASS`（ASan・UBSan、約 3 分 20 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 sigver`（x86_64、Wine） | `test-sigver: PASS (492859 checks) (win32)` / `run-unit: PASS`（約 1 分） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `sh plan/ws335/tests/run-unit.sh all` | 全 module PASS（test-sigver 492859 checks を含む）、最後の行 `run-unit: PASS`（約 15 分。`rsa_alg` の体裁の直しの前の実行。直しの後は sigver の posix・win32・skeleton を流し直し PASS） |
| `git diff --check` | 出力なし |

実装の中身（`cryc-sigver.c`）:

- `cryc_ecdsa_signature_decode`: `cryc_der` の厳密な reader で外側の SEQUENCE（入力の全部）、中の INTEGER 2 つ（中身の全部）を読む。INTEGER は `cryc_der_integer`（負・非最小を拒む）で大きさを取り、0（1 byte の 0x00）と size byte を超える値を拒む。r・s は 2 つとも受理した後でだけ size byte に右詰めで書く。どの誤りも EBADMSG。
- `cryc_signature_verify`: 引数 → 未設定 → 範囲外 → key と algorithm の種類 → hash → 種類ごとの確かめ、の順。message を algorithm の hash で hash し、RSA は `cryc_rsa_pkcs1_verify`・`cryc_rsa_pss_verify`（salt は params->salt_size）、ECDSA は曲線を `cryc_ecp_init` し、点を `cryc_ecp_decode` で確かめてから DER を decode し `cryc_ecdsa_verify`（hash の長さと曲線は結び付けない。左端の bit は primitive が取る）。RSA の誤り（EBADMSG・EINVAL・ENOTSUP）はそのまま返す。

試験の中身（492859 checks）:

- 復号の独立の reader（`ref_*`、X.690 の 8.1・8.3・10 から試験の側に書いた）と照合: 幅 1・2・20・32・48・64・66・70 の値（全部 1・最上位 bit 付き（sign octet）・最上位 bit 無し・乱数・先頭 0 の乱数）の 6 形 × field の幅 8 通り（狭い・同じ・広い）を derwriter で組み、全ての真の接頭辞と末尾 1 byte の追加が EBADMSG、全 byte の 1 つの変更が reader と一致、手で作った誤り 25 種（tag 違い・1 個・3 個の INTEGER・負・非最小（r・s）・0（r・s・両方）・空・長さの長形式・INTEGER の長形式・不定長・長さの過・不足・INTEGER の長さの超過・高い tag・後ろの byte・幅の超過と受理の境界）、乱数の 3 か所までの変更 4 万通り、乱数の byte 列 2 万通り（長さ 0〜80）が全部 reader と一致（件数 121803）。誤りの時は r・s の buffer が触られないこと（sentinel）も確認。size 0・NULL の入力は EBADMSG。幅 66（P-521 の幅）で長形式の SEQUENCE が通ること。
- 公式の vector: Wycheproof ECDSA 4 file（P-256 sha256・sha512、P-384 sha384・sha512）の全 2084 件（valid 842 は 0、invalid は EBADMSG。壊れた DER・広い整数を含む。空の message は NULL でも同じ）、CAVP ECDSA SigVer の P-256・P-384 × SHA-256・384・512 の 6 通り全部（valid 18、F 72。F は key が曲線の外なら EINVAL、他は EBADMSG）、Wycheproof RSA PKCS#1（5 file）と PSS（3 file）の全 1689 件（valid 297）、CAVP RSA SigVer15・SigVerPSS の SHA-256・384・512 の 324 件（valid 36、2048 bit 未満の 108 件は ENOTSUP。SigVerPSS は SHA-384 の salt の長さ違いも含む）。
- valid な署名 1 つ（RSA PKCS#1 の 2048 bit sha256・4096 bit sha512、PSS 2048 sha256、P-256 sha256、P-384 sha384）を fixture にして 1 つずつ変える: message の 1 bit（先頭・中・最後 × 8 bit）・長さ ±1・空、signature の長さの全部（0 から size-1、+1）・全 byte の 1 bit・NULL、他の hash の algorithm（EBADMSG）、他の方式、RSA の key（n の 1 bit・偶数 EINVAL・e = 0・1・2・3・4・無し・5 byte・n または e が NULL・n の 1024 / 2047 / 8193 bit は ENOTSUP・8192 bit は範囲内）、PSS の salt の長さ（±1・0・1・n の長さ・SIZE_MAX・SIZE_MAX-33。PKCS#1 は salt_size を使わない）、EC の key（点の長さの全部 0〜total+1、形の byte 0〜255 の全部、圧縮形、x・y の 1 bit（曲線の外）、座標 = p・全部 1、他の曲線の点、曲線 none・3・99・-1、点が NULL、RSA の field は無視）、key の誤りは signature の誤りより先、RSA の algorithm と EC の key（RSA の key も持たせて種類で決まること）・ECDSA の algorithm と RSA の key（EC の点も持たせて）は EINVAL。
- 引数: key・params NULL、message・signature の NULL（size 0 は message は空・signature は EBADMSG、size 付きは EINVAL）、algorithm none（EINVAL）・unsupported・11・99・-1（ENOTSUP）、key none（EINVAL）・unsupported・4・100（ENOTSUP）、none と unsupported が重なる時の順、種類違いの組、結果が 0 になる 9 つの algorithm。
- 乱数: RSA の乱数の key（200〜1024 byte、偶数・奇数）・指数・署名・algorithm 300 組、EC の有効な key に乱数の署名 600 組と乱数の点 200 組（0 を返すのは 0 件、全部が 0・EBADMSG・EINVAL・ENOTSUP のどれか）。
- 実在の証明書: ca-certificates の bundle（`WS335_CACERT`、既定は cacert-2026-08-13.pem）の 121 枚の root の自己署名（117 枚が 0（RSA PKCS#1 77、ECDSA 40）、4 枚が ENOTSUP（SHA-1 の署名 3、P-521 など未対応の key 1））、その tbs と署名の 1 bit の変更を各 root で 6 通り（702 通りが全部 EBADMSG）、別の root の key での確かめ。x509-limbo の 9,792 件（SUCCESS 930 件の全証明書と、他の 8 件に 1 件）で、issuer の Name と byte が同じ subject を持つ証明書の組（8670 組、重複を除いて 6368 組を確かめた）の結果が 4 つの値のどれかであること、SUCCESS 929 件（peer に issuer 候補がある物）のうち 926 件は peer の署名が候補のどれかで 0、3 件は未対応の algorithm（SHA-1 など）の ENOTSUP。

わざと壊す確かめ（git の外。`scratchpad/p018-mut/mutate.py`、.c を `p018-mut/mNNN/` に複写して置換し、sanitizer 無しの -O2 で test-sigver を build して流す。実装 file は触らず、最後に sha256 が変わっていないことを script が確認。CPU 400 秒・書き込み 64 MB・RSS 6 GB・600 秒の上限、4 並列）。判定は build の誤り・試験の失敗・crash・見逃しを区別した。70 通り: 58 通りが試験の失敗（`test-sigver: FAIL (...)`）、5 通りが crash（試験が FAIL を出した後、または NULL の確かめの脱落で NULL を使って segfault）、build の誤りは 0、見逃し 7 通りで全部等価か冗長:

- 試験の失敗 58: decode（外側・中の長さの確かめ、tag、0・幅・右詰め・padding・r と s の取り違え・誤りの前に書く・DER reader を通さない）、verify（引数の NULL、alg・key の none、範囲の境 4、未知の key の種類、種類の分け方 2、種類違いの確かめ 2、空の signature の代替、hash の大きさ・種類、結果の無視 2・誤りの値の置き換え 2）、rsa（PSS の振り分け 3、salt 4、digest の大きさ、signature の長さ、hash の固定 2）、ec（key の先の確かめ、誤りの値、decode の幅、digest の切り方 3、r と s の入れ替え、結果の無視、曲線の固定）。
- crash 5: decode の幅の確かめの脱落 2（r・s の buffer を越えて書く）、params・key の NULL の確かめの脱落 2、EC の点の size を固定。
- 見逃し 7（観測できる違いが無い）: (1) algorithm unsupported の確かめの脱落: 後の範囲の確かめが同じ ENOTSUP（冗長）。(2) key unsupported の確かめの脱落: 後の種類の確かめが同じ ENOTSUP（冗長）。(3) 範囲の下限 -1: 下限未満の none と unsupported は先に返る（等価）。(4) digest size 0 の確かめの脱落、(5) hash の誤りの無視: 3 つの hash は常に使える（到達しない防御）。(6) EC の曲線の確かめの脱落: `cryc_ecp_init` が同じ誤りを返し EINVAL に写す（冗長）。(7) `cryc_ecp_init` の誤りの値を通す: (6) の確かめがあるので到達しない（等価）。
- 最初の実行では 037・038（種類違い）が見逃しだった。EC の key が RSA の key も持たない形だったため、確かめを外しても primitive が EINVAL を返した。RSA の key を持つ EC の key、EC の点を持つ RSA の key を試験に足して塞いだ。

自分で決めた点（header が黙っていた所）:

1. `cryc_ecdsa_signature_decode` の size に上限を置かない（P-521 の 66 でも読める）。size 0 は EBADMSG。誤りの時は r・s を書かない（2 つとも受理した後で書く）。0 の r・s は EBADMSG（FIPS 186-5 6.4.2 step 1）。DER の誤りも幅の超過も EBADMSG にまとめた。
2. `cryc_signature_verify` の検査の順: key・params の NULL（EINVAL）、message・signature が NULL で size 付き（EINVAL）、algorithm none と key none（EINVAL）、algorithm・key が unsupported か列挙値でない値（ENOTSUP）、algorithm と key の種類の不一致（EINVAL）。header は none と列挙値でない値を書いていない。none は呼び出し側の誤りとして EINVAL、列挙値でない値は unsupported と同じ ENOTSUP にした。
3. message が NULL で size 0 は空の message。signature が NULL で size 0 は長さ 0 の署名（RSA も ECDSA も EBADMSG。RSA の primitive は NULL を EINVAL にするので、1 byte の代替を渡して key の確かめの後に EBADMSG にした）。
4. EC の key: curve が P-256・P-384 のどちらでもなければ EINVAL（ENOTSUP でなく。X.509 の parse が他の曲線を CRYC_KEY_UNSUPPORTED にするので、type EC でこれらの曲線以外は不正な構造）。点の誤りは全部 EINVAL（圧縮形も。`cryc_ecp_decode` は圧縮形に ENOTSUP を返すが、`struct cryc_pubkey` の point は uncompressed と書いてあるので不正な key）。key の確かめは signature の decode より先（key の誤りが署名の誤りより先に返る）。点を 2 回 decode する（key の確かめと `cryc_ecdsa_verify`）。費用は積の計算に比べて小さい。
5. `struct cryc_ecp`（約 4 KiB）は `sigver_verify_ecdsa` の stack に置く（呼び出しごとに `cryc_ecp_init`）。
6. RSA は primitive の誤りをそのまま返す（key の大きさの ENOTSUP・指数の EINVAL・署名の EBADMSG）。PKCS#1 v1.5 の algorithm は salt_size を使わない。
7. static の補助は `sigver_signature_integer`、`sigver_verify_rsa`、`sigver_verify_ecdsa`。

header の comment への持ち越し（p035 向け）: (a) `cryc_signature_verify` の誤りの値に、algorithm none・key none（EINVAL）と列挙値でない値（ENOTSUP）、NULL の扱いがない。(b) EC の点が圧縮形の時は EINVAL（`cryc_ecp_decode` の ENOTSUP とは違う）。(c) `cryc_ecdsa_signature_decode` が誤りの時に r・s を書かないこと、size に上限が無いこと、0 の値の拒否が書かれていない（header は「negative or zero」とだけ書いてある）。header と phase.md の食い違いは見つからなかった。

未実施: `cryc_signature_verify` の ct の試験（公開の値だけ）、性能の測定、aarch64 Windows の実行（build のみ）、32 bit の size_t の機械、RSA-PSS の SHA-384 の Wycheproof（file が無く、CAVP の SigVerPSS で確認）、x509-limbo の FAILURE 事例の全件の署名の確かめ（8 件に 1 件に絞った。時間の上限のため）。

commit: 1568f960ad（実装と試験）、最終版は下の commit。
