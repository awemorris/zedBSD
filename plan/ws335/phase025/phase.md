<!-- awesome-plan project=zedbsd record=ws335-p025 -->

# ws335-p025: TLS の定数・suite の表・presentation language の reader/writer

Status: 実装済み・試験 PASS（2026-11-02 Sonnet q1878-i01。cleared の判定は Q1）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libssl-compat/sslc-tls.h`、`userland/base/libssl-compat/sslc-buf.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `rfc8446.txt`（SHA-256 `47871bc8820a2c3b6ea89f061055577058862cf543686b82d10131239702b3bd`）: 3、4.1.3（HRR の random、downgrade の印）、B.1〜B.4。
  - `rfc5289.txt`（SHA-256 `4bc7e5ae5b8a9e8c022a85530989593ba95bff2d2153aaf2aa5c017f2bd1410b`）: 3。
  - `rfc7905.txt`（SHA-256 `afa1e233822c6bda1f1ced014d32e6d740454415b53130fa84630a9c5ad2fe69`）: 2。
  - `rfc5746.txt`（SHA-256 `fb894a464bffd123ef7be802b63d7309b7af2bc34091683873c311556de2036d`）: 3.3。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libssl-compat/sslc-tls.c`、`userland/base/libssl-compat/sslc-buf.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-tls.c`、`plan/ws335/tests/unit/test-buf.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- sslc_tls_hrr_random などの値は data（出典）。
- writer の誤りは errno の値を持つ（ENOBUFS・EINVAL）。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh tls buf     # この module
sh plan/ws335/tests/run-unit.sh --win32 tls buf     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 00:39 JST）Sonnet 5.5（q1878-i01）。worktree s-ws335-p025（main 400efaaee0）。

