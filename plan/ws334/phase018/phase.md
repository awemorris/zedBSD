<!-- awesome-plan project=zedbsd record=ws334-p018 -->

# ws334-p018 CF_HTML: cf-html（旧 T2）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-11-01 03:28、cleared は Q1）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §4.4 の N6、§8 の T2。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libkeiland/win32/cf-html.h`。それが include する header も読む。
4. 仕様: Microsoft Learn「HTML Clipboard Format」: `/home/awe/zedBSD-claude1/build/sources/specs/ws334/html-clipboard-format.html`（SHA-256 `9b765a33e3209f59934682a7fd55cc78bd04bbd64ccfd88b3d428731917717e5`、2026-11-01 取得、https://learn.microsoft.com/en-us/windows/win32/dataxchg/html-clipboard-format）。git に入れない。Q1 が置く。

## 作る物

- `userland/desktop/libkeiland/win32/cf-html.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-cf-html.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- encode: 空の fragment、"<b>x</b>"、日本語の UTF-8、SourceURL の有無で、出力を header の形に手で組み立てた bytes と比べる（offset の 10 桁の値を手で数える）。size が要る長さちょうど（+1 の NUL）は成功、1 足りないと ERANGE と空の文字列と out_length が要る長さ。URL に CR・LF → EINVAL。
- decode: encode の往復（fragment と URL が同じ）。手で組み立てた Version 1.0 の data（offset を手で数える。仕様の文書の例は offset が bytes の位置と合わないので写さない。写した時の期待は、header の「header は最小の offset で終わる」で Version が切れて EINVAL）。StartHTML が -1 で StartFragment・EndFragment がある物。Fragment の offset が無く HTML の offset だけの物。改行が LF だけ・CR だけ。未知の name の行、`StartSelection`・`EndSelection` の行。誤り: Version の行が無い、offset が length を超える、end が start の前、数でない値、"-1" が End に、header だけで本体が無い → EINVAL と出力が NULL・0。
- 乱数: 正しい data の各 byte を変えた物と、乱数の 10 万の列を decode に渡して範囲の外を読まない（ASan）。

## 注意

- 仕様の文書の例の bytes は試験の data として使ってよい（文書の例。code ではない）。他の実装（Chromium・Firefox・Wine の clipboard）を見ない。
- POSIX の file（`*-posix.c`）と OS に依らない file に `#ifdef _WIN32`・`<windows.h>` を書かない（`check-skeleton.sh` が見る）。Windows の file（`*-win32.c`）は `<windows.h>`・`<winsock2.h>` などを include してよいが、W の API だけを使い（A の API を使わない）、`<jpeglib.h>` を include しない。
- 他の実装（Chromium・Firefox・Wine・curl・libuv・glib・Qt など）の code・comment・構造・名前を写さない。Win32・POSIX の API の使い方は API の reference から書く。
- source の comment に WS・Phase の番号・plan/ への参照・日付を書かない。各関数は coding-style の形（multi-line の定義、宣言の群、comment の付いた段落、成功の return を最後に）。
- 試験は header の約束を全部の誤りの場合まで確かめる。期待の値は header・API の reference・手の計算から作り、他の実装の出力を使わない。Windows の側の期待を Windows の API で独立に求めてよい（試験の file の中で、実装と別の道で）。
- 試験の file は `plan/ws334/tests/unit/test-<名前>.c`。`#include "check.h"` の `CHECK`・`CHECK_EQ`・`check_finish("<名前>")` を使い、`main(argc, argv)` の `argv[1]` が "posix" か "win32"。今の directory は試験ごとの空の directory で、file を作ってよい。

## 変えてはいけない物

- 全部の module の header（`plan/ws334/tests/modules.sh` の `ws334_headers` の全部）。header の不備・矛盾・足りない口を見つけたら直さずに止めて Q1 に返す（どの header のどの関数か、何が足りないか、案の差分）。header が黙っている所は自分で決め、「結果」に列挙する。
- 担当の外の `.c`（他の Phase の module、libbrowser・libmedia の既存の file）、`apps/browser-win32/Makefile`・一番上の `Makefile`、試験の道具（`plan/ws334/tests/modules.sh`・`run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/test-tools.c`）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。push しない。

## 確かめの command（worktree の root で）

- `timeout 1800 sh plan/ws334/tests/run-unit.sh cf-html` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

- 日付: 2026-11-01 03:28（host の `date`）。担当: Sonnet 5.5（phase-runner-low）。状態: 実装済み・試験 PASS（cleared は Q1）。
- 実装: `userland/desktop/libkeiland/win32/cf-html.c`（encode・decode と static の補助）。試験: `plan/ws334/tests/unit/test-cf-html.c`。header は変えていない。
- 実行した command と最後の行（`LC_ALL=C.UTF-8`）:
  - `timeout 1800 sh plan/ws334/tests/run-unit.sh cf-html` → `test-cf-html: PASS (155072 checks) (posix)`・`test-cf-html: PASS (155072 checks) (win32)`・`run-unit: PASS`。
  - `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → 全部の試験が posix・win32 とも PASS、`run-unit: PASS`。
  - `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（headers: 19、sources: 22）。
  - `git diff --check` → 出力なし。
