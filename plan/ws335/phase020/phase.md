<!-- awesome-plan project=zedbsd record=ws335-p020 -->

# ws335-p020: trust store と path の検証

Status: cleared（2026-11-02 10:10 Q1）。前: 実装済み・試験 PASS（2026-11-02 Sonnet 5.5 q1912-i01。cleared は Q1。依存: p018、p019、p015）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-chain.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc5280.txt`（SHA-256 `a2f2628c0a83b873fc4786abd921f9b2c02395954b655d190bf16b831633345d`）: 6.1（6.1.1〜6.1.5）。
  - `rfc4158.txt`（SHA-256 `5d67d470a12edf63574c421e43d2d6b86f78e2eb6c79282acb73917f824fb3be`）: 2、3（path の組み立ての考え方、informative）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: limbo。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-chain.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-chain.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 仕事の上限（CRYC_CHAIN_MAX_WORK）と深さ。
- 信頼しない証明書（`cryc_store_add_distrusted`、[design.md](../design.md) D24、header の「Distrusted certificates」）: 印は DER 全体の SHA-256 だけ、重複は足さない、CRYC_STORE_MAX で ENOBUFS、size 0 は EINVAL。完全な path の確かめの最初に（署名の前）leaf から上へ印を探し、最初に見つかった（一番下の）深さで CRYC_VERIFY_REJECTED、その path は無効、探索は次の候補へ戻る（別の path、例えば cross-sign の別の中間で通れば OK）。仕事の数に入らない。どの path も通らない時の code の優先は header のとおり（最初の完全な path の code、REJECTED を含む → WORK_EXCEEDED → 自己署名の 2 つ → UNABLE_TO_GET_ISSUER）。
  試験: 印の付いた leaf・中間・anchor のそれぞれで REJECTED とその深さ、2 つに印がある path で一番下の深さ、印の付いた中間を避ける別の path で OK、印の付いた anchor が store に有っても他の anchor で通る件、印の後に同じ証明書を `cryc_store_add_der` で足しても REJECTED のまま、印の付いた中間だけの path と仕事の上限の組で REJECTED が WORK_EXCEEDED に勝つ件。
  cryc_store_add_system は試験の側の偽の cryc_os_system_roots を使わず実の物で: posix は `/etc/ssl/cert.pem`（無い host では ENOENT を無視して 0）、`--win32` では Wine の ROOT（host の CA、100 以上）が入り 0 を返す事だけ（件数を決め打ちにしない）。lock を持ったまま callback を呼ばない事（header）を、別の thread が同時に `cryc_store_add_der` する試験で確かめる（deadlock しない）。
- x509-limbo は id が `rfc5280::`・`pathlen::`・`webpki::`・`pathological::` の件（約 190 件）だけを流し、期待と一致した件数と、違った件の id と理由（policy の処理をしない、CN を見ない、trust anchor の制約を見ない等の決めた違い）を結果の節に表で。全件（9,792）の照合は p038。
- 自分の試験の chain は試験の側で作る: 鍵と証明書は derwriter.h と cryc の検証済みの部品（p011 の ECDSA の確かめに通る署名は p037 の署名器を待たず、固定の自己署名の root と中間を host の `openssl req`・`openssl x509` で作って試験の data として置く。期待の判定は RFC 5280 6.1 から手で決める）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh chain     # この module
sh plan/ws335/tests/run-unit.sh --win32 chain     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 09:50 JST）Sonnet 5.5 high（q1912-i01）。worktree s-ws335-p020（main 4eae22020 から）。

作った物: `userland/base/libcrypto-compat/cryc-chain.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-chain.c`。header・試験の道具・他の module・Makefile は変えていない（`check-skeleton.sh` の BASE=641a2f195 の照合に出る差分は p019 の承認済みの `cryc-x509.h` の 1 語だけ）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`、各 command に `ulimit -f 65536` と `timeout` を付けた）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh chain`（ASan・UBSan、約 1〜2 分） | `test-chain: PASS (22305 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 chain`（x86_64、Wine） | `test-chain: PASS (22305 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（test と ct の 50 本が全部 PASS、`test-chain: PASS (22305 checks)` を含む） |
| `sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲、Windows の static link） |
| `BASE=641a2f195 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: FAIL`: 理由は `cryc-x509.h | 2 +-` の 1 行だけ（p019 の承認済みの変更） |
| `git diff --check` | 出力なし |

実装（`cryc-chain.c`）の形:

- store: `entries` は 1 つずつ malloc した entry の pointer の配列（成長しても entry は動かない。容量は 16 から倍々、上限 CRYC_STORE_MAX）。`cryc_store_add_der` は copy → parse → SHA-256 を lock の外で行い、lock の中で重複（SHA-256 と DER の一致）と上限を見て追加する。`distrusted` は SHA-256 の配列（同じ lock）。`cryc_store_add_pem` は `cryc_pem_next_certificate` を順に呼び、decode できない block・parse できない certificate は数えて飛ばし、良い物は残す（EINVAL）。`cryc_store_add_system` は lock を持たずに `cryc_os_system_roots` を呼び、callback が各々 lock を取る。
- path の組み立て: leaf を最初に、深さ優先。各 node で store の entry（ここに着けば path はそこで終わり）を先に、次に server の他の certificate を、server が送った順に試す。候補は「subject が issuer と cryc_name_equal で等しく、双方に有れば SKI = AKI」。server の certificate の中で、先に試した物と同じ DER の物・store の entry と同じ DER の物は候補にしない（store の方を試すので同じ path）。path にある物（pointer）も候補にしない。leaf が store の entry と同じ DER なら、その 1 枚の path を最初に確かめ、その後に通常の探索へ進む。
- 信頼しない印: 候補を path に積む時に印を引いて node に持たせる（leaf、server の certificate、store の entry のどれも SHA-256 で）。完全な path の最初に leaf から上へ最初の印を探して REJECTED（その深さ）。印の付いた物を含む path の辺は署名を確かめない（仕事に数えない）。
- 署名は「辺」ごとに（子と issuer の組）、候補を積む前（組み立ての途中）に 1 回確かめて覚える。確かめは仕事に数え、CRYC_CHAIN_MAX_WORK で探索を止める。完全な path の確かめはこの記憶を使う（再計算も再計数もしない）。signature の algorithm か issuer の key が CRYC_*_UNSUPPORTED の辺は確かめず、数えない。
- 完全な path の確かめは anchor から leaf へ: 辺の署名 → 有効期間（anchor も）→ name constraints（上の全部の certificate、anchor を含む。途中の self-issued は対象にしない）→ 知らない critical の拡張 → role（leaf は EKU・KU の digitalSignature・key の種類と大きさ。途中の CA は version 3 の cA TRUE・keyCertSign・pathLen・EKU）。anchor は有効期間だけ。1 枚の path（trusted な自己署名の server）は anchor 兼 leaf として有効期間と leaf の確かめを受ける。
- 有効な path の後で名前か address を確かめ（HOSTNAME_MISMATCH・IP_ADDRESS_MISMATCH、chain も返す）、最初の有効な path で探索を終える。有効な path が無ければ「最初の完全な path の code」→ WORK_EXCEEDED → DEPTH_ZERO_SELF_SIGNED（leaf が self-issued）→ SELF_SIGNED_IN_CHAIN（探索の行き止まりが self-issued の server の certificate）→ UNABLE_TO_GET_ISSUER。chain は最初の完全な path か、最も長かった部分の path。

試験の中身（test-chain.c、22305 checks。証明書は derwriter.h と tlspeer.h の署名器・鍵・名前・SPKI の部品で試験の側が作り、期待の値は RFC 5280 の 4.1・4.2・6.1 から手で決めた）:

- store: init・free（2 回の free）、追加の copy・SHA-256・重複・entry の address が成長で動かない・順序、parse できない物・先頭の部分・末尾の 1 byte・NULL・size 0、印（SHA-256 だけ、parse できない byte も可、重複、size 0・NULL は EINVAL、entries には入らない）、上限（4096 枚で ENOBUFS、その後の重複は 0、印も 4096 と 5 番目の重複）、PEM（3 block と前後の文、2 回目は added 0、decode できない block・certificate でない block が EINVAL で良い block は残る、END の無い BEGIN、ENOENT の 4 形、ENOBUFS の時の added の件数、引数）、file（良い・missing・空・bad block）、system（posix は `/etc/ssl/cert.pem` が有れば件数 > 0・無ければ 0、Wine の ROOT は 100 以上、2 回目は増えない）、複数 thread（2 本が 150 枚ずつ `cryc_store_add_der` する間に、本 thread が 10 回の検証と `cryc_store_add_system` 2 回を行い、deadlock せず全部が入る）。
- 検証: 引数（peer 0・17・NULL・store/params/result の NULL、max_depth 1・11・SIZE_MAX が MALFORMED、2・3・10 が通る、名前と address の同時指定、時刻が無い時の時計）、正常（中間 1 枚・store が中間・順序の違いと重複と無関係な root・root も送られた場合・leaf 直下の root・P-384 の中間・trusted な自己署名の server・無い時の DEPTH_ZERO_SELF_SIGNED・空の store）、AKI/SKI（不一致で除外・片方が無ければ除外しない・1 byte 短い AKI・issuer の名前の大文字小文字）、有効期間（leaf・中間・anchor、境界の ±1 秒、anchor が先に見られる）、署名（leaf・中間の 1 bit・別の key・SHA-1・anchor の署名は見ない・RSA の algorithm と EC の key・RSA 1024・2047・2048 の issuer・8200 bit の issuer・EC 以外の key・leaf の RSA 1024・2047・2048・2049 と未対応の key）、role（中間の BC 無し・cA FALSE・version 1・critical でない BC・KU・EKU の全組、leaf の EKU・KU・CA の leaf・version 1 の leaf）、pathLen（0・1・2・上限の超え・self-issued の途中は数えない・名前が大文字小文字違いの self-issued も・root の pathLen は見ない）、name constraints（anchor・中間・critical でない物・excluded・IP・URI の評価できない形・malformed・leaf 自身の物は無効・self-issued の leaf は検査される・self-issued の中間は検査されない）、拡張（知らない critical・critical でない・root の物は見ない・certificatePolicies の critical は通す）、深さ（10 枚・11 枚・9・長い path より後の短い有効な path）、名前と address（一致・大文字・末尾の dot・不一致で chain も返す・address の長さ違い・期限切れが名前より先）、探索（cross-sign の 2 つの中間でどちらを通るか・両方が悪い時は最初の完全な path の code・store の候補が先・root が untrusted で SELF_SIGNED_IN_CHAIN・同じ名前の別の鍵の root・UNABLE_TO_GET_ISSUER の depth と chain・cycle・同じ長さの部分の path は最初の物・self-issued だが候補の有る物は行き止まりでない・look-alike の 1〜15 組の探索を、深さ優先・組ごとに 1 回の署名・node の上限を試験の側に書いた小さな model で追って、止まる深さまで一致させた・8 と 7 の look-alike で署名がちょうど 64 回の件・最初の完全な path の code が WORK_EXCEEDED に勝つ件）、信頼しない印（leaf・中間・anchor・複数の最小の深さ・他の検査に勝つ・印の後の add_der・store の中間が anchor で印・trusted な自己署名の server・cross-sign の別の中間・別の anchor で通る・両方に印・REJECTED が WORK_EXCEEDED に勝つ）、乱数（leaf か中間の 1 byte を 600 回変えて、parse できた物が有効になることが無い・1500 回の無作為な部分集合と順序と store で、code の範囲・chain の形・OK の時の leaf と depth）、実の bundle（`WS335_CACERT` の 121 枚: 全部が読める・2 回目は増えない・各 root を 1 枚の server として検証して 38 枚が有効 83 枚が KU（keyCertSign だけで digitalSignature が無い）・全部に印を付ければ全部 REJECTED）。
- x509-limbo: id が `rfc5280::`・`pathlen::`・`webpki::`・`pathological::` の SERVER の 179 件（99・13・56・11）を、trusted の物を store、peer と untrusted の物（15 枚まで: 実際の server が送る数の上限、4 件（pathological の 100 枚を越える物）は 16 枚で切る）を server の list、case の時刻・名前・address・深さ（limbo は中間の数、こちらは全部の数なので +2。converter が 0 と「無し」を区別しないので、features に max-chain-depth を持つ 0 は 2）で検証した。

limbo の件数: 179 件のうち、SUCCESS の期待が 40 件（37 件が有効、3 件が下の key-usage・depth の違い）。期待と一致が 133 件（leaf が parse できず検証の前に拒んだ 7 件を含む。全部 FAILURE の期待）、**違いが 46 件、全て理由を付けて試験の表に固定**（表に無い違いは FAIL、表の物が違わなくなっても FAIL）。違いの理由と件数:

| 理由 | 件 | id と理由 |
| --- | --- | --- |
| profile（CA/Browser Forum・webpki の profile、RFC 5280 の発行の規則で、path の検証（6.1）と header の検査の一覧には無い） | 26 | `rfc5280::aki::{critical-aki, leaf-missing-aki, intermediate-missing-aki, cross-signed-root-missing-aki}`・`rfc5280::ski::{root-missing-ski, intermediate-missing-ski}`（AKI・SKI の有無・critical）、`rfc5280::san::noncritical-with-empty-subject`・`webpki::san::san-critical-with-nonempty-subject`（SAN の critical の規則）、`rfc5280::serial::zero`、`rfc5280::leaf-ku-keycertsign`、`webpki::cn::*` の 9 件（CN と SAN の一致の規則）、`webpki::eku::{ee-anyeku, ee-critical-eku, ee-without-eku, root-has-eku}`、`webpki::malformed-aia`（AIA は読まない）、`webpki::forbidden-rsa-key-not-divisible-by-8-in-leaf`（2049 bit は 2048 以上）、`webpki::ee-basicconstraints-ca`（leaf の cA は拒まない。`rfc5280::ca-as-leaf` は SUCCESS の期待） |
| anchor（trust anchor の中身は 6.1.1 (d) の通り、そのまま信頼する） | 9 | `rfc5280::unknown-critical-extension-root`・`rfc5280::root-missing-basic-constraints`・`rfc5280::root-non-critical-basic-constraints`・`webpki::aki::root-with-aki-*` の 5 件・`webpki::forbidden-rsa-not-divisible-by-8-in-root` |
| name（名前の module の header の定め） | 4 | `rfc5280::nc::invalid-dnsname-leading-period`（header は先頭の dot を許す）、`rfc5280::san::underscore-dns`（header は `_` を許す）、`webpki::san::public-suffix-multi-label-wildcard-san`・`...private-namespace-wildcard-san`（public suffix の表を見ない） |
| decided（下の決めた点） | 3 | `rfc5280::nc::permitted-dns-match-noncritical`・`rfc5280::nc::not-allowed-in-ee-noncritical`・`rfc5280::nc::not-allowed-in-ee-critical` |
| key-usage（leaf の KU に digitalSignature が無いと拒む: header。case は SUCCESS） | 2 | `rfc5280::ca-as-leaf`・`pathlen::validation-ignores-pathlen-in-leaf`（どちらも leaf が keyCertSign だけの CA） |
| depth（case は self-issued の中間を深さに数えない。header は全部の certificate） | 1 | `pathlen::max-chain-depth-1-self-issued` |
| no-policy（policy を処理しない） | 1 | `rfc5280::pc::ica-noncritical-pc` |

p019 が p020 に残した 4 件の決定（RFC 5280 の path の規則で）:

1. `rfc5280::nc::permitted-dns-match-noncritical`（期待 FAILURE）: Q1 の判断（p017・p019）どおり critical でない nameConstraints を処理する。permitted の中の名前なので有効（期待と違う）。
2. `rfc5280::nc::not-allowed-in-ee-noncritical`・`rfc5280::nc::not-allowed-in-ee-critical`（期待 FAILURE）: 6.1.4 (g) の name constraints は path の最後の証明書でない物（CA）の処理で、最後の証明書（leaf）の拡張は 6.1.5 の wrap-up でも処理されない。4.2.1.10 の「CA の証明書にだけ使う」は発行の規則で path の検証の規則でない。header の検査の一覧にも無いので、leaf 自身の nameConstraints は何も制約せず、有効（試験で leaf に permitted `example.com` と SAN `evil.org` で OK を固定）。
3. `rfc5280::nc::invalid-dnsname-leading-period`（期待 FAILURE）: header（cryc-name.h）が dNSName の制約の先頭の dot を「その下の名前だけ」と定めるので許可（p019 と同じ）。

自分で決めた点（header が黙っていた所）:

1. `cryc_store_add_pem` / `add_der`: 入力の NULL・size 0 は EINVAL（`add_pem` は NULL の store・「NULL の text に size 有り」が EINVAL、NULL の text と size 0 は ENOENT）。decode も parse もできない block だけの text は EINVAL（ENOENT は block が 1 つも無い時だけ）。ENOBUFS・ENOMEM の時も `*added` にそれまでの件数を入れる。`added` は NULL でもよい。上限での重複は 0（ENOBUFS でない）。
2. `cryc_store_add_system`: callback の中で、parse できない certificate（`add_der` が EINVAL）・bundle の中の bad block（`add_file` が EINVAL、良い block は残る）・size 0 の印は、エラーにせず飛ばす（header の「最初の ENOENT 以外の error を返す」の字面とは違う: 一枚の変わった証明書で全部の root が読めなくなるのを避ける。Windows の ROOT の実在の証明書を読む時に効く。Q1 が字面どおりを望めば callback の 2 行を外す）。
3. `cryc_chain_verify`: NULL の result は EINVAL。NULL の store・params・peer（count > 0）は EINVAL を返し、result があれば MALFORMED を入れる。範囲の外の引数（peer_count が 16 を越える・max_depth が 1 か 10 を越える・名前と address の同時指定）は MALFORMED を入れて 0 を返す。header は max_depth の範囲を「2 から CRYC_CHAIN_MAX_DEPTH」としか書かない。
4. result の code と depth: OK と名前・address の不一致は depth 0（chain を返す）。CHAIN_TOO_LONG の depth は max_depth（制限を越える最初の位置）。name constraints の check が EINVAL（制約か名前が壊れている）を返したら MALFORMED（その certificate の depth）。署名の確かめが EBADMSG・EINVAL（algorithm と key の種類が合わない）なら SIGNATURE_FAILURE、ENOTSUP は issuer の key が RSA で 2048 bit 未満なら KEY_TOO_SMALL、他は UNSUPPORTED_ALGORITHM。
5. 探索の組み立ての細部: 署名の確かめを候補を積む前（組み立ての途中）に行う（header の「署名の確かめ CRYC_CHAIN_MAX_WORK 回で探索を止める」を、完全な path ができる前にも効かせるため。そうしないと、行き着く anchor の無い look-alike の鎖で WORK_EXCEEDED が出る時が無い）。結果は組で覚え、完全な path の確かめで再計算しない。印の付いた path の辺は確かめない（REJECTED は仕事に数えない）。組み立ての walk に node の上限 1024 を足した（署名が全部確かめ済みの組を辿る path の数が多い鎖の時間を抑える。これも WORK_EXCEEDED。6 組以上の look-alike で実際に働き、試験の model が同じ数で追う）。
6. trust anchor の確かめは有効期間だけ（pathLenConstraint・basicConstraints・keyUsage・extKeyUsage・critical の拡張・署名は見ない）。1 枚の path は leaf の確かめ（EKU・KU・key）も受ける。path の長さの制限は path の全部の certificate の数（leaf と anchor を含む）。
7. 内部: `struct chain_search`（約 3 KB の stack）、`struct chain_node`・`chain_edge`・`chain_snapshot`、static の補助。

header の comment で気づいた点（直すかは p035）: `cryc_chain_verify` の「Returns 0 or ENOMEM」は NULL の引数で EINVAL を返す・範囲の外で MALFORMED を返すことを書いていない。`cryc_store_add_system` の ENOENT 以外のエラーの字面（上の決めた点 2）。`cryc_store_add_pem` の ENOENT の条件（block が 1 つも無い）と EINVAL（壊れた block だけ）。WORK_EXCEEDED の「署名 CRYC_CHAIN_MAX_WORK 回」に node の上限が加わった点。max_depth の範囲の外が MALFORMED である点。phase.md が仕様と食い違う点は無かった。

わざと壊す確かめ（git の外。`build/tmp/p020-mut/mutate.py` が cryc-chain.c の複写を機械的に変える: 演算子（`==`/`!=`、`<`/`<=`、`&&`/`||`、`+`/`-`、`&`/`|`、`<<`/`>>`、`++`/`--`）、数値の ±1、return の値、if・while の条件を 0/1/否定、assign する code の置換、1 行の文・break・continue の削除。複写だけを `cc -std=c89 -O2` で build し、他の module と試験の object は共通。1 本ごとに `RLIMIT_FSIZE` 64 MB、`RLIMIT_CPU` 400 s、RSS が 6 GB を越えるか 150 s で kill、同時 4 本。実装 file は変えない。出力は file）:

- 1165 通り。build の誤りは 0（non-building は無かった）。1 回目（試験を足す前）は 1170 通りで見逃し 301、試験を足した後の 2 回目で 897 通りが検出（試験の失敗 603、crash 281、timeout 13）、見逃し 268。さらに試験を足し（下）、見逃しの 268 通りを流し直して 34 通りが検出（試験の失敗）、残り 234 通りを ASan・UBSan・LeakSanitizer 付きで流し直して 18 通りが検出（leak 13、境界の外の読み書き・未初期化の読み 5）。**最終: 検出 949 通り（試験の失敗・crash・timeout・sanitizer）、見逃し 216 通り**。
- 試験を足した所（見逃しから）: add_pem の引数エラーでの `*added`、parse できない certificate だけの block、ENOBUFS の時の added、self-issued の判定が名前の比較（大文字小文字違い）による件、AKI が 1 byte 短い件、RSA 2047 bit と 8200 bit の issuer・leaf、self-issued の leaf が anchor の name constraints を受ける件、同じ長さの部分の path は最初の物、2 つの有効な anchor の最初が答え、leaf と root だけで SELF_SIGNED_IN_CHAIN の depth 1、行き止まりでない self-issued、look-alike の探索を model で追う件（署名が 64 回ちょうどの件も）。
- 見逃し 216 通りは全部、観測できる違いが無い物として個別に確認した（件数は区分の目安）: (a) 真偽の値の 1→2・初期値・flag の値・loop の守りの細部 49。(b) 重複と store の twin を候補から外す最適化（結果は同じで仕事の量だけ）と leaf の twin の loop の細部 41。(c) 署名の記憶・組み立て中の署名の確かめを飛ばす条件・打ち切りの深さの計算などの仕事の数え方 31（印の付いた path の辺を数えない件は結果に出ない: 署名の予算が尽きる直前に印の付いた path に着く鎖を作れておらず、読んで確かめた）。(d) 確保・hash・mutex の失敗の経路（失敗を注入できない）、配列の成長の大きさ、lock の除去（TSan が要る）28。(e) 系統の root（`cryc_os_system_roots`）の callback の誤りの写し 28（試験で差し替えられる系統の source が無い。成功の経路は posix は無ければ 0、Wine は ROOT の 100 枚以上で確かめた）。(f) RSA の modulus の先頭の 0 の byte の処理など、parse が必ず取り除くので到達しない物・到達しない守り 39（node の上限の `>` と `>=` なども、境界が 1024 の 1 つ違いで model が同じ数で追って見分けられる物は検出済み）。
- build の誤りの mutant は 0 だったので別の形での流し直しは無い。

未実施: 確保の失敗・hash の失敗・mutex の初期化の失敗の注入（malloc の差し替えが ASan と合わない）、TSan での lock の除去の検出、`cryc_os_system_roots` の差し替え（header の約束で試験は実の物を使う）、Windows の Disallowed の store に実際に印を付ける試験（p015 が OS 層で Wine を使って行った）、aarch64 Windows の実行（build のみ）、32 bit の size_t の機械、性能の測定、`run-ct`（秘密を扱わない）、x509-limbo の 9,792 件の全件（p038）。実機・QEMU は範囲外。

commit: 09bc044935（実装と試験の最初）、8973c1d711（試験の追加・実装の整理・この結果の節・ws.md の行）。