作った物: `userland/base/libssl-compat/sslc-tls.c`、`userland/base/libssl-compat/sslc-buf.c`、`plan/ws335/tests/unit/test-tls.c`、`plan/ws335/tests/unit/test-buf.c`。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh tls buf` | `test-tls: PASS (2230 checks)` / `test-buf: PASS (1559755 checks)` / `run-unit: PASS`（ASan・UBSan） |
| `sh plan/ws335/tests/run-unit.sh --win32 tls buf`（x86_64、Wine、static link） | `test-tls: PASS (2230 checks) (win32)` / `test-buf: PASS (1559755 checks) (win32)` / `run-unit: PASS` |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `sh plan/ws335/tests/run-unit.sh all` | test-* 15 本（この Phase の 2 本を含む）と ct-* 10 本の計 25 本が全部 PASS、FAIL 0 / `run-unit: PASS` |
| `git diff --check` | 出力なし |

（test-buf の件数は 1559738 から、prefix 4 の見逃しを直した後の 1559755 になった。win32 も直した後に流し直し、表の件数のとおり。）

`run-ct.sh` は未実施（この Phase に秘密を扱う関数は無く、定数時間の対象は無い）。

実装の中身:

- `sslc-tls.c`: 表の値は block ごとに出典（RFC 8446 4.1.3 の HRR の random と downgrade の 2 つ、B.4・5.3・RFC 5289 3・RFC 7905 2・RFC 5288 3 の suite の表）。suite は 9 行の const 配列の線形探索。`sslc_tls_offered_suites` は優先順の別の配列（1.3: 1301・1303・1302、1.2: ECDSA の 3 つ（c02b・cca9・c02c）、RSA の 3 つ（c02f・cca8・c030））を順に引き、`sslc_tls_suite` の version が [min, max] に入る物だけを room まで書く。
- `sslc-buf.c`: reader と writer を header の comment どおりに。内部の補助は static の `sslc_reader_integer`・`sslc_writer_integer`・`sslc_writer_fail`。

試験の中身:

- test-tls（2230 checks）: 全 macro の値を RFC の表の 2 byte 表記（`{0xC0,0x2B}` など）から別に書いた値と照合（content type・handshake type・alert 全部・group・scheme・extension・SCSV・record の上限）。hrr の random は RFC 8446 4.1.3 の 32 byte と、libcrypto-compat の SHA-256 で出した "HelloRetryRequest" の digest の両方と一致。downgrade の 2 つは "DOWNGRD" + 01/00。suite の表は 9 行の全 field（id・version・aead・hash・auth・key_size・fixed_iv_size・name）を RFC から書いた値と照合、65536 個の番号の全部を引いてちょうど 9 つだけが suite であること、CBC・CCM・RSA 鍵輸送・DHE・SCSV などの番号は NULL。offered は版の組 13 通り（min > max や範囲外の空を含む）× room 0〜件数+2 の切り詰め、out NULL、順序（1.3 の後に 1.2、ECDSA の後に RSA、AEAD の順）、重複無し、SCSV 無し、exact-size の heap block への書き込み。RFC 8448 の ClientHello の suite の並び（13 01 13 03 13 02）と 1.3 の順が一致。
- test-buf（1559755 checks）: RFC 8446 3.3 の例（01 02 03 04 = 16909060）、各 width × 境界の値（0・1・0x7f・0x80・0xff・… 0xffffffff）を手で作った byte で、連続 read の left、短い入力（width 1〜4 × 長さ 0〜width-1）の 0 とエラーと sticky（残りの byte も読まない）、`bytes`（size 0・ちょうど・1 超過・`(size_t)-1`・NULL の空 reader）、vector（RFC 8446 3.4 の `mandatory<300..400>`・`longer<0..800>` の長さ 15 通り、prefix 1・2・3 × 範囲の両端、length が残りを越える・最大の length field・prefix の途中切れ・prefix 0〜6・失敗後の vector・min > max、入れ子と、inner が outer の byte を読めないこと）、finish。writer は低 bit の切り詰め、room の境（0〜）で ENOBUFS と「失敗した書き込みは何も書かない」、guard byte で範囲外の書き込み無し、`(size_t)-1` で wrap しない、vector の 1・2 byte prefix の 255/256・65535/65536 と 3 byte prefix の 2^24-1/2^24（16 MiB の block を 1 回作る）、深さ 6 と 7、begin の prefix 0・4・room 不足、end の空の stack、開いたままの finish、最初のエラーが残ること（ENOBUFS の後・EINVAL の後）、server_name の入れ子を手で書いた形（RFC 8448 の `00 00 00 0b 00 09 00 00 06 'server'`）。
  RFC 8448 の ClientHello（196 octets）・ServerHello（90）・HelloRetryRequest（176）を RFC の text から取り、reader で読み（各 field を RFC の text から読んだ値と照合、拡張の type と長さの一覧）、writer で書き直して元と byte 単位で一致、全ての短い prefix の入力（outer の length を合わせた物も）が拒否されること、小さい room で ENOBUFS。
  乱数: reader（6000 入力 × 8 操作、深さ 3 の vector、モデルと全ての値・エラー・left・finish が一致）、writer（8000 run × 24 操作、guard 付き、モデルと byte・used・error・depth が一致）、往復（3000 の木）。モデルは header の comment から試験の中に別に書いた。

わざと壊す確かめ（git の外。`build/tmp/p025/brk.py`、2 つの directory を `build/tmp/p025/run-<名前>/m<番号>/` に複写して 1 つだけ書き換え、非 sanitizer の -O1 で test-tls・test-buf を build して流す。実装 file は触らず、前後の sha256 が変わっていない事を script が確認し、`git diff` も空）。98 通り（buf 63、tls 35）: 94 通りが試験の失敗（`FAIL` 78、SIGSEGV で落ちた 16。build の誤りは 0、BAD-MUTATION は 0）。見逃し 4 通りは全部等価:
- reader の `finish` の `position != size` を `position < size` に: position が size を越えない（等価）。
- writer の整数の書き込みで `& 0xff` を外す: `(uint8_t)` の cast が同じ事をする（等価）。
- `sslc_writer_fail` が最後のエラーを残す: 全ての書き込みが先に error で return するので呼ばれない（冗長な検査）。
- offered の優先順の配列に SCSV を足す: `sslc_tls_suite` が NULL を返して飛ばす（二重の防御）。
最初の実行（99 行）では `vector: prefix > 4`（prefix 4 を受け入れる）が見逃しだったので、test-buf に「全部 0 の 8 byte に prefix 0〜6」を足し、2 回目の全部の実行で 94 通りが検出になった。

自分で決めた点（header が黙っていた所）:

1. reader・writer のエラーの値: reader の error は EINVAL を入れる（`error` を非 0 の印と使う）。writer の error は errno の値で、room 不足は ENOBUFS、それ以外（長すぎる vector・深さ超過・begin の prefix が 1〜3 でない・対の無い end・NULL data で size > 0）は EINVAL。最初のエラーだけが残る（writer）。
2. `sslc_reader_init` と `sslc_writer_init` は NULL data で size（room）> 0 を、size 0（room 0）にして error を EINVAL にする。
3. `sslc_reader_vector` の prefix が 1〜3 以外は、outer の error を立てて EINVAL、inner は空で失敗。min > max は何も受けない。失敗の時も outer の position は進んでよい（後は sticky で何も読めない）。
4. `sslc_reader_bytes` で size 0・data が NULL の reader は NULL を返しエラー無し。data があれば、末尾でも非 NULL（position の指す所）。
5. writer: 失敗した書き込み（room 不足）は部分的にも書かない。`sslc_writer_end` は長すぎても depth を 1 減らす（error が残るので finish は error を返す）。begin は prefix の幅 → depth → room の順で調べる。
6. `sslc_tls_offered_suites` の out が NULL なら room に関わらず 0（数えるだけの呼び出しは無い）。優先順は、1.2 の中で ECDSA の 3 つの後に RSA の 3 つ（header の「ECDSA before RSA」を「全部の ECDSA の後に全部の RSA」と読んだ）。min > max は 0。
7. 試験で参照した RFC 8448（`rfc8448.txt`、SHA-256 `6564d1376d1ec744fc7a9993da15ebc1b9be361908b166091f47ef605c537fba`、共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`）は phase.md の「読む物」に無いが、design.md の資料の表にあり、公式の vector として使った。

header と phase.md の食い違い: 見つからなかった。header の `sslc_writer_end` の comment は「2^8, 2^16 or 2^24 bytes or more sets error」で、実装と試験はそのとおり（255・65535・16777215 が最大）。

別の確認: header の macro の値は RFC 8446・5246・7627・5746・8422 の text に対して python で照合した（content type・handshake type・alert 全 27 件と 5246 の no_renegotiation・extension・group・scheme・cipher suite の表。session_ticket の 35 は RFC 5077 でこの資料に無く、未照合。header の値をそのまま試験に写さず、RFC の表記から別に書いた）。

未実施: `run-ct.sh`（対象無し）、32 bit の size_t の機械・aarch64 Windows の実行（build のみ、check-skeleton）、実機・QEMU（範囲外）、RFC 5077 の session_ticket の値の照合、コードの規約の第三者のレビュー（自分の diff を §14 の checklist で見直した）。

commit: bb6b23d212（実装と試験の初版）、最終版は Q1 への報告に書く。
