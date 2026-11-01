<!-- awesome-plan project=zedbsd record=ws335-p026 -->

# ws335-p026: record の保護

Status: 実装済み・試験 PASS（2026-11-02 02:15 Sonnet。cleared は Q1。依存: p025、p005、p006）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-record.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 5.1〜5.5。
  - `rfc5246.txt`（SHA-256 `2ebe1b8e651696aeacc8ab3848bedb465505737901ea3b691acf40c392958feb`）: 6.2.1〜6.2.3.3。
  - `rfc5288.txt`（SHA-256 `f68375c3f1948802240ca5b21f3aa44311ea02f877e4e240d1fc70927ecf1307`）: 3。
  - `rfc7905.txt`（SHA-256 `afa1e233822c6bda1f1ced014d32e6d740454415b53130fa84630a9c5ad2fe69`）: 2。
  - `rfc8448.txt`（SHA-256 `6564d1376d1ec744fc7a9993da15ebc1b9be361908b166091f47ef605c537fba`）: 3（暗号化された record の値）。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: rfc8448。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-record.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-record.c`。定数時間の試験: `plan/ws335/tests/unit/ct-record.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- RFC 8448 の record を開き、同じ鍵と sequence で閉じて同じ bytes になること。
- tag の失敗は平文を 0 に。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh record     # この module
sh plan/ws335/tests/run-unit.sh --win32 record     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh record
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 02:15 JST）Sonnet 5.5（q1885-i01）。worktree s-ws335-p026（main 56abae2bf8）。

