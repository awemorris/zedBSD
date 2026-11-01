<!-- awesome-plan project=zedbsd record=ws335-p019 -->

# ws335-p019: 名前（Name の比較・host と address の照合・name constraints）

Status: cleared（2026-11-02 06:09 Q1、main で test-name 255567・test-x509 PASS。dNSName の制約の先頭 dot は header に従い p020 で見直す）。前: 実装済み・試験 PASS（2026-11-02 Sonnet 5.5 high。cleared は Q1）。2026-11-01 p001 が作成。依存: p017
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-name.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc5280.txt`（SHA-256 `a2f2628c0a83b873fc4786abd921f9b2c02395954b655d190bf16b831633345d`）: 4.2.1.6、4.2.1.10、6.1.3 (b)(c)、6.1.4 (g)、7.1。
  - `rfc9525.txt`（SHA-256 `ed8116c2435376dffe37ec2ca688c3a8515e36820852b29e0b6cdfbe92abd2c5`）: 6（6.1〜6.4）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: limbo（名前の件、features と id で選ぶ）。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-name.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-name.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- wildcard は左端の label の全体だけ。CN を見ない。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh name     # この module
sh plan/ws335/tests/run-unit.sh --win32 name     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 05:25 JST）Sonnet 5.5 high（q1903-i01）。worktree s-ws335-p019（main 05982d71c）。commit は 3bdd299b65（実装・試験）、88bb76f6a3（規約の見直し）、bec5fabb0d（試験の追加）と、この phase.md・ws.md の commit。

作った物: `userland/base/libcrypto-compat/cryc-name.c`、`plan/ws335/tests/unit/test-name.c`。header（cryc-name.h）・試験の道具・他の module は変えていない。変えた物は次の Q1 の承認済みの 1 点だけ（下）。

**Q1 の承認済みの変更（2026-11-02 Q1 の判断）: critical でない nameConstraints を受理して処理する**（RFC 5280 4.2.1.10 は CA に critical を求めるが、読む側が拒むと制約の付いた root を読めず、制約は処理すれば守れる）。変えた所は 3 つだけ:

- `cryc-x509.h` の `cryc_x509_parse` の comment の「nameConstraints (4.2.1.10; must be critical, ...)」を「critical or not」に（1 語。`check-skeleton.sh` の BASE の照合に差分として出る。承認済みの差分はこれだけ）。
- `cryc-x509.c` の `x509_apply_extension`: critical でなければ EINVAL とする検査を外し、理由の comment を直した。
- `test-x509.c`: nameConstraints を critical でなくても受理して `HAS_NAME_CONSTRAINTS` と view が入る検査に直し、limbo の固定の拒否 id の一覧から 3 件（`rfc5280::nc::permitted-dns-match-noncritical`、`rfc5280::nc::not-allowed-in-ee-noncritical`、`webpki::nc::permitted-dns-match-noncritical`）と、SUCCESS の例外（`LIMBO_SUCCESS_REFUSED`）を外した。**limbo の再計数: 拒否は 16 枚から 13 枚になり、SUCCESS の case の証明書の拒否は 1 枚から 0 枚になった**（`test-x509: PASS (159977 checks)`）。
- p020 への申し送り: `rfc5280::nc::permitted-dns-match-noncritical`（expected FAILURE、features に `rfc5280-incompatible-with-webpki`）と `rfc5280::nc::not-allowed-in-ee-noncritical`（EE の nameConstraints、expected FAILURE）は、今は parse でき、名前の確かめでも許可になる（`limbo_nc_exceptions` に固定）。強制しない。path の検証（p020）で、「CA でない証明書の nameConstraints」「critical でない nameConstraints」を FAILURE とするか決める（`rfc5280::nc::not-allowed-in-ee-critical` も同じ。EE の nameConstraints は 3 件とも p020）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`、各 command に `ulimit -f` と `timeout` を付けた）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh name`（ASan・UBSan） | `test-name: PASS (255567 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 name`（x86_64、Wine） | `test-name: PASS (255567 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh x509` と `--win32 x509` | `test-x509: PASS (159977 checks)`、`test-x509: PASS (159977 checks) (win32)` |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（test 26 本と ct- 15 本の全部 PASS。test-name 255567、test-x509 159977 checks） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: FAIL`: 理由は `cryc-x509.h | 2 +-` の 1 行だけ（上の承認済みの変更）。BASE の照合の外の項目は、BASE を付けずに流した `sh plan/ws335/tests/check-skeleton.sh` が `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲、Windows の static link） |
| `git diff --check` | 出力なし |

試験の中身（test-name.c。期待の値は RFC 5280・RFC 9525 と手の計算だけから作り、無作為の比較は本文に書いた小さな model（label の配列、文字列の整形）で行う）:

- `cryc_name_equal`: 同じ byte、空の Name、string 型の組（Printable と UTF8 は互いに折りたたみ、IA5・T61・BMP は同じ型の byte 一致のみ）、大文字小文字、空白（前後の除去、内部の run、全部空白と空）、NUL・0x7f・0x80 の境、非 ASCII は byte 一致、type の OID（prefix の別 OID）、RDN の数と順序、多値の RDN（順序が違えば別）、壊れた物（prefix 全部、余りの byte、RDN が空・SET でない、属性が 1 つ・3 つ、OID が壊れ）は同一でも 0。model との 30000 組の無作為の比較（反射・対称も）、20000 本の 1 byte 変異、20000 本の無作為。入力は exact-size の block で渡し、1 byte の読み過ぎも ASan で検出する。
- `cryc_name_host_normalize`: 大文字・末尾の dot・label 63/64・名前 253/254（dot の有無）・空の label・文字の範囲の端（a z A Z 0 9 - _ とその隣の文字）・NULL・出力の canary・冪等、40000 本の無作為（model）。
- `cryc_name_match_host`: 完全一致、大文字小文字、wildcard（左端の label の全体、1 label だけ、3 label 未満は不一致、label の数・残りの label の長さ、`*` の位置の誤り各種）、空の label・先頭と末尾の dot、非 ASCII・NUL、253 の端、CN を使わない、SAN の flag なし、reference の正規化、壊れた SAN（途中までの全 prefix、一致の後に壊れた物があれば不一致）、60000 本の無作為（model）。
- `cryc_name_match_ip`: 4 と 16 octet の完全一致（全 bit）、family の混在、8・32 octet の entry、size 0・1・8・15・17・18、複数 entry、壊れた SAN。
- `cryc_name_constraints_check`: dNSName（host・先頭 dot・空・大文字小文字・label の境・excluded・両方・複数 subtree と複数名・wildcard の許可と excluded の重なり・malformed の名前と制約・253 と 254 の端・octet の範囲の端）、iPAddress（/24・/25・/32・/0・IPv6・CIDR でない mask 各種・family の混在・名前の size）、rfc822Name（host・domain・mailbox・excluded・subject の emailAddress（SAN が無い時だけ、型の扱い）・malformed）、directoryName（prefix・順序・折りたたみ・空の制約・空の subject・SAN の directoryName・多値の RDN・壊れた Name 各種）、評価しない形（URI・otherName・x400・ediPartyName・registeredID: 制約と名前がそろえば ENOTSUP、そうでなければ無視）、GeneralName の tag の誤り、前提（flag なし・NULL・view の size だけ）、budget の端（4095 は許可、4096 の 1 つ超えで ENOTSUP、excluded と permitted が共有、他の形の subtree は数えない、excluded だけで尽きる、2049×2049 の病的な chain）。全ての cert の view は exact-size の block。
- x509-limbo（SERVER の 9792 case）: peer の名前を peer の証明書と照合（SUCCESS 930 のうち名前のある 929 が全部一致、FAILURE のうち 2266 も一致する。理由が名前でない case）、名前の理由の case 約 30 件は id ごとに期待の一致・不一致を固定、peer から trusted まで一意に組める chain（9792 のうち 99 は選択があり除く）の全ての nameConstraints を適用（self-issued の途中の証明書は除く）し、SUCCESS の chain 880 は全部許可、nameConstraints の case 9538（bettertls 9491 件を含む）のうち 9534 が expected と一致（許可かつ名前が合う = SUCCESS）、4 件は expected と違うが理由があって固定（下）。制約が拒否した case 5863、名前だけで不一致 2840。

limbo で expected と違う 4 件（`limbo_nc_exceptions`。強制しない）:

| id | expected | この module | 理由 |
| --- | --- | --- | --- |
| rfc5280::nc::permitted-dns-match-noncritical | FAILURE | 許可 | critical でない nameConstraints（承認済みの変更。path の検証は p020） |
| rfc5280::nc::not-allowed-in-ee-noncritical | FAILURE | 許可（EE の nameConstraints は上の CA の制約でなく、EE 自身のもの） | CA でない証明書の nameConstraints（p020） |
| rfc5280::nc::not-allowed-in-ee-critical | FAILURE | 同上 | 同上（p020） |
| rfc5280::nc::invalid-dnsname-leading-period | FAILURE | 許可 | header は dNSName の制約の先頭の dot を「その下の名前だけ」と定める。limbo は RFC 5280 4.2.1.10 の dNSName に先頭 dot は無いとして FAILURE。header に従った |

名前の照合で limbo と違う物（header に従った。理由は照合の外）: `webpki::san::public-suffix-multi-label-wildcard-san`（`*.co.uk`）と `...private-namespace-wildcard-san`（`*.s3.amazonaws.com`）は一致する（header: public suffix の表を見ない。`*.com` は 2 label で不一致）、`rfc5280::san::underscore-dns` は一致する（header: reference の `_` を受け付ける）、`webpki::cn::*` の 9 件・`webpki::san::san-critical-with-nonempty-subject`・`rfc5280::san::noncritical-with-empty-subject` は名前は一致し、FAILURE の理由は CN・SAN の critical の規則（p020 か p038）。

わざと壊す確かめ（git の外。`/tmp/claude-1000/-home-awe-zedBSD-claude1/b57eaf0e-d1bd-5084-9fc6-6409b527934c/scratchpad/p019/mutate.py` が cryc-name.c の複写を機械的に変える: 演算子（`==`/`!=`、`<`/`<=`、`&&`/`||`、`+`/`-`、`&`/`|`/`^`、`<<`/`>>`、`!` の削除）、数値の ±1、return の値、if・while の条件を 0/1 に、1 行の文の削除、`++`/`--`。複写だけを build し、他の module と試験の object は共通。1 本ごとに `RLIMIT_FSIZE` 64 MB、`RLIMIT_CPU` 400 s、wall 12〜20 s（無限 loop 用。上限 600 s より短い）、RSS が 6 GB を越えれば kill、同時 3 本。実装 file は変えない）:

- 1633 通り。1 回目の結果（試験が今より少し少なかった時）: 試験の失敗 681、sanitizer の異常終了 72、timeout 683、見逃し 193、build の誤り 4。timeout の 683 通りは、出力が多すぎて pipe が詰まる script の不具合を含んでいた（FAIL の行が大量に出る mutant）。script を出力を file に書く形に直して 683 通りを流し直した結果は、試験の失敗 633、sanitizer 28、timeout 22（本物の無限 loop）、見逃し 0。
- 見逃し 193 通りと build の誤り 4 通りを、試験の追加（下）の後に流し直した: 34 通りが試験の失敗か sanitizer で検出され、**159 通りが残った**。159 通りは全部、観測できる違いが無い物として個別に確認した: 使われていない定数の値（URI・EDI・x400・registeredID・otherName の形の番号、それぞれ参照されない）、真偽として使う値の 1→2、上の検査が先に拒む二重の検査（到達しない `cryc_der_enter` や `expect` の誤り、内側の検査が次の段で同じ結果になる連鎖、`name_ends_with_nocase` の長さの検査、wildcard の検査が後の label の比較と重複する物、`>`/`>=` が等しい長さの場合に起きない物）、初期化が後で上書きされる物（forms の 0、matched の初期値）、結果を使わない引数（wanted_size・reference_size）、`count++` が `== 0` の判定にしか使われない物。
- build の誤り 4 通り（`dot - constraint` を `dot + constraint` にした物 2 通りと、定数の番号の変更が switch の case を重複させた物 2 通り）は、同じ意味の壊し方を別の形に直して 7 通り流した（引数の入れ替え、番号の入れ替え、番号を別の値に）: 全て検出（試験の失敗 4、timeout 3）。
- 1 回目に見逃して試験を足した所: 数字の 9、先頭の byte だけが不正な名前、NUL・0x80 を含む値、OID が別の OID の prefix、先頭の mask の byte が不正、wildcard の `*x`・`a.*.x`、残りの label が長い reference、mail の host の longer 形（`example.com.evil.org`）、size 1 の制約の `*`、他の形の subtree が budget に入らない物、excluded の budget 切れ、SAN の flag なしで bytes のある cert、8 octet の address、subject の size 0。exact-size の block は、1 byte の読み過ぎ（`<` と `<=` など）を検出するために入れた。試験は足しただけで外していないので、1 回目に検出された mutant は今の試験でも検出される（全部を今の試験で流し直してはいない）。

自分で決めた点（header が黙っていた所）:

1. `cryc_name_equal`: 先に両方の構造を確かめ、壊れていれば byte 一致でも 0。値は PrintableString と UTF8String の ASCII だけを折りたたみ、全部空白と空の string は等しい（RFC 4518 の空白の規則の簡略）。それ以外（非 ASCII・他の string 型・tag の違い）は encoding 全体の byte 一致（tag も含む）。
2. `cryc_name_host_normalize`: 失敗時は `out` を空の文字列にする。長さを調べる読みは 255 文字で止める。`host` か `out` が NULL なら EINVAL。
3. `cryc_name_match_host`: reference をもう一度 normalize する（normalize 済みでない名前も比較でき、normalize できない名前は何にも一致しない）。dNSName の entry は全部たどり、どこかで SAN が壊れていれば一致があっても 0。presented の identifier は、空・253 を越える・空の label（先頭・末尾の dot を含む）・`*` が左端の label の全体でない・複数、のとき無効で無視。wildcard は 3 label 以上、reference の label 数が同じ、残りの label が等しい。
4. `cryc_name_match_ip`: size が 4 か 16 でなければ 0。entry の長さが違えば不一致。
5. `cryc_name_constraints_check`: `ca` が flag を持たない・NULL・view が NULL で size ありなら EINVAL。両方の list を最初に全部確かめる（制約の誤りは名前が何であれ EINVAL）。名前は、その形の制約がある時だけ確かめる（制約の無い形の壊れた名前は見ない）。ただし SAN の GeneralName の tag の誤りは常に EINVAL。dNSName は印字できる ASCII（0x21〜0x7e）、label 1〜63、全体 253 以下、名前の `*` は左端の label の全体だけ、制約は `*` なし・先頭の dot は 1 つまで・空は全ての名前（制約の末尾の dot は EINVAL）。wildcard の dNSName は、permitted では書いてある通りに比べ（`*.x` は `x` が入っていれば入る）、excluded では書いてある通りに加え「`x` の 1 label 下の制約にも当たる」とした（limbo の cve-2025-61727 の 2 件に合わせる）。rfc822Name は mailbox（最後の `@` で分け、local part は大文字小文字を区別、host は区別しない）・host・先頭 dot の domain、空の制約は EINVAL。subject の emailAddress は、SAN が無い時だけ rfc822Name として扱う（RFC 5280 4.2.1.10。型は IA5・UTF8・Printable のみ、それ以外は EINVAL）。iPAddress は 8 か 32 octet、mask は 1 の後に 0（CIDR）でなければ EINVAL、address の mask 外の bit は見ない、family が違えば入らない（ただし「その形の permitted がある」は family を区別しない: header の通り）。directoryName は最初の RDN が一致（RDN の比較は cryc_name_equal と同じ）、空の subject は制約しない、SAN の空の directoryName は 0 個の RDN として扱う（空の制約にだけ入る）。評価しない形（otherName・x400Address・ediPartyName・URI・registeredID）は、制約と名前が両方あれば ENOTSUP、片方だけなら無視。`is_leaf` は使わない（UNUSED_PARAMETER。header の通り、同じ検査）。budget は、形が同じ subtree との比較 1 回ごとに 1 つ使い、excluded と permitted で共有、先に尽きれば ENOTSUP。subtree の minimum と maximum は見ない（parse が確かめる）。
6. 内部: static の補助の分け方、`struct name_limits`・`struct name_folder`、GeneralName の形ごとの表（構築の bit は RFC 5280 A.1）、emailAddress の OID の表。

header の comment で気づいた点（直すかは p035）: `cryc_name_constraints_check` の comment が dNSName の先頭の dot を許すのは、limbo（RFC 5280 4.2.1.10 の dNSName に先頭の dot は無い）と合わない（上の表）。header の「`is_leaf` の違い」は何も起きない。

未実施: run-ct（秘密を扱わない）、32 bit の size_t の機械、aarch64 Windows の実行（build のみ）、性能の測定、実機・QEMU（範囲外）、1633 通り全部を最後の試験で流し直すこと（1 回目の検出は今の試験でも成り立つ）。
