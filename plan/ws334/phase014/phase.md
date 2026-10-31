<!-- awesome-plan project=zedbsd record=ws334-p014 -->

# ws334-p014 鍵の表: keys-table（表の data、旧 T1）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §4.5 の window の行（鍵の写し）、§8 の T1。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libkeiland/win32/keys-table.h`。それが include する header も読む。
4. 仕様: 置き場は `/home/awe/zedBSD-claude1/build/sources/specs/ws334/`（git に入れない。Q1 が置く）。
   - Microsoft Learn「Keyboard Input Overview」の「Scan codes」の表（HID の usage → Scan 1 Make）: `about-keyboard-input.html`（SHA-256 `8a90dc2dbef395c1ce7a817fcabe488e8b16ff50293b98188975db8423c991b0`、2026-11-01 取得、https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input）。表の Note 1〜6 も読む（Print Screen・Pause・Num Lock・LANG1・2 の Windows の報告の形）。
   - Microsoft「Keyboard Scan Code Specification」Revision 1.3a（2000-03-16）: `scancode.doc`（SHA-256 `eeeb53fe9871cab7022e9073b11694ff33c8d7dec2612825ebf1c8c5696e65f3`、`catdoc` で text に）。上の表の照合に使う 2 つ目の源。
   - USB HID Usage Tables 1.5: `hut1_5.pdf`（SHA-256 `f4ead217b01134f89199155ed616d4eeae9ad40a5f73dad9544002b592c07db3`、443 頁）の §10 Keyboard/Keypad Page（0x07）と §15 Consumer Page（0x0C）。usage の名前の確かめ。
   - Linux v6.12 の `drivers/hid/hid-input.c`（`hid_keyboard[]` の表、HID の usage 0x07 → evdev。SHA-256 `31f38838949d853a932735f2c6d76b4344b445f5d8550fd067c3a3cf88291ef9`）と `include/uapi/linux/input-event-codes.h`（SHA-256 `f9889a73d7d4f7e303d2d346a3622657169c5e8d7d354cb7a030ff47a7fdfdee`）。GPL の file だが値を data として使う（guardrail の規則: 値だけ、block ごとに「Values referenced:」、code・comment・構造・名前を写さない。evdev の名前 `KEY_*` は Keiland の公開の header が既に使う番号の名前としてだけ使ってよい）。Consumer の page の media の鍵（`0xE022` Play/Pause など）は `hid-input.c` の consumer の switch の値。

## 作る物

- `userland/desktop/libkeiland/win32/keys-table.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-keys-table.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- Microsoft の表の Keyboard/Keypad の全部の行（Scan 1 Make が書かれた行、0x00FF の ErrorRollOver を除く）と Consumer・Generic Desktop の行のうち hid-input.c が evdev を持つ物: scan（下位 8 bit）と extended（0xE0 の前置き）の組から期待の evdev を試験の表に書き、全部を確かめる。試験の表は実装の表と別の手順で作る（例えば試験は HID の usage の列を経て 2 段で引く形、実装は 1 段の表）。
- header の特別な場合の全部: 0x45 と VK_PAUSE（extended 0 と 1）→ KEY_PAUSE、0x45 と VK_NUMLOCK・他の VK → KEY_NUMLOCK、0x46 extended → KEY_PAUSE、0x46 非 extended → KEY_SCROLLLOCK、0x37 extended → KEY_SYSRQ、0x37 非 extended → KEY_KPASTERISK、0x54 → KEY_SYSRQ、日本語・韓国語の鍵（0x29・0x70・0x73・0x79・0x7B・0x7D・0x72・0xF2・0x71・0xF1・0x78・0x77）と 0x76 → KEY_F24（header: F24 と LANG5 が同じ make code で F24 を取る）、0x72・0xF2・0x71・0xF1 は extended 0 と 1 の両方で同じ。
- extended の有無で違う鍵の組（Enter と keypad Enter 0x1C、左右の Ctrl 0x1D、左右の Alt 0x38、矢印と keypad の数字 0x48 など、Insert・Delete・Home・End・PageUp・PageDown、keypad の / 0x35 と /、左右の Windows 0x5B・0x5C と International6 の 0x5C）。
- scan 0 → 0、表に無い scan（0x5A〜0x5F の空き、0xFF）→ 0、scan が 256 以上（呼び手の誤り）→ 0、extended が 2 は 1 と同じ。
- 全部の (scan 0〜255, extended 0・1) の 512 の組で返り値が 0 か 1〜0x2ff。

## 注意

- 表は data。block の前に「Values referenced: Microsoft Learn, Keyboard Input Overview, Scan codes table (retrieved 2026-11-01); USB HID Usage Tables 1.5 §10; Linux v6.12 drivers/hid/hid-input.c hid_keyboard[] (values only)」の複数行の comment。
- 値は 2 つの方法で照合する（手順 §5.3）: (1) html の表を `python3 -I` の自分の script（git に入れない）で読んだ一覧と、(2) `scancode.doc` の text と HUT の PDF の頁の画で数十件を手で。照合の弱い所（Note の付いた cell）を結果に書く。
- license: `hid-input.c` は GPL-2.0。そこから取るのは HID の usage → evdev の番号の事実の値だけ（Q1 の確認、2026-11-01）。表の並べ方・comment・関数と変数の名前・code の構造を写さない。`input-event-codes.h`（GPL-2.0 WITH Linux-syscall-note）も番号の値だけ。試験の期待の値の 2 段目（usage → evdev）も同じく値だけを書く。
- 表の形は自由（256 × 2 の配列など）。1 つの関数が pure であること。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh keys-table` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