作った物: `userland/base/libssl-compat/sslc-record.c`（実装）、`plan/ws335/tests/unit/test-record.c`、`plan/ws335/tests/unit/ct-record.c`。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh record` | `test-record: PASS (240579 checks)` / `run-unit: PASS`（ASan・UBSan） |
| `sh plan/ws335/tests/run-unit.sh --win32 record` | `test-record: PASS (240579 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh record` | `ct-record: PASS (49 checks)` / `run-ct: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、2 つの compiler・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `sh plan/ws335/tests/run-unit.sh all` | test-* 18 本・ct-* 12 本が全部 PASS / `run-unit: PASS` |
| `git diff --check` | 出力なし |

試験の中身:

- RFC 8448（`rfc8448.vec`、section 3〜7）: file を順に読み、`derive write traffic keys`（と、server の `derive read traffic keys` は client の write 鍵）の key・iv で方向を作り、保護された 27 件の record を全部、同じ鍵・sequence で seal し直して wire と byte 単位で一致、open して type・平文が一致、各 byte の 1 bit 変更が拒否されること（header の type・長さは EPROTO、他は EBADMSG）。暗号化されない 14 件（ClientHello の 0x0301、ServerHello、ccs）も seal・open が一致。件数（27 と 14）を数えて照合。
- header: type 256 通り × 4 種の方向（平文・1.3・1.2 AES-GCM・1.2 ChaCha20）、version（検査なしと 0x0303 指定）、長さの境（各方向の最小・最大の前後 5 通りと間隔の広い値、65535）、1.3 の ccs は長さ 1 だけ。overhead の式と 16385 以上は 0。
- 9 suite（表の全部）× 16 の大きさ（0〜16384）× 10 の sequence（0・1・255・256・65536・2^32-1・2^32・任意・2^63・2^64-2）× type を、試験の中の参照（RFC 8446 5.2・5.3、RFC 5246 6.2.3.3、RFC 5288 3、RFC 7905 2 の式から nonce と追加データを別に作り、libcrypto-compat の AEAD を直に呼ぶ）と byte 単位で比較。大きさぴったりの malloc の block、1.2 AES-GCM の explicit nonce が sequence の BE であること、view が record の中、再 seal が別 bytes。
- TLS 1.3 の inner plaintext: padding 0〜5000 × 6 つの大きさ × 3 種の type（参照が作る）で、inner が 2^14+1 までは type と内容が戻り、それ以上は EMSGSIZE。全部 0 の inner は EPROTO（EMSGSIZE が先の時も）、type byte 0 の時は最後の非 0 byte が type、偽造 tag と長すぎる inner は EBADMSG が先、ciphertext 2^14+256 超は EMSGSIZE。
- 外側の type: 1.3 で ccs は 1 byte・値 1 だけ（長さ 0・2・長い物・値 0〜255 の他・全て EPROTO）、handshake・alert・他の type は EPROTO、1.2 では type が追加データに入り EBADMSG。
- 偽造・切れた入力: 全 suite で record の全 bit の反転（EBADMSG の時は平文の位置が全部 0、sequence は進まず、後で正しい record が開く）、sequence・鍵・IV・salt の違い、header のままの全ての短い size（EPROTO）、長さを合わせて直した全ての短い size（EBADMSG）、1 byte 長い size。大きさぴったりの heap block（ASan）。
- sequence: 9 suite × 300 件の lock-step、順序違い、鍵の更新で 0 に戻る、2^64-2 の次は使え 2^64-1 は seal・open とも EOVERFLOW で何も書かず sequence も変えない、平文の方向は sequence が 0 のまま。seal の誤り（type 252 通り・大きさ超過・NULL の入力・room 0〜total-1 は何も書かず ENOBUFS・ぴったりで境の外に書かない）。set_keys の誤り（NULL suite・AEAD 無し・version13 の食い違い・鍵と IV の size の過不足と構造体を越える値）は構造体が 1 bit も変わらない、成功で sequence 0・鍵の置き換え・clear が全 0。
- 乱数: 10 種の方向 × 3000 の乱数 record（header を合わせた物を含む。結果は 0・EBADMSG・EPROTO・EMSGSIZE のどれかで、成功なら view が範囲内）、乱数 header 50000、9 suite × 120 の乱数の往復（任意の初期 sequence）。
- 定数時間（ct-record、49 checks）: 6 suite（1.3 の 3 つと 1.2 の AES-128・AES-256・ChaCha20）で鍵・IV・平文を CT_SECRET にして set_keys・seal・open（良い record と、tag を壊した record）。判定・type・size・sequence・平文は呼び出しの後で CT_PUBLIC（呼び手が知る値）。

わざと壊す確かめ（git の外。`build/tmp/p026/brk.py`: 実装を 63 通りに書き換えた複製を `build/tmp/p026/run-<時刻>/m<番号>/` に作り、非 sanitizer の -O1・warning を error で build して試験を流す。実装 file は触らず、前後の sha256 が同じことを script が確認した）:
- 58 通りが試験の失敗（FAIL の出力。うち 7 通りは FAIL を出した後に segfault で落ちた: seal や overhead の誤りで後続の buffer が壊れるため）。build の誤りで落ちた物は 0（最初の 5 通りは未使用の変数で build が通らなかったので、同じ意味で build が通る形に書き直した）。
- 見逃し 5 通りは全部等価: set_keys の `key_size != suite->key_size`（`>` に: 小さい鍵は `cryc_gcm_init`・ChaCha の size 検査が EINVAL で拒む二重の防御）、`key_size > sizeof(key)`（同様に後段が拒む）、set_keys の途中で local を wipe する・clear を呼ぶ・clear の中の wipe を外す（観測できる状態が同じ: 後で全部上書き・memset される）。
- 定数時間の確かめ（`build/tmp/p026/ctbrk.py`、-O2・valgrind）: 6 通り（scan の中の平文による分岐、tag の判定での早い return、鍵の byte による分岐、IV による分岐、seal の平文による分岐）のうち 5 通りを memcheck が検出。見逃し 1 通りは平文の方向の seal（平文の方向は秘密を持たないので ct-record の対象外）。

自分で決めた点（header が黙っていた所）:

1. `sslc_record_open` の size は header 込みの全体。`size < 5` か header の長さ欄 + 5 と食い違えば EPROTO。
2. TLS 1.3 の ccs は、Q1 の指示（q1885 の途中）で、暗号化されない 1 byte・値 0x01 だけを受ける。header 検査（暗号化済みの 1.3 の方向）は type 20 の長さが 1 でなければ EPROTO（保護の最小長 17 は適用しない）。open は長さ 1・値 1 の時だけ type 20・size 1 で返し、他は EPROTO。sequence は進めない。他の type（handshake・alert）が暗号化された 1.3 の方向に平文で来たら EPROTO。
3. 失敗した open の `out`: AEAD・長さ・padding の失敗では type・size は 0、data は使わない。header・size の食い違い・EOVERFLOW・ccs の誤りは data も NULL。成功の時だけ sequence が進む。
4. 判定の順: 先に公開の長さ（size・sequence の overflow・最小長）、AEAD の tag、その後で 1.3 の inner の長さ（EMSGSIZE）と全部 0（EPROTO）。tag が悪ければ必ず EBADMSG。tag の後の処理（inner type の探索・判定・sequence の更新）は分岐無しで、全 byte を見る（header の注記は padding の長さに時間が依るのを許していたが、それより強くした。空き padding の時間差が無い）。
5. TLS 1.2 の追加データの version は、open では record の header の version を、seal では 0x0303 を使う（RFC 5246 6.2.3.3 の TLSCompressed.version）。
6. TLS 1.2 の ChaCha20-Poly1305 の nonce は IV xor sequence（RFC 7905 2）、AES-GCM は 4 byte の salt + 8 byte の explicit（seal は sequence の BE、open は record の bytes）で、AES-GCM は 12 byte の nonce を `cryc_gcm_seal・open` に通す（Q1 の判断）。
7. seal の誤り: type が 20〜23 以外、size > 16384、NULL の in で size > 0 は EINVAL。順は EINVAL → ENOBUFS → EOVERFLOW。平文の方向は sequence を使わず進めず EOVERFLOW も無い。
8. header 検査の誤りの値: type が 20〜23 以外と version の不一致は EPROTO、長さ超過は EMSGSIZE、保護された方向の最小長未満は EBADMSG、平文の方向の長さ 0 は EPROTO。出力 `*type`・`*length` は成功の時だけ書く。version_check が 0 の時は 0x0301〜0x0303。
9. `sslc_record_overhead` は size > 16384 で 0。
10. `sslc_record_set_keys` は、suite が NULL・AEAD 無し・version13 と suite の version の食い違い・key_size・iv_size が suite の表と違う・構造体の配列より大きい、のどれでも EINVAL で、direction は 1 bit も変えない（成功する時は local に作ってから置き換え、古い鍵と local を wipe）。

header と phase.md の食い違い: 見つからなかった。header の open の comment は ccs を「長さ 1 以上でよい」とは書いておらず、Q1 の指示と同じ形にした。

未実施: 32 bit の size_t の機械・aarch64 Windows の実行（build のみ、check-skeleton）、実機・QEMU（範囲外）、全 suite の巨大 padding を含む fuzz の長時間実行、コードの規約の第三者のレビュー（自分の diff を §14 の checklist で見直した）。
