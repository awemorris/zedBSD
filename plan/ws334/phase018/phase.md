<!-- awesome-plan project=zedbsd record=ws334-p018 -->

# ws334-p018 CF_HTML: cf-html（旧 T2）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
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
- decode: encode の往復（fragment と URL が同じ）。仕様の文書の例の data（文書の例を試験に写す。Version 0.9 と 1.0）。StartHTML が -1 で StartFragment・EndFragment がある物。Fragment の offset が無く HTML の offset だけの物。改行が LF だけ・CR だけ。未知の name の行、`StartSelection`・`EndSelection` の行。誤り: Version の行が無い、offset が length を超える、end が start の前、数でない値、"-1" が End に、header だけで本体が無い → EINVAL と出力が NULL・0。
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
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
