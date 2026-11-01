<!-- awesome-plan project=zedbsd record=ws335-p016 -->

# ws335-p016: DER の reader と PEM・base64

Status: cleared（2026-11-02 01:47 Q1、main 03a1ab446。main で test-der 1967・test-pem 666 PASS。bundle 121 枚・limbo 19,587/19,589 枚が読める）。前: 実装済み・試験 PASS（2026-11-02 01:40 Sonnet、q1877-i01。cleared は Q1 が決める。起動は Q1。依存: p002）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-der.h`、`userland/base/libcrypto-compat/cryc-pem.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - `itu-t-x690-202102.pdf`（SHA-256 `6ac799a2d1ebed36abe39c9eac3586185bfb7db679f801e737189d6fdb1bbd6c`、印刷の頁 N は PDF の頁 N+8）: 8.1〜8.3、8.6、8.19、10.1、10.2、11.1、11.2。
  - `itu-t-x680-202102.pdf`（SHA-256 `b69dc4754f0709a7e344c886f277ed9e37a4336029593ba91bc6df6065f9ee14`、印刷の頁 N は PDF の頁 N+12）: 8.4 Table 1。
  - `rfc5280.txt`（SHA-256 `a2f2628c0a83b873fc4786abd921f9b2c02395954b655d190bf16b831633345d`）: 4.1.2.5（時刻）。
  - `rfc7468.txt`（SHA-256 `0b2c3c2087cc0b099789c90e61c0208e87b25793f0ce40090979e8c734b3d989`）: 2、3、5.1。
  - `rfc4648.txt`（SHA-256 `84e14418f795d503be5f34bf23ce4ebaa119e9ec7c9f667d8caeb111385b178f`）: 3.5、4。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-der.c`、`userland/base/libcrypto-compat/cryc-pem.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-der.c`、`plan/ws335/tests/unit/test-pem.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 壊れた入力は derwriter.h（試験の道具）で作る（長い形の長さ、不定長、高い tag、残りより長い長さ、最小でない INTEGER、unused bit）。
- ca-certificates の bundle（/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem、SHA-256 f66dff1b…）を全部読めること。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh der pem     # この module
sh plan/ws335/tests/run-unit.sh --win32 der pem     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 01:40 JST）Sonnet 5.5 high（q1877-i01）。worktree s-ws335-p016（main 400efaaee）。

