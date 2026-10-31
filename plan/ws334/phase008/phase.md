<!-- awesome-plan project=zedbsd record=ws334-p008 -->

# ws334-p008 時計と時刻帯: os-time

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p005・p009（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.4 の時刻の行、§2.2 の O17・O19・O20。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libbrowser/base/os.h`、`userland/desktop/libbrowser/base/os-win32.h`、`userland/desktop/libbrowser/base/os-time.h`、`userland/desktop/libbrowser/base/os-zones.h`。それが include する header も読む。
4. 仕様: POSIX.1-2024 の `clock_gettime`・`tzset`・`localtime_r`（`tm_gmtoff`・`tm_zone` は BSD・glibc の拡張）。Microsoft Learn の `QueryPerformanceCounter`・`QueryPerformanceFrequency`・`GetSystemTimePreciseAsFileTime`・`GetDynamicTimeZoneInformation`（`DYNAMIC_TIME_ZONE_INFORMATION` の `Bias`・`StandardBias`・`DaylightBias`・`TimeZoneKeyName`・`StandardName`・`DaylightName`）・`SystemTimeToTzSpecificLocalTimeEx`・`FileTimeToSystemTime`・`SystemTimeToFileTime`。手元に置く文書は無い。

## 作る物

- `userland/desktop/libbrowser/base/os-time-posix.c`（今は骨格。全部の関数を書く）。
- `userland/desktop/libbrowser/base/os-time-win32.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-os-time.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- monotonic: 100000 回の呼び出しで減らない。試験が 100 ms 眠る（posix `nanosleep`、win32 `Sleep`）前後の差が 90 ms 以上 2 s 未満。
- wall: `time(NULL)` との差が 2 s 未満、2 回の呼び出しで減らない（時計が変えられない限り）。
- posix の offset と名前: 試験が `setenv("TZ", ...)` と `wb_os_time_zone_start` で zone を変える。"UTC0" → 0 と "UTC"、"JST-9" → 32400 と "JST"、"EST5EDT,M3.2.0,M11.1.0"（POSIX の TZ の規則の文字列、tzdata の file に依らない）で 2024-07-01 12:00 UTC（1719835200）→ -14400 と "EDT"、2024-01-15 12:00 UTC（1705320000）→ -18000 と "EST"。WB_OS_TIME_FIRST より前の値は FIRST と同じ答え、LAST より後は LAST と同じ答え。
- win32 の offset: 試験が `GetTimeZoneInformationForYear`（その年の規則）と `SystemTimeToTzSpecificLocalTime` で独立に求めた offset（2000〜2030 年の 20 の時点）と一致（UCRT の `localtime_s` は今年の規則を全部の年に当てうるので使わない）。
- win32 の名前: 試験が `GetDynamicTimeZoneInformation` で key を読み、`wb_os_zone_find` が見つける key なら名前が standard か daylight のどちらか（offset が daylight の offset の時 daylight）、見つけない key なら system の StandardName か DaylightName の UTF-8。
- 誤り: offset NULL・buffer NULL・size 0 → EINVAL。名前の長さちょうどの size + 1 は成功、size が長さ以下 → ERANGE と空の文字列。
- Wine の zone は host の zone に従う（試験は zone に依らない形で書く。Wine で `TZ` を変えた確かめは任意で、結果に記録）。

## 注意

- Windows の QPC の換算は商と余りに分けて掛ける（`counter / frequency * 1e9 + counter % frequency * 1e9 / frequency`）。64 bit の積の overflow を避ける。
- Windows の offset は instant の UTC の SYSTEMTIME と、`SystemTimeToTzSpecificLocalTimeEx(&dynamic, &utc, &local)` の local の差（両方を FILETIME にして引く）。範囲は header の FIRST〜LAST に clamp。
- daylight の判定は header の通り（offset が `-(Bias + DaylightBias)` 分で DaylightBias が 0 でない時）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh os-time` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