- 期待の値: encode は header の形に従い行の長さを手で数えた bytes（空 = StartHTML 105・EndHTML 175、"<b>x</b>" = 105・183・139・147、URL "http://a/" 付き = 126・204・160・168 など）。decode の 64 の data は、offset を text の中の marker の位置から求めた（C の code と別の道、Python で生成して literal として貼った）。仕様の文書の例（offset が bytes の位置と合わない物）は EINVAL の試験にだけ使った（header が最小の offset で切れて Version が読めない）。
- 試験の内容: encode の exact bytes・size の境（要る長さ + NUL ちょうど / 1 足りない / 0）・CR/LF の URL・NULL の引数・9999999999 の限度と size_t の wrap（64 bit のみ限度の値）、decode の手作りの data（LF のみ・CR のみ・0 の詰め・-1・未知の name・Selection の行・SourceURL の重複・順序）、誤りの data（Version 無し・範囲外・end < start・数でない・-1・header のみ・offset が header の中）の EINVAL と出力の NULL・0、encode → decode の往復（NUL を長さに含む・含まない）、全ての短縮の長さ、全 byte × 256 値の変更、乱数 10 万（乱数の bytes・語の列・形の正しい header）。decode は毎回ちょうどの大きさの heap の block に複写して渡す（ASan）。
- 自分で決めた点（header が黙っている所）:
  - decode の header は 2 回読む: 1 回目で全ての offset の最小値を求め（無ければ data の長さ）、2 回目はその位置より前で終わる行だけを読む（最小の offset で header が切れる。位置にかかる行、改行の無い最後の行は読まない）。
  - header の行の name は ASCII の英数字・'-'・'_' の 1 文字以上。それ以外（HTML の `<a href="http:` など）の行で header は終わる。空行でも終わる。
  - Version の値は検査しない（行が在れば良い）。同じ name の行は最初の物を使う（offset も SourceURL も）。StartSelection・EndSelection は未知の name と同じに飛ばす（値も検査せず、header の切れにも使わない）。
  - offset の値は数字だけ（空白・'+'・'0x' は EINVAL）か "-1" ちょうど。'-1' は StartHTML と EndHTML の組でだけ許し（StartHTML が -1 で EndHTML が無いか -1）、EndHTML だけが -1、StartHTML が -1 で EndHTML が数、StartFragment・EndFragment が -1 は EINVAL。
  - 与えられた offset の誤りは飛ばさず EINVAL（Fragment の組が正しく HTML の組が誤りでも EINVAL）。組の片方だけの物は使わず無視する。start は data の中の byte を指す（start < length）。start == length は本体が無く EINVAL。end == start（空の fragment）は良い。
  - length に NUL を含む data で、EndHTML == length は良い（長さをそのまま上限にする）。
  - SourceURL の値は行の末尾まで（空白を削らない）。空の値は NULL でなく長さ 0 の pointer。source_url が NULL で source_url_length が NULL でない時も長さは書く。URL の行が header の中に無ければ NULL・0。
  - encode: 空の URL "" は "SourceURL:" の行を書く。EINVAL の時も buffer を空の文字列（size > 0）、out_length を 0 にする。buffer と out_length のどちらかが NULL なら何も書かず EINVAL。size 0 の ERANGE は buffer に触れない。
- 壊して確かめた（git の外、scratchpad の複写で、posix の ASan 版）: encode の offset を 1 つずらす（FAIL 56）、ERANGE の境 <（5）と <= +1（27）、空文字列にしない（24）、CR の検査を消す（9）、限度 +1（3）、URL 行の改行を消す（24）、桁の計算（54）、end < start の比較（6）、start >= length の比較（2）、offset の上限 +1（7）、"-1" の規則の 4 つの検査（各 3、初めは試験が足りず見逃した 3 つに case を足して捕まえた）、name の大小の区別（6）、Version の検査（6）、lone CR の扱い（ASan 中止）、最小の offset による切り（9、初めは見逃し。仕様の例と header の中の offset の case を足した）、SourceURL の最初の物（1）、fragment の優先（104）、name の '-' と '_'（6）、URL の長さ +1（18）、CR LF の範囲検査（中止）。最終は下の 2 つ以外の見逃し無し。
  - 見逃し（意図的に残した）: (1) 数字の桁あふれの検査（`value > (SIZE_MAX - digit) / 10`）は、value が毎回 length 以下に収まるので length が SIZE_MAX / 10 を超える data でしか効かず、試験できない（防御として残す）。(2) 行の終わりがちょうど最小の offset の位置にある時（`line_end >= limit` と `>` の違い）は、header が黙っている境で、どちらでも良いと判断し試験で固定しなかった。
- 未実施: 実機の Windows（Wine のみ）。aarch64 の Windows での実行（compile のみ、check-skeleton）。32 bit の size_t での試験。
