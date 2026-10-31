<!-- awesome-plan project=zedbsd record=ws334-p008 -->

# ws334-p008 時計と時刻帯: os-time

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-11-01 03:50、cleared は Q1）。
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
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

2026-11-01（host の `date`）に実装した。

実装: `userland/desktop/libbrowser/base/os-time-posix.c`・`os-time-win32.c`（全部の関数）、単体試験 `plan/ws334/tests/unit/test-os-time.c`。header・他の module・道具は変えていない。

確かめ:

- `timeout 1800 sh plan/ws334/tests/run-unit.sh os-time` → `test-os-time: PASS (197 checks) (posix)`、`test-os-time: PASS (743 checks) (win32)`、`run-unit: PASS`。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`（PASS の行 17 本 = 全 test の posix と win32、リファクタ前に `run-unit: PASS` の行も確認、後に PASS 17 本を数えた）。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- `git diff --check HEAD` → 出力なし。
- 任意の確かめ（Wine は起動時の `TZ` で zone を決めるので、新しい wineserver で `TZ=<zone> wine os-time.win32.exe win32`）: America/New_York（747 checks）・Australia/Sydney（769）・Europe/London（743）・Pacific/Kiritimati（739）が PASS。harness の zone は host の Asia/Tokyo（DST 無し）なので、DST の経路は harness だけでは動かない。

わざと壊した確認（git の外で変えて元に戻し、`cmp` で一致を確認）: posix 10 通り（ERANGE の境界、上端・下端の clamp を外す、`tm_gmtoff` の符号、単調の秒の換算、名前の切りの長さ、壁の時計の nsec を落とす、単調の nsec を落とす、"UTC" の既定名、offset NULL の検査）と win32 11 通り（QPC の余りの換算、秒の換算、壁の時計の epoch、壁の時計の ×100、daylight の判定の反転、daylight の offset の式の符号、秒への割り算、ERANGE の境界、上端の clamp、FILETIME の epoch を 1 日ずらす、端の日の再試行を外す）。最初に見逃した物（上端 clamp を 400e9 秒ずらす壊し方は glibc が年 22000 を扱うので同じ答えになり検出できない、名前を 62 byte で切る、壁の時計・単調の時計の nsec を落とす、QPC の秒の換算）は試験を足して検出するようにした（1100 ms の眠り、sub-second の非零、C の library の tm_zone を基準にした 62・63・64・80 字の略号）。DST の 3 通り（daylight の offset の符号、変換する instant を 1 日ずらす、端の日の再試行を外す）は、harness の zone（Tokyo）では検出できず、`TZ=America/New_York` の手動の Wine 実行では 3 通りとも FAIL になった。見逃しが残るのは次の 2 つ: posix の `*offset = 0;`（失敗の時に 0 を返す約束）は localtime_r が失敗する状況を作れず試験できない、POSIX の「tm_zone が空なら UTC」は glibc が不正な TZ で空の名前を返すので `TZ="XX"` で試験できたが、`tm_zone` が NULL の系は試験できない。

自分で決めた点（header が黙っている所、または header と違う点）:

- **header の記述と違う点（Q1 の確認が要る）**: header の comment は Windows の offset を `SystemTimeToTzSpecificLocalTimeEx` と書くが、Wine 10.0（Debian）はこの関数が stub で、呼ぶと abort する（`unimplemented function KERNEL32.dll.SystemTimeToTzSpecificLocalTimeEx`）。そこで instant の年の UTC を `FileTimeToSystemTime` で求め、`GetTimeZoneInformationForYear(年, 動的な zone)` でその年の規則を取り、`SystemTimeToTzSpecificLocalTime` で local にした（Ex 版は同じ規則を使うので offset は同じ）。試験の基準（phase.md が指定）も同じ API の組なので、試験にその年の規則を手で評価する基準（`rule_offset`: Bias・StandardBias・DaylightBias と遷移の週・曜日・時刻から日数を数える）を足し、API の結果と一致を確かめる。header の comment を「Windows: その年の規則による変換」に直すかは Q1 が決める（直す時は p004 の確定の扱い）。
- Windows の最初の日: 西の zone では UTC の 1601-01-01 00:00 の local が 1600 年になり、`SystemTimeToTzSpecificLocalTime` が失敗する（`TZ=America/New_York` の Wine で確認、再試行を外すと EIO）。FIRST から 1 日以内の instant は、変換が失敗したら FIRST + 1 日の offset を答える。LAST 側は local が 10000 年になるが SYSTEMTIME は 30827 年まで扱うので不要。
- 失敗の時の出力: offset は 0、名前は空の文字列にする（EINVAL の size 0 の時だけ buffer を触らない）。EIO は time_t が instant を持てない時（32 bit の time_t）と localtime_r の失敗。
- POSIX の名前: `tm_zone` が NULL か空なら "UTC"。63 byte を超える略号は UTF-8 の文字の途中を避けて 63 byte 以下に切る（`tm_zone` の中身が UTF-8 かは調べない）。
- Windows の名前: 表が key を知っていれば daylight の判定の時に daylight の名前（表の daylight が NULL なら standard）、そうでなければ standard。表が知らない key は system の StandardName・DaylightName の preferred の方、それが空か UTF-16 として不正ならもう一方、全部空なら "UTC"。key は 127 unit まで読み（NUL が無くても配列の外を読まない）、変換できない key は表が知らない key として扱う。名前は 63 byte に文字の境界で切る。
- Windows の単調の時計: 余りの積が 64 bit を超える速さ（約 18 GHz 超）の counter は、余りと周波数を同じだけ右に shift して精度を落とす（実在しない値への防護）。counter が読めない・周波数が 0 以下・counter が負の時は 0 を返す（header の戻り値は 0 の失敗を決めていない）。壁の時計は 64 bit に入らない時刻を INT64_MAX・INT64_MIN にする。
- `wb_os_time_zone_start` の Windows は何もしない。
- 試験: 100 ms の眠りの前後に加え、秒の繰り上がりを確かめる 1100 ms の眠り（1 s 以上 3 s 未満）も入れた（試験は約 1.3 s 増える）。壁の時計は `time(NULL)` の前後の間にあることも確かめる（10 ms の余裕）。

未実施:

- 実機の Windows、aarch64 の Windows の実行（compile だけ）。
- `TZ` を変えた Wine の確かめは harness に入れていない（新しい wineserver が要る。同じ prefix で wineserver が動いている間は環境変数を変えても効かず、子 process を使っても Tokyo のままだった）。DST の Windows の経路は上の手動の実行のみ。
- Windows の `GetDynamicTimeZoneInformation` が `TIME_ZONE_ID_INVALID` を返す EIO の経路、`SystemTimeToTzSpecificLocalTimeEx` を使う経路。
