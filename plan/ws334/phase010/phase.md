<!-- awesome-plan project=zedbsd record=ws334-p010 -->

# ws334-p010 storage の記録: records

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.5（特に §3.5.1〜§3.5.3 の space・key の約束・記録の形）、§2.5。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/include/browser/storage.h`、`userland/desktop/libbrowser/storage/records.h`、`userland/desktop/libbrowser/base/base.h（wb_buffer・wb_units）`。それが include する header も読む。
4. 仕様: FNV-1a 64 の定数（draft-eastlake-fnv の §2。値は header に書いた）。localStorage の version 1 の形は今の `userland/desktop/libbrowser/page/storage.c` の `storage_load`・`storage_save`（:731-890）を読み、同じ bytes を読めるようにする（code は写さず、形を header から書く）。

## 作る物

- `userland/desktop/libbrowser/storage/records.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-records.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- hash: "" → 0xcbf29ce484222325、"a" → 0xaf63dc4c8601ec8c、"foobar" → 0x85944171f73967e8（FNV の公開の試験の値。試験の中で手の計算でも確かめる）。
- key_text: 1 byte "a" → "61"、100 byte → 200 文字の 16 進、101 byte → 16 進の hash + "-0"、collision 3 の短い original → hash + "-3"。どの key も `wb_storage_key_valid` が 1。NULL・length 0 → EINVAL。
- key_valid: "0"・"0123456789abcdef-"・200 文字 → 1、""・201 文字・"A"・"g"・"/"・NULL → 0。
- cookie_original_key: domain・path・name の長さの前置き（"a.example"・"/"・"" と "a.example"・"/"・"x" が別の bytes）。
- cookie の encode と decode の往復: 全部の flag の組、空の value、時刻の負の値と INT64 の端、長い value（1 MiB）。decode が返す original が rebuild と一致。誤り: 違う magic、各長さの field を 1 ずつ大きくした物、最後の byte を 1 つ削った物と 1 つ足した物、未知の flag の bit → EINVAL で `*cookie` が 0。
- cache の往復: 空の content_type・etag・body、max_age -1 と 0 と大きな値、no_cache 0 と 1。誤り: url 空、max_age -2、no_cache 2、切れた・余った bytes → EINVAL。
- local: version 2 の往復（0 item・1 item・空の key と value・組でない surrogate の unit・1000 item）、each の順が入れた順、each が 9 を返すと 9 で止まる。version 1: 試験の中で magic の行と item の列を手で組み立てた bytes が読め、origin が NULL・0。誤り: 切れた item（key の unit の途中で終わる）→ EINVAL。
- 乱数の入力: 固定の種の擬似乱数で作った 10000 個の bytes の列（0〜4096 byte）を 3 つの decode に渡して、範囲の外を読まず（ASan）必ず終わる。正しい record の各 byte を 1 つずつ書き換えた物も同じ。
- sink: `wb_storage_sink_init` の後の `browser_storage_sink_append` の積み、合計が WB_STORAGE_RECORD_MAX を超える append → EFBIG とそれ以後の append も EFBIG、`wb_storage_sink_error` が EFBIG。NULL の sink → EINVAL。

## 注意

- 整数は全部 little-endian で、byte ごとに組み立てる（未定義の動作の shift をしない: 符号の無い型で、負の i64 は uint64 の 2 の補数を経て）。
- 長さの field の和は広い型（size_t の加算の前に残りの bytes と比べる）で確かめる。
- `browser_storage_sink_append` は公開の関数（`exports.map` への追加は p023）。sink の確保の上限は WB_STORAGE_RECORD_MAX。
- 今の `page/storage.c` の version 1 の読みは「切れた item までの item を残す」が、この module は EINVAL（header の通り。呼び手が全部を捨てる）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh records` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
