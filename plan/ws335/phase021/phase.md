<!-- awesome-plan project=zedbsd record=ws335-p021 -->

# ws335-p021: 誤りの queue と ERR_・RAND_bytes・OPENSSL_cleanse・CRYPTO_memcmp

Status: cleared（2026-11-02 03:44 Q1、main で test-err・ct-err PASS。依存: p015）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。標準廉価実装手順 §5（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md)）。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7・§9・§10、[plan/coding-style.md](../../coding-style.md) の全文、[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- 設計: [design.md](../design.md) の §3・§6・§9。
- 実装する header（仕様の正。変えない）: `userland/base/libcrypto-compat/cryc-err.h`、`include/libc/compat/crypto/err.h`、`include/libc/compat/crypto/rand.h`、`include/libc/compat/crypto/crypto.h`。header の共通の約束は `userland/base/libcrypto-compat/cryc.h` の冒頭（誤りの値・定数時間・消去・C89）。
- 仕様（git の外。共有の置き場所 `/home/awe/zedBSD-claude1/build/sources/specs/tls/`、無ければ p001 の担当の worktree の `/home/awe/zedBSD-worktrees/p6-ws335/build/ws335-specs/`）:
  - OpenSSL 3.5 の API の文書（互換の振る舞いの正）: /home/awe/zedBSD-claude1/build/distfiles/openssl-3.5.8.tar.gz（SHA-256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2、展開は自分の build/tmp の新しい dir に。`doc/man3`・`doc/man7` の .pod は振る舞いの仕様として読み、文・code を写さない）: doc/man3/ERR_get_error.pod、ERR_error_string.pod、RAND_bytes.pod、OPENSSL_malloc.pod（OPENSSL_cleanse）、CRYPTO_memcmp.pod。
  - 読み方: `pdftotext -f N -l M -layout`、表の値は頁の画像（Read の tool で PDF の頁）でも照合する。text は `sed -n`。
- 試験の道具（変えない）: [run-unit.sh](../tests/run-unit.sh)、[check-skeleton.sh](../tests/check-skeleton.sh)、[unit/check.h](../tests/unit/check.h)、[unit/vecfile.h](../tests/unit/vecfile.h)、[unit/derwriter.h](../tests/unit/derwriter.h)、[unit/ct.h](../tests/unit/ct.h)・[run-ct.sh](../tests/run-ct.sh)。

## 2. 作る物

- 実装: `userland/base/libcrypto-compat/cryc-err.c`、`userland/base/libcrypto-compat/api-err.c`（骨格の ENOSYS の本体を置き換える。static の補助関数は足してよい）。
- 単体試験: `plan/ws335/tests/unit/test-err.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

- 正常: 仕様の例と公式の vector の全件（件数を結果に）。境界: 長さ 0・1・block の境・最大。誤りの入力: 各関数の comment の誤りの値の全部の条件。途中で切れた入力と乱数の入力（ASan・UBSan で落ちず、決めた誤りを返す）。往復（encode と decode、seal と open）。
- 期待の値は仕様の表・式・付録・公式の vector と手の計算だけから作る。他の実装（OpenSSL・BoringSSL・mbedTLS・libsodium・Go ほか）の出力を使わない。
- わざと壊す確かめ（必須）: 試験が通った後、実装を git の外で数通り壊し（定数の 1 つ、比較の向き、境界の off-by-one、mask の反転）、試験が FAIL することを確かめ、戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- queue は thread ごと（cryc_os_thread_*）。満杯は古い物を捨てる。memory が無い時は黙って捨てる。
- 文字列の形は "error:%08lX:<library>::<reason>"。
- Windows: 誤りの queue は cryc_os_thread_value（FLS）に載る。`run-unit.sh --win32 err` で、thread ごとに別の queue である事（ある thread で積んだ誤りが別の thread の `ERR_get_error` に見えない事、新しい thread の queue は空）を Wine で確かめる。Windows の試験の thread は `#ifdef _WIN32` の下で CreateThread か `_beginthreadex`（`--win32` は pthread を link しない）。thread の終わりの解放は API から見えないので、多くの thread（1000）を作って終わらせても失敗しない事で見る。
- 他の実装（OpenSSL・BoringSSL・LibreSSL・mbedTLS・wolfSSL・libsodium・BearSSL・Go の crypto・Rust の crypto ほか）の code・comment・構造・内部の名前を写さない。表・定数は data として block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> <節・表>」。file の先頭に出典を書かない。source に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。
- Windows（static link）: 同じ source が llvm-mingw で Windows の static library になり、OmniGrammaton の 1 つの .exe に link される（[design.md](../design.md) D23・D25）。`long` は Windows で 32 bit（LLP64）なので、32 bit を越える値に `long`・`unsigned long` を使わない（`uint64_t`・`size_t`）。static でない関数・object の名前は公開の名前か `cryc_`・`sslc_` で始める（check-skeleton の 7 が照合）。試験で binary の file を読む時は `fopen(…, "rb")`（Windows の text mode を避ける）。
- header が黙っている内部の事（static の補助の分け方、内部の表の形、loop の順など、interface に出ない事）は自分で決め、結果の節に列挙する。interface の不備（足りない引数、矛盾する comment、header だけでは決められない約束）は §5 のとおり止めて返す。phase.md が仕様と食い違えば仕様に従い、報告する。

## 5. 変えてはいけない物

- 全ての header（`userland/base/libcrypto-compat/*.h`・`userland/base/libssl-compat/*.h`・`include/libc/compat/crypto/*.h`・`include/libc/compat/ssl/*.h`）、他の module の .c、Makefile と build の規則、試験の道具（§1 の一覧）、HAL・toolchain・共有の `build/`。
- header の不備（足りない引数、誤った comment、決められない約束）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。

## 6. 確かめの command

```
sh plan/ws335/tests/run-unit.sh err     # この module
sh plan/ws335/tests/run-unit.sh --win32 err     # 同じ試験を Windows（x86_64、static link、Wine）で
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

2026-11-02（host の `date` は 03:13 JST）Sonnet 5.5、Queue q1905-i01。worktree s-ws335-p021（main 641a2f195）。実装と試験の commit は fd11533454（以降の記録だけの commit は Q1 への報告に記す）。

作った物: `userland/base/libcrypto-compat/cryc-err.c`、`userland/base/libcrypto-compat/api-err.c`、`plan/ws335/tests/unit/test-err.c`、`plan/ws335/tests/unit/ct-err.c`（CRYPTO_memcmp と OPENSSL_cleanse の秘密の扱い）。header・試験の道具・他の module・Makefile は変えていない。

実行した command と最後の行（`WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors`）:

| command | 最後の行 |
| --- | --- |
| `sh plan/ws335/tests/run-unit.sh err`（ASan・UBSan・LeakSanitizer） | `test-err: PASS (4590 checks)` / `run-unit: PASS` |
| `LC_ALL=C.UTF-8 sh plan/ws335/tests/run-unit.sh --win32 err`（x86_64・Wine） | `test-err: PASS (4587 checks) (win32)` / `run-unit: PASS`（3 件の差は 64 bit の long でだけ流す check） |
| `sh plan/ws335/tests/run-ct.sh err`（valgrind） | `ct-err: PASS (5 checks)` / `run-ct: PASS` |
| `sh plan/ws335/tests/run-unit.sh all` | 全 test と全 ct（12 分 37 秒）が PASS、最後の行 `run-unit: PASS`（test-err 4590・ct-err 5 を含む） |
| `BASE=641a2f195 sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS` |
| `git diff --check` | 出力なし |

`BASE=b7dfd3952` では `check-skeleton: FAIL`（`plan/ws335/tests/run-unit.sh` が b7dfd3952 から変わっている。main の p015 の Q1 の判断で Wine に LC_ALL を付けた変更で、この Phase の変更ではない）。BASE を基点の直後の 641a2f195（main のこの Phase の起点）にして PASS（header・試験の道具の変更が無いことの照合）。

試験の内容（posix 4590 checks、win32 4587 checks）:

- 符号の詰め方: 手計算した値（20 と 134 で 0x0A000086、6 と 163 で 0x030000A3、15 と common の MALLOC で 0x078C0100、最大の library と reason で 0x7fffffff、桁あふれの library・reason が field に切られる事、header の定数の値と library 番号の一致）。
- queue: 空・1 件・2 件の順・peek が消さない・clear・ちょうど 16 件・17 件（最古が落ちる）・1〜100 件の積み（輪の周回の全部）、20 万回の乱数の操作（put・get・peek・clear）を試験の側の参照の queue と照合。
- 文字列: 全 library の名前、名前の無い library は `lib(<n>)`、reason の無い text は `reason(<n>)`、common の reason 5 件が全 library で text、library ごとの reason 9 件が自分の library だけで text で他の 9 通りの library では数字（全組合せを `check_reason_scope` で）、hex の全 16 文字、`%08lX` の幅（8 桁未満の埋め・64 bit の long の 9〜16 桁）、0 と最大値。長さ 0 から全長+3 までの全部の切り方（前後の guard と、長さより先が変わらない事）。NULL の buffer は長さ 0 でも 10 でも書かない。
- 登録: 引数の NULL・count 0 は登録を使い切らない、登録後に SSL の text、text が NULL の reason は数字、無い reason は数字、SSL 以外の library は登録の text を使わない、同じ table・別の table の 2 回目は何も変えない。
- thread: 別の thread の queue は空で自分の分だけ、main の分は見えない、8 thread 同時、1000 thread を順に作って終わらせる（queue の解放を LeakSanitizer が見る。Windows は FLS の callback）。
- RAND_bytes: 長さ 0・NULL（成功）、1〜100000 の 32 通りの長さで前後の guard・4 通りの fill で書かれなかった byte が無い事、2 回の出力が違う事、64 KiB の byte の度数（各値 160〜352）、負の count と NULL の buffer は 0 と決めた error が queue に積まれる事と buffer が変わらない事、成功は queue に積まない。
- OPENSSL_cleanse: 長さ 0〜300 で zero 化と前後の guard、volatile で読む局所の配列。NULL と長さ 0 と、NULL と長さ（落ちない）。
- CRYPTO_memcmp: 長さ 0（NULL も）、同じ pointer、NULL と長さ（非 0）、長さ 1〜129 を 4 通りの alignment で、等しい物は 0、全 byte 位置（長さ 40 以上は 7 byte おき）× 8 bit の 1 bit 反転は非 0、長さの外の byte の違いは無視、符号 bit・0x00 対 0xff・最後の byte だけ・全 byte が違う。
- ct-err: 秘密の byte の CRYPTO_memcmp（等しい・先頭・中間・最後の違い）と OPENSSL_cleanse が valgrind の memcheck で分岐・番地に秘密を使わない。

期待の値は header の定数（OpenSSL 3.5.8 の err.h.in の値）と手計算だけから作った。他の実装の出力は使っていない。OpenSSL の doc（ERR_get_error.pod・ERR_error_string.pod・RAND_bytes.pod・CRYPTO_memcmp.pod）は振る舞い（最古から返す・peek は消さない・len までに切る・library の名前の data は crypto/err/err.c の ERR_str_libraries）の確認にだけ読んだ。

わざと壊す確かめ（git の外。`scratchpad/mut/mut.py`、1 通りごとに複写の `cryc-err.c`・`api-err.c` を書き換え、試験の `-fsanitize=address,undefined` の build と実行、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB か 600 秒で kill、4 並列。実装の file の sha256 が変わっていない事を最後に確認）:

- 159 通りのうち、数えない build の誤り 1（確かめるための空の変異）を除く 158 通り: 試験の check の FAIL 149、sanitizer の中断 10（`ERR_DECIMAL_DIGITS` を 6 にした stack の溢れ・NUL の代わりに 'x'・hex の loop 条件・register の NULL 検査・queue の free の削除・queue の初期化の削除ほか）、見逃し 3。build の誤りで落ちた変異（unsigned の `< 0`・未使用の変数）は書き直して再実行した。
- 変異の内容: 符号の shift・field の幅 4 通り、`%08lX` の幅 2 通り、queue の drop・index・count・first の進め・get と peek の入れ替わり・clear 各 2〜3 通り、文字列の `>=` の境・NUL の位置・hex の table と mask と shift・decimal の桁・library と reason の取り出しの mask・各 literal（"error:"・"::"・"lib("・"reason("・括弧）・registration の claim の条件・count・pointer の NULL・count-1・ready の削除・reason の matching の向きと library の条件、library と reason の全 table の行ごとに text の末尾に x を足す・先頭を削る・key を変える、RAND_bytes の境（count 0・負・NULL）・長さ−1・error の library と reason・成功の値・向き、CRYPTO_memcmp の 0/1 の反転・長さ−1・同じ pointer の比較、OPENSSL_cleanse の長さ±1・何もしない、api の関数の入れ替わり・長さ−1・code+1。
- 見逃しの理由: (1) `err_queue_make` で `cryc_os_thread_set_value` が失敗した時の `free(queue)` を外す（失敗させる手段が無い）、(2) `cryc_err_put` の malloc の NULL の検査を外す（malloc を失敗させる手段が無い。外すと NULL を書くので実際には落ちるが試験の中で起こせない）、(3) RAND_bytes で `cryc_os_random` が失敗した時の `cryc_wipe`（getrandom を失敗させる手段が無い。cryc_os_random 自身も出力を wipe する）。3 つとも実行できない経路。

自分で決めた点（header が黙っていた所。Q1 が確認する）:

1. 符号の作り方は `((library & 0xff) << 23) | (reason & 0x7fffff)`。library・reason は field に切る。
2. 文字列の書き込みは自前の出力器（`snprintf` は C89 に無い）。buffer が NULL か length が 0 は何も書かない。それ以外は NUL を必ず付ける。hex は大文字で 8 桁以上（`%08lX` と同じ）、unsigned long の全桁（64 bit の long の符号も全桁）。
3. library の名前が無い時は `lib(<n>)`（header は reason だけ `reason(<n>)` と書いた。OpenSSL の形に合わせた）。名前の data（unknown library・system library・digital envelope routines・PEM routines・x509 certificate routines・asn1 encoding routines・common libcrypto routines・SSL routines・random number generator）は OpenSSL 3.5.8 crypto/err/err.c の ERR_str_libraries の値で、block の comment に出典。PEM と NONE は cryc-err.h に定数が無いので数値を直に書く。
4. reason の text は自前の語（"out of memory" など 14 件）。common の reason（library 0 の行）は全 library で、library 固有の reason は自分の library でだけ。登録の text は SSL の library だけ。組み込みの text を先に引き、無ければ登録を引く。
5. 登録: 引数の NULL・count 0 は何もしない（登録を使い切らない）。最初の有効な呼び出しだけが table を取り、2 回目以降は（同じ table でも別の table でも）何もしない。`cryc_os_atomic_add` で claim と公開（読む側は +0 の加算で atomic に読む）。texts の要素が NULL の reason は text 無し（数字）。
6. queue は 16 件の輪（malloc した 1 つの構造体、thread の値）。thread の終わりに free（秘密を持たない）。clear は memory を残し空にするだけ。メモリが無い時と `cryc_os_thread_set_value` が失敗した時は黙って捨てる。
7. RAND_bytes: 負の count は PASSED_INVALID_ARGUMENT、NULL の buffer と count>0 は PASSED_NULL_PARAMETER を RAND の library で queue に積んで 0（header は refusal の時だけ「error queued」と書き、負の count は 0 とだけ書く。積んだ方が呼び出し側に有益と判断）。OS の refusal は INTERNAL_ERROR を積み、buffer を wipe して 0。count 0 は buffer が NULL でも 1。
8. OPENSSL_cleanse: NULL の memory は length が何でも何もしない。CRYPTO_memcmp: NULL と length>0 は非 0（`cryc_ct_equal` に任せる）、返りは 0 か 1（`1 - equal` の算術。分岐無し）。

phase.md と仕様の食い違い・header の不備: 見つからなかった。header の comment の持ち越し（規約の見直しの Phase へ）: (a) `cryc_err_string` の名前の無い library の書き方（`lib(<n>)`）、(b) `RAND_bytes` の負の count と NULL の buffer が error を積む事、(c) `cryc_err_register_ssl` の「最初の有効な呼び出しだけが効く」と count 0・NULL の扱い。header は変えていない。

未実施: zedBSD・QEMU・実機での実行（範囲外）、aarch64 Windows の実行（check-skeleton で build と link のみ）、実際の Windows での実行（Wine のみ）、`cryc_os_random` が失敗する経路と malloc の失敗の経路（失敗させる手段が無い）、複数 thread が同時に `cryc_err_register_ssl` を呼ぶ試験（claim の atomic を信頼。試験は 1 thread の呼び出しだけ）。
