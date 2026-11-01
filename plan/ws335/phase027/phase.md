<!-- awesome-plan project=zedbsd record=ws335-p027 -->

# ws335-p027: transcript と鍵の schedule（1.3 と 1.2）

Status: cleared（2026-11-02 01:41 Q1）。前: 実装済み・試験 PASS（2026-11-02 Sonnet q1882-i01。cleared の判定は Q1）。前: planned（2026-11-01 p001 が作成。依存: p025・p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-keysched.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 4.4.1、4.4.4、7.1〜7.3。
  - `rfc8448.txt`（SHA-256 `6564d1376d1ec744fc7a9993da15ebc1b9be361908b166091f47ef605c537fba`）: 3、5。
  - `rfc5246.txt`（SHA-256 `2ebe1b8e651696aeacc8ab3848bedb465505737901ea3b691acf40c392958feb`）: 5、6.3、7.4.9、8.1。
  - `rfc7627.txt`（SHA-256 `3e908b1a511f5894b64b2c7867c5e0e8db43896add4d99a340fe67a07ee11a59`）: 4。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の data（`WS335_VECTORS`、共有は `/home/awe/zedBSD-claude1/build/sources/tls-vectors`、無ければ `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-vectors`。`rec/<名前>.vec` を [vecfile.h](../tests/unit/vecfile.h) で読む）: rfc8448。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-keysched.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-keysched.c`。定数時間の試験: `plan/ws335/tests/unit/ct-keysched.c`（ct.h の CT_SECRET で秘密を印し、run-ct.sh で流す）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- 定数時間: 秘密の入力を CT_SECRET にして流し、run-ct.sh が PASS。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- RFC 8448 の全部の secret・key・iv・finished を一致させる。HRR は section 5。
- 1.2 の PRF の期待の値: 公式の vector が無いので、試験の側で HMAC（p002 で確かめ済み）を RFC 5246 5 の式どおりに組んで作り、sslc_tls12_prf と比べる（同じ式の 2 つ目の実装、他の library の出力は使わない）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh keysched     # この module
sh plan/ws335/tests/run-unit.sh --win32 keysched     # 同じ試験を Windows（x86_64、static link、Wine）で
sh plan/ws335/tests/run-unit.sh all
sh plan/ws335/tests/run-ct.sh keysched
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

- §6 の command が全部 PASS（最後の行を自分で読む）。check-skeleton.sh の warning 0（cc・clang・zedBSD の clang・llvm-mingw の 2 arch）と、Windows の static link・名前の範囲（check-skeleton の 6・7）。
- [coding-style.md](../../coding-style.md) §14 の checklist で自分の diff を見直した。表の block ごとの出典の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 「結果」の節: 日付（host の `date`）、実行した command と最後の行、試験の件数、わざと壊す確かめの内容と結果、自分で決めた点、未実施。ws.md の行を「実装済み・試験 PASS」に。

## 結果

2026-11-02（host の `date` は 01:38 JST）Sonnet 5.5（q1882-i01）。worktree s-ws335-p027（main ad65fbe3f6）。

作った物: `userland/base/libssl-compat/sslc-keysched.c`（骨格の ENOSYS を置き換え）、`plan/ws335/tests/unit/test-keysched.c`、`plan/ws335/tests/unit/ct-keysched.c`。header・試験の道具・他の module・Makefile は変えていない。header と phase.md の食い違いは見つからなかった。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh keysched`（ASan・UBSan） | `test-keysched: PASS (79369 checks)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-unit.sh --win32 keysched ct-keysched`（x86_64、Wine、static link） | `test-keysched: PASS (79369 checks) (win32)` / `ct-keysched: PASS (55 checks) (win32)` / `run-unit: PASS` |
| `sh plan/ws335/tests/run-ct.sh keysched`（valgrind memcheck、-O2） | `ct-keysched: PASS (55 checks)` / `run-ct: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、名前の範囲、Windows の static link を含む） |
| `sh plan/ws335/tests/run-unit.sh all` | test-* 17 本と ct-* 11 本の全部 PASS / `run-unit: PASS` |
| `git diff --check` | 出力なし |

試験の中身（test-keysched、79369 checks）:

- RFC 8448（`rfc8448.vec`）の全ての HKDF-Expand-Label（section 3〜7 の 100 件超。各 step の info を label・context・length に分解して実装の `sslc_tls13_expand_label` と、printed の info に対する HKDF 直接の両方で printed の expanded と一致。info の構造と、label と context から試験側で組んだ HkdfLabel が printed の info と一致することも照合）、全ての key・iv（`sslc_tls13_traffic_keys`）、section 4 の PSK binder の finished（`sslc_tls13_finished`）。
- PSK 無しの 4 本（section 3 simple 1-RTT、5 HelloRetryRequest、6 client authentication、7 compatibility mode）は全体を通す: transcript を ClientHello から順に、SHA-256 と SHA-384 を両方持つ状態で流し、ServerHello で select、section 5 は HRR を random で見分けて select → restart_hrr（結果を 0xfe 00 00 20 || H(ClientHello1) の試験側の計算と照合）→ HRR を吸収。各点の transcript hash（c hs traffic・c ap traffic・res master の hash）、Early・Handshake・Master Secret、4 つの traffic secret、server と client の Finished（message の中身と printed の finished の両方）が一致。section 4（PSK・0-RTT）は schedule が PSK を持たないので、expand-label・key・binder だけを照合し、通し（transcript と schedule）は未実施。
- RFC 8448 に無い値（SHA-384、32 byte の key、traffic update、TLS 1.2）は、試験側で HMAC・HKDF を式どおり（RFC 8446 7.1、RFC 5246 5）に別の形で書いて照合: expand_label（label 8 種 × 長さ 12 × context 7 × 2 hash、最長 label 249 と最長 255 block）、schedule 全体（hash 2 × shared の大きさ 7 × 10 回）、traffic keys（3 つの 1.3 suite）、finished と update の 3 世代、TLS 1.2 の PRF（secret 10 × seed 6 × 出力 16 の大きさ × 2 hash、1024 まで、短い出力が長い出力の先頭）、master（ems と通常、pre-master 4 通り）、key block（6 つの 1.2 suite、random の順と 4 つの切り出し）、finished（client と server、2 hash）。
- transcript: 大きさ 16 通りの組 × select の前後 × 2 hash、分割と NULL・size 0、hash の読み出しで終わらない事、struct の複写の独立、select の規則（同じ hash の 2 回目は 0、別の hash は EINVAL、sha512・none・99 は EINVAL、拒否は変化無し、選ばれなかった方の state が 0 に消える）、restart_hrr（2 hash × ClientHello1 を select の前後に吸収）、clear が全部 0。
- 誤りの入力: 各関数の全ての EINVAL の条件と、拒否の時に出力・schedule・keys が変わらない事（guard の byte）、NULL と size 0 の組。乱数の呼び出し（3000 round × 全関数、random の hash id・大きさ・NULL、ちょうどの大きさの heap block、ASan・UBSan で落ちず戻り値は 0 か EINVAL）。
- ct-keysched（55 checks）: 共有の秘密・pre-master を CT_SECRET にして 1.3 の 3 suite と 1.2 の 3 suite（2 hash、3 つの key の大きさ、ems と通常）の全部の鎖（start → handshake → master → keys → finished → update、pre-master → master → key block → finished・PRF）を秘密のまま通し、最後に CT_PUBLIC。memcheck の指摘 0。

わざと壊す確かめ（git の外。`build/tmp/p027/brk.py`: 実装を複写して 1 か所だけ書き換え、他の module は 1 度 build した object を使い、test-keysched（非 sanitizer の -O1）か ct-keysched（-O2、valgrind）を build して流す。元の file の sha256 が前後で変わっていない事を確認、`git diff` も空）。117 通り（unit 109、ct 8）の内容: label 1 文字の違い（tls13 の prefix・derived・c/s hs・c/s ap・key・iv・finished・traffic upd・key expansion・master secret・extended master secret・client/server finished）、HkdfLabel の長さの byte・context の長さ・context の欠落、上限の境（label 249・248、context 255・254・256、255 block、shared 128・127・129、PRF 1024・1023・1025）、NULL の許可、Early/Handshake/Master の ikm・salt・secret の取り違えと格納漏れ、client と server の入れ替え、keys の suite・version・hash の検査の欠落と key_size・iv_size の誤り、finished の key と hmac の長さ、update の部分複写、transcript（片方の hash だけ吸収、select の wipe の向き・省略・sha512 や none の受け入れ・別 hash の再 select の許可、hash の取り違えと size の戻り値、HRR の type 254→253・length の byte・header と digest の欠落と短縮・select 前の許可、clear の省略）、P_hash（A の更新を block から・更新の停止、block の label と seed の欠落、A(1) の label と seed の欠落、最後の block の部分利用の欠落、block 数のずれ）、master（label と size の入れ替え、flag 2 の許可、空の pre-master、47 byte）、key block（random の順、label、client と server の key と IV の入れ替え、IV の offset、総和、同じ構造体の許可、1.3 suite の許可、出力の wipe の省略、server の iv_size、client の suite）、finished（label の入れ替え、11・13 byte、hash 大きさの固定）、定数時間（finished_key・Handshake Secret・shared・update・P_hash の block・pre-master・key block・traffic key の秘密への分岐と秘密による index を入れた 8 通り）。
結果: 初回の 117 通りのうち 108 通りが検出（`FAIL` 96、NULL の参照で SIGSEGV 4、memcheck 8。build の誤りは 0）、BAD-MUTATION（書き換えの目印が複数 hit して適用されず）4 通りは目印を絞って再実行し、全部が `FAIL` で検出。`keys version not checked`（TLS 1.2 の suite を 1.3 の関数に渡して version の検査だけを外す）は、IV の大きさの検査が先に弾いて初回は見逃し。TLS 1.2 の ChaCha20-Poly1305 の suite（hash・IV・key の大きさが 1.3 と同じ）を渡す検査を足して検出。見逃し 4 通りは全て等価:
- 255 block の上限を 256 block に: `cryc_hkdf_expand` が同じ上限で EINVAL を返す（二重の防御）。
- Early Secret の salt を zeros の size-1 に: HMAC が鍵を block まで 0 で埋めるので、同じ鍵になる（等価）。
- `sslc_tls13_master` の transcript_hash の NULL を許す: 下の `sslc_tls13_expand_label` が context の NULL と size を EINVAL にする（二重の防御）。
- `sslc_tls12_prf` の secret の NULL と size > 0 を許す: `cryc_hmac_init` が EINVAL を返す（二重の防御）。

自分で決めた点（header が黙っていた所）:

1. 受ける hash: 全ての関数で SHA-256 と SHA-384 だけ。SHA-512・none・その他は EINVAL（expand_label・finished・update・TLS 1.2 の関数も同じ。header の「hash 大きさ」の想定が suite の hash だけのため）。
2. `sslc_transcript_select`: 同じ hash の 2 回目は 0、選んだ後の別の hash は EINVAL（捨てた方は戻せない）。選ばれなかった方の hash の state は cryc_wipe で 0。`sslc_transcript_restart_hrr` は選んだ hash を新しく始め直して `fe 00 00 <size>` と ClientHello1 の hash を吸収（2 度呼べば、その時の状態の hash で同じ事を繰り返す。header に規定が無く、試験していない）。`sslc_transcript_update` の size 0 は message が NULL でもよい。
3. `sslc_tls13_start` は schedule 全体を先に wipe（以前の内容を残さない）。schedule に段階の field が無いので、`sslc_tls13_handshake`・`sslc_tls13_master` を順序どおりに呼ぶかは検出できない（呼び出す側の責任。検査するのは hash が 256/384 で size が hash の大きさと一致する事 = start 済みである事だけ）。handshake・master は途中の誤りで schedule を変えない（一時変数で作ってから複写）。
4. `sslc_tls13_handshake` の shared の大きさ: 1 から 128（0 は EINVAL、129 以上も EINVAL。x25519・secp256r1 の 32、secp384r1 の 48、X25519MLKEM768 の 64 を越える余裕。上限は呼び出し側が際限無く hash しないための防御）。
5. `sslc_tls13_expand_label`: label 1〜249 文字、context 0〜255、length 0〜255 × hash の大きさ（65535 の上限は超えない）、out が NULL なのは length 0 の時だけ許す。`sslc_tls13_traffic_keys`: suite の version が TLS 1.3、hash が引数と一致、key_size が 1〜32、fixed_iv_size が 12 のとき以外は EINVAL。成功時は keys を先に全部 0 にしてから書く（key の余りは 0）、失敗時も 0 にする（引数の検査で弾く時は触らない）。
6. TLS 1.2: `sslc_tls12_prf` の secret と seed は size 0 なら NULL でもよい（secret が空でも HMAC は可能）、label は C 文字列（長さの上限は設けない。streaming で hash する）、out は 1024 以下。`sslc_tls12_master` の pre_master は 1 byte 以上、extended_master_secret は 0 か 1 だけ（他は EINVAL）、context の大きさは ems が hash の大きさ、通常が 64。`sslc_tls12_key_block` は suite の version が TLS 1.2、hash が 256/384、key_size 1〜32、fixed_iv_size 1〜12、client_keys と server_keys が同じ構造体なら EINVAL、出力は先に全部 0 にしてから書く。`sslc_tls12_finished` の from_client は 0 以外が client。
7. 戻り値: header が EINVAL と書く所は全部 EINVAL（下の cryc_ 関数の ENOTSUP は EINVAL に直す）。
8. 内部は `sslc_keysched_hash_size`・`sslc_tls13_derive_secret`・`sslc_tls13_next_secret`・`sslc_tls12_p_hash` の static 4 つ。

phase.md の「注意」の指示との差: 1.2 の PRF の期待値は、試験側で HMAC を RFC 5246 5 の式どおりに組み（`ref_prf`: 連結した buffer に対する 1 回呼びの cryc_hmac）、実装（streaming の cryc_hmac_update で A(i)・label・seed を順に吸収）と別の形で書いた。RFC 8448 の text は参照せず、`rfc8448.vec`（p025 で使った前例と同じ cache）を使った。

未実施: section 4（PSK・0-RTT）の transcript と schedule の通し（schedule が PSK 無しのため）、`sslc_tls13_update_secret` と 1.2 の関数の公式の vector（RFC 8446・5246 に値が無い。試験側の式との照合だけ）、32 bit の size_t の機械と aarch64 Windows の実行（check-skeleton の build のみ）、実機・QEMU（範囲外）、規約の第三者のレビュー（§14 の checklist で自分の diff を見直した）。

commit: 5d5c591da5（実装・試験・結果。この行の追記だけが次の commit）。