作った物: `userland/base/libcrypto-compat/cryc-der.c`、`userland/base/libcrypto-compat/cryc-pem.c`、`plan/ws335/tests/unit/test-der.c`、`plan/ws335/tests/unit/test-pem.c`。header・試験の道具・他の module は変えていない。ct の試験（run-ct）はこの Phase に無い（秘密を扱わない）。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh der pem` | `test-der: PASS (1967 checks)` / `test-pem: PASS (666 checks)` / `run-unit: PASS`（ASan・UBSan、約 16 秒） |
| `sh plan/ws335/tests/run-unit.sh --win32 der pem`（x86_64、Wine） | `test-der: PASS (1967 checks) (win32)` / `test-pem: PASS (666 checks) (win32)` / `run-unit: PASS`（bundle も Wine の Z: 経由で読めた） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（warning 0、cc・clang・zedBSD の clang・llvm-mingw の 2 arch、名前の範囲） |
| `git diff --check` | 出力なし |
| `sh plan/ws335/tests/run-unit.sh all` | `run-unit: PASS`（全 15 本の test と 9 本の ct-、posix。ct- は他 module の既存の試験） |

試験の中身:

- test-der（check 1967。うち数百万回の反復は 1 つの check にまとめた）: 長さの全形（0・1・2・126〜129・254〜257・65535〜65537・100 万・0x00fffffe・0x00ffffff の minimal な形と views、0x01000000 は 16 MB の実体があっても EINVAL、5・8 octet の長さ、0xff、0x80、long form の leading zero を内容が揃った状態で）、厳密な prefix の全切り（EINVAL・reader 不動・element 零）、tag（high form 全 class、universal の constructed 形の拒否、primitive の SEQUENCE・SET の拒否、context・application・private）、expect・optional（壊れた present は EINVAL）・enter・空の reader、INTEGER（手書き表と、writer で作った乱数 400 本の往復）、small INTEGER（max の端・ERANGE の順序）、BOOLEAN（256 値のうち 0x00・0xff だけ）、BIT STRING（unused 0〜7 × 末尾 byte 256 通りを全部、規則は `last % 2^unused == 0`）、OID（実 OID・1〜3 octet の全 16.8M 通りを仕様の規則を書いた oracle と比較・乱数）、time（手計算の epoch 約 30、1970 から 9999-12-31 と 1969 から 0000-01-01 まで全 3,652,425 日を、epoch から前後に歩く暦の counter で GeneralizedTime、1950〜2049 は UTCTime も、時刻は日ごとに変える。月・日・時・分・秒の範囲外、leap second、全桁位置への非数字、長さ 0〜20、tag 違い、`z`・offset・fraction・秒なし）、random（30000 本の乱数と 4000 本の mutated な実証明書を再帰で歩き、view が入力の内側にあることと、reader が必ず前進することを確認）。
- 外の corpus: ca-certificates bundle（cacert-2026-08-13.pem）の 121 枚は全部 Certificate → tbs（version・serial・alg・issuer・validity の 2 time・subject・SPKI・[1][2][3]・extension の列）を最後の byte まで walk できた。x509-limbo の 9,792 件の peer・trusted の 19,589 枚のうち 19,587 枚が walk できた（残り 2 枚は `rfc5280::serial::negative`、serial が負で cryc_der_integer が header の約束どおり拒む）。Wycheproof の ECDSA 4 file の署名（valid は全件 parse、InvalidEncoding・BerEncodedSignature・InvalidTypesInSignature・MissingZero の flag は全件 parse に失敗）、publicKeyDer 331 本（ECDSA・Ed25519・RSA）が SubjectPublicKeyInfo として最後まで読め、RSA 9 本は modulus が file の値と一致した。
- test-pem（check 666）: RFC 4648 §10 の 7 件、64 文字の全値、256 byte 値 × 8 位置、URL alphabet の拒否、padding の誤り 29 通り、末尾 2 文字・3 文字の全組（4096＋262144 通り、RFC 4648 3.5 の 0 bit の規則を全部）、空白（4 種を全位置、他の control・0x80 以上・NUL は拒否）、room（0〜40 byte の exact と 1 byte 不足、不足時に room 以降を触らない）、乱数の往復（0〜39 byte 全部、wrap 0・4・5・64・76・1000 × 改行 5 種）、200,000 本の乱数文字列を参照の判定規則と比較（受理したものは再 encode で同じ文字列）、1 MiB の往復。PEM は BEGIN・END の形（LF・CRLF・CR 単独・末尾なし・BEGIN の後の空白・body の改行・空行・1 文字ごとの改行）、explanatory text、lookalike 約 20 通り（行頭でない・後に文字がある・dash の数・小文字・他 label・X509 CERTIFICATE・TRUSTED CERTIFICATE など）、壊れた block 約 25 通りの error と position、複数 block の順次読み・他 label の読み飛ばし・壊れた block の後の再開、position の途中開始、RFC 7468 の Figure 6・7 の証明書（560・413 byte、1 個の SEQUENCE に収まる）、40000 本の乱数 fragment の連結（終わること・position が進むこと・契約）、Wycheproof の PUBLIC KEY の PEM 1132 本（他 label として ENOENT、base64 が file の DER と一致）、limbo の 9,792 枚を試験側の encoder で PEM にして 4 種の wrap・改行で読み戻し、bundle の 121 枚を strstr で別に切り出して再 encode した文字列と照合。
- 試験の側の道具: base64 の encoder は RFC 4648 §4 から、暦の oracle は epoch から歩く counter で、どちらも reader の code を使わない。期待の値は仕様・手の計算・各 file の flag だけから作った。

わざと壊す確かめ（git の外。`build/tmp/p016/brk.py`、.c を `build/tmp/p016/brk/m<N>/` に複写して書き換え、その module だけを build して試験を流す。実装 file は触らず、`git status` で変化が無いことを確認）。判定は build の誤り（BUILD-ERROR）、試験の失敗（`test-NAME: FAIL (...)` の最後の行）、sanitizer・crash（FAIL の行が無い異常終了）、見逃しを区別した。der 78 通り: 試験の失敗 60、sanitizer の異常終了 14（出力側の check が落ちる前に ASan・UBSan が範囲外の読み書きを検出）、見逃し 4（等価）、build の誤り 0。pem 48 通り: 試験の失敗 42、異常終了 2（`emit` の誤りは segfault、`free(buffer)` の脱落は ASan の leak）、見逃し 4（等価）、build の誤り 0。

- 見逃し 8 通りは等価か冗長で、観測できる違いが無い: der の (1) 長さ octet が `first <= 0x80` の `<`（0x80 は直前の indefinite の判定で処理済み）、(2) 長さ octet が 0xff の個別の判定を 0xfe にしても、octets が 127 で 4 を越え EINVAL になる（冗長）、(3) BOOLEAN で `contents[0] == 0xff` を `!= 0x00` にしても、その時点で値は 0x00 か 0xff だけ（等価）、(4) BIT STRING の「unused あり・bytes 無し」の個別の判定を消しても、後の「unused bits が 0」の判定が unused の値自身を最後の byte として読んで必ず非 0 になり EINVAL（冗長）。pem の (5) END の無い block の判定を消しても、`pem_prefix_matches` が範囲外を拒んで EINVAL（冗長）、(6) `from >= size` を `>` にしても、`from == size` は検索が空で ENOENT・position = size（等価）、(7) `room` の `+ 3U` の脱落は、復号 byte 数が常に文字数 / 4 × 3 以下（余りの slack で等価）、(8) 検索の再開位置を `line_end` でなく `begin + 1` にしても、同じ行の中に行頭は無い（等価）。
- 最初の実行で見逃した 8 通りは試験を足して塞いだ: long form の leading zero は内容が揃った状態で（先の case は内容が足りず別の理由で EINVAL だった）、5・8 octet の長さ、`used > room` の脱落（room を越えた後の group が出力を書く。room 0〜11 の全部で room 以降が無傷）、BEGIN の前の CR 単独の改行、OID・BIT STRING・INTEGER・BOOLEAN の空の内容の範囲外の読み（内容を exact-size の heap に置き ASan に見せる。size 0 の buffer は 0 で埋める）。無限の loop が出力を溜めて host の memory を使い切らないよう、試験の loop に上限（32 回）を足した。
- 試験の失敗と build の誤りの区別: 全 126 通りで BUILD-ERROR は 0（最初の実行は include の path の誤りで全部 BUILD-ERROR になったので直して流し直した）。

自分で決めた点（header が黙っていた所）:

1. `element->tag` は最初の identifier octet そのまま。universal の型で constructed の形が許されないもの（BOOLEAN・INTEGER・BIT STRING・OCTET STRING・NULL・OID・文字列・時刻など、SEQUENCE と SET 以外の全 universal）の constructed 形は EINVAL（header の「string types は primitive」を、DER で常に primitive の型にも広げた）。primitive の SEQUENCE・SET も EINVAL。context・application・private class は形を問わない。tag number 31 以上（0x1f の bit が全部立つ）は class と形を問わず EINVAL。
2. 長さ: 1 octet の short form、long form は 1〜4 octet（5 以上・0xff は EINVAL）、先頭 octet が 0 なら EINVAL、128 未満の long form は EINVAL、0x80（indefinite）は EINVAL、`CRYC_DER_MAX_LENGTH` を越えれば EINVAL。
3. 失敗時の出力: `cryc_der_next`・`cryc_der_expect` は element を 0 に、reader を動かさない。`cryc_der_optional` は不在のとき element を 0 に、present を 0 に、reader を動かさない。`cryc_der_enter` は失敗時に inner を空（NULL・0）にする。`cryc_der_integer`・`cryc_der_bit_string` は失敗時に出力を NULL・0 にし、`cryc_der_small_integer`・`cryc_der_boolean`・`cryc_der_time` は失敗時に 0 にする。
4. `cryc_der_integer`・`cryc_der_small_integer`・`cryc_der_boolean`・`cryc_der_bit_string`・`cryc_der_oid` は tag を見ず内容だけを見る（implicit tag の INTEGER・BIT STRING に使うため）。`cryc_der_time` は tag を見る（header が「another tag は EINVAL」と書いている）。
5. `cryc_der_small_integer`: 先頭の 0x00 を除いた大きさが 5 byte 以上なら ERANGE（max がいくつでも）、max 以下は成功（max を含む）。
6. `cryc_der_bit_string`: bytes は contents + 1（bytes が 0 でも contents + 1 を指す）。
7. `cryc_der_oid`: subidentifier の大きさの上限は見ない（header が求めていない）。
8. base64: 空白（space・tab・CR・LF）は padding の途中も含めどこでも読み飛ばす。空の text・空白だけの text は 0 byte の成功。`=` は group の 3・4 文字目だけ、`=` の後は空白以外が来たら EINVAL（次の group も）。room 不足でも読み進めて EINVAL を優先し、EINVAL が無ければ ENOBUFS（書かずに数える。*written は 0、room 以降の byte は触らない）。
9. PEM: BEGIN 行は text の先頭か CR・LF の直後にあり、`-----BEGIN ` ＋ label ＋ `-----` の後は空白・tab だけで行末（それ以外は explanatory text として読み飛ばす）。label は `CERTIFICATE` だけ（X509 CERTIFICATE・TRUSTED CERTIFICATE ほかは読み飛ばし）。END は BEGIN 行の後の最初の `-` で、`-----END CERTIFICATE-----` と正確に同じでなければ EINVAL（base64 に `-` は無い）。END 行の前に改行が無くてもよい。BEGIN の後 body が空（空白だけを含む）なら EINVAL。END の後は空白・tab と 1 個の行末（LF・CR・CR LF）を読み飛ばした所が *position。EINVAL のとき *position は BEGIN 行（とその行末）の次。position が size 以上なら ENOENT で *position = size。
10. static の補助の分け方（`der_tag_is_valid`・`der_read_length`・暦の補助 4 つ、`base64_value`・`base64_is_space`・`pem_find_begin`・`pem_skip_line_end`・`pem_prefix_matches`）は自由に決めた。暦は 0000-01-01 からの日数の閉じた式（`365 * y + (y+3)/4 - (y+99)/100 + (y+399)/400`）。

header と phase.md の食い違い: header の cryc_der_boolean の comment は「length other than 1」だけだが、tag を見ない。header の `cryc_der_small_integer` の ERANGE は「max を越える」だけで、5 byte 以上の大きさを含むと解釈した。他は見つからなかった。

未実施: vectors.py が変換しない形式の vectors は使っていない（PEM の例は RFC 7468 Figure 6・7 を出典の comment 付きで試験に data として置いた、alphabet は RFC 4648 Table 1 を出典の comment 付きで置いた。他に data は無し）、run-ct（この Phase は秘密を扱わない）、32 bit の size_t の機械、aarch64 Windows の実行（build のみ）、性能の測定、実機・QEMU（範囲外）。

### Q1 の記録（2026-11-02 01:47）

- 破壊の確かめの 1 回目で、試験の無限 loop の失敗の出力を python が溜め、数分 100 GB 超の memory を使った（host の used 130 GB、kill で解放、他への害は見ていない）。以後の破壊の確かめは、試験の loop に上限を付け、出力を file に書き、size と memory に上限（ulimit など）を付けて流す。

