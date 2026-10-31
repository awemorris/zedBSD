<!-- awesome-plan project=zedbsd record=ws335-p020 -->

# ws335-p020: trust store と path の検証

Status: planned（2026-11-01 p001 が作成。起動は Q1。依存: p018、p019、p015）
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
BASE=NEWBASE sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

（未着手）
