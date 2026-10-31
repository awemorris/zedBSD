<!-- awesome-plan project=zedbsd record=ws334-p017 -->

# ws334-p017 UTF-16 と入力の写し: wide・input

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §4.4 の N2・N7、§4.5 の window と text input の行。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libkeiland/win32/wide.h`、`userland/desktop/libkeiland/win32/input.h`。それが include する header も読む。
4. 仕様: The Unicode Standard 15.0（または 16.0）の §3.9「Unicode Encoding Forms」（UTF-8 の表 3-7 の well-formed の列、UTF-16 の surrogate、「U+FFFD Substitution of Maximal Subparts」）。Microsoft Learn の `WM_CHAR`・`WM_MOUSEWHEEL`・`WM_MOUSEHWHEEL`（`WHEEL_DELTA`）・`GET_X_LPARAM`・`WM_KEYDOWN` の lParam の bit・`POINTER_INPUT_TYPE`。手元の file は無い（Unicode の文書は unicode.org の PDF、無料。必要なら Q1 に置き場を頼む）。

## 作る物

- `userland/desktop/libkeiland/win32/wide.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libkeiland/win32/input.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-wide.c`。
- 単体試験 `plan/ws334/tests/unit/test-input.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- `test-wide.c`: ASCII・2〜4 byte・surrogate の組の往復。組でない high・low（先頭・途中・末尾）→ EF BF BD。buffer の大きさで文字の途中で切らない（3 byte の文字の前で止まり consumed がその前まで、ERANGE）、NUL が必ず付く、size 1 は空の文字列と ERANGE（文字があれば）。NUL の unit で早く終わる。UTF-8 → UTF-16: 不正な列の maximal subpart ごとの U+FFFD（Unicode の §3.9 の表 3-8 の例: `61 F1 80 80 E1 80 C2 62 80 63 80 BF 64` → `0061 FFFD FFFD FFFD 0062 FFFD 0063 FFFD FFFD 0064`）、overlong・surrogate・F5 以上、length が文字の途中で終わる、ERANGE の時の count が要る数。`keiui_win32_char_unit`: BMP の文字、組の 2 つの unit、high の後に high（U+FFFD を返し新しい high が待つ）、low だけ（U+FFFD）、NULL の引数で 0。乱数の 10 万の列で往復が落ちない。
- `test-input.c`: wheel（120 → -60.0、-120 → 60.0、horizontal 120 → 60.0、30 → -15.0、0 → 0）。modifiers（各 bit、AltGr で CTRL・ALT が落ち SHIFT・SUPER が残る、未知の bit を無視）。button（5 つと 5 以上 → 0）。point（0x0010FFF0 → x -16・y 16 など、負の y、NULL の出力）。key flags（bit 16〜23・24・30・31 の組、全部 1、全部 0。repeated は bit 30 のそのままで、key の up でも 1 になりうる: header の通り）。wide の consumed: NUL の unit で早く終わった時、NUL の前までの数。

## 注意

- wide は Windows の `MultiByteToWideChar` を使わず ISO C で書く（host で試すため。header の通り uint16_t）。
- input の値の約束（60 px の 1 notch、向き）は header の通り。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh wide input` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
