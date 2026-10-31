<!-- awesome-plan project=zedbsd record=ws334-p002 -->

# ws334-p002 設計 A: libbrowser・libmedia の module と build の骨格（標準廉価実装手順）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。手順は docs/agent/spec-module-delegation.md の §4（ws.md の決定 22）。
- 状態: in-progress（2026-11-01 01:30 P7 q1790）。
- 依存: p001。

## 目的

design.md の §8 の p002〜p009・p013 の範囲（libbrowser の OS の層・storage・libmedia の OS の層・build）を、実装の担当（Sonnet 5.5 high）が header と phase.md だけで書ける module に分け、header・.c の骨格・試験の道具・実装の phase.md を作る。統合（Opus）の Phase の phase.md も手順・file・受け入れまで書き、実装の間に設計の判断を残さない（Q1 の条件）。

## module の分け（この Phase の分）

| module | header（約束） | .c | Phase | 試験 |
| --- | --- | --- | --- | --- |
| Win32 の補助 | `libbrowser/base/os-win32.h` | `os-win32.c` | p005 | test-os-win32（win32） |
| system | `base/os-system.h` | `os-system-posix.c`・`-win32.c` | p005 | test-os-system |
| file | `base/os-file.h` | `os-file-posix.c`・`-win32.c` | p006 | test-os-file |
| network | `base/os-net.h` | `os-net-posix.c`・`-win32.c` | p007 | test-os-net |
| 時計と時刻帯 | `base/os-time.h` | `os-time-posix.c`・`-win32.c` | p008 | test-os-time |
| 時刻帯の名前の表 | `base/os-zones.h` | `os-zones.c` | p009 | test-os-zones |
| storage の記録 | `storage/records.h`（＋公開の `include/browser/storage.h` の sink） | `records.c` | p010 | test-records |
| files の backend | `storage/files.h`（＋公開の `browser_storage_files_open`） | `files.c` | p011 | test-files |
| storage の thread | `storage/queue.h` | `queue.c` | p012 | test-queue |
| libmedia の OS の層 | `libmedia/media-os.h` | `media-os-posix.c`・`-win32.c` | p013 | test-media-os |

- 共通の約束は `base/os.h`（.c 無し）。依存は一方向: os-win32 ← os-system・os-file・os-net・os-time（Windows の側だけ）、os-zones ← os-time（Windows）、records ← files・queue、os-file ← files、os-system ← queue（thread の名前）。
- 手順の「1 module = 1 header + 1 .c」からの外れ: OS の層は 1 つの header に POSIX と Windows の 2 つの .c（同じ約束の 2 つの実装。1 回の起動で両方を書き、同じ試験を両方の system で通す）。p005 は header 2 つ（os-win32 と os-system。os-win32 は小さく、他の Windows の .c が依るので最初に）。
- 公開の header `include/browser/storage.h` は新しい file にした（browser.h は p023 で版を上げる時に include する）。今の browser.h・exports.map を設計の間は変えない（P8 との重なりを避ける）。

## 私（設計の担当）が持つ物と理由

- header・骨格・build の骨格（`apps/browser-win32/Makefile`、一番上の `browser-win64`・`browser-winarm64`）・試験の道具は設計の成果物（Q1 の条件）。build の本体（sources.mk の分け、DLL、Vulkan の遅延の import）は p020 の実装の Phase に置いた（glue だが実装なので）。
- 統合の Phase（p020〜p027・p030）は Opus の phase.md に手順・変える file・受け入れを書いた。

## 試験の道具（`plan/ws334/tests/`、実装の担当は変えない）

- `modules.sh`: header・source・道具の一覧、include の path、試験ごとの link する source（posix・win32）。
- `run-unit.sh [--posix|--win32] <名前>...|all`: posix は host の cc と ASan・UBSan、win32 は llvm-mingw の x86_64 と UBSan（trap）で build して Wine で流す（prefix は `build/tmp/ws334-wine`、初回に数分）。試験は自分の空の directory で走る。最後の行 `run-unit: PASS`。
- `check-skeleton.sh`（`BASE=` で header と道具の不変）: header の単独の compile（cc・clang・x86_64 と aarch64 の llvm-mingw）、.c の compile（posix は cc・clang、win32 は llvm-mingw の 2 つ、他は 4 つ）warning 0、`_WIN32`・`<windows.h>` が Windows の file の外に無い、`make browser-win64`・`browser-winarm64`。
- `unit/check.h`（`CHECK`・`CHECK_EQ`・`check_finish`）、`unit/test-tools.c`（道具の自己試験）。

## 経過

- 2026-11-01 01:30 決定 22 で p002 を設計 A に組み直した（ws.md の表、Q1 が形を了承）。
- 2026-11-01 01:57 header 12・骨格 15・道具・phase.md（p005〜p013・p020・p021）を commit c22068d43。`check-skeleton: PASS`（header 12、source 15、make の 2 つ）、`run-unit.sh tools` PASS（posix と win32 の Wine）。
- 2026-11-01 CLDR 48 の 3 file を Q1 が `build/sources/specs/cldr-48/` に置いた（SHA-256 一致）。p009 に license（Unicode License V3）を記録。
- 2026-11-01 02:03 phase.md（p022〜p027・p030）。

## 残り

- design-reviewer は p004 で設計 B（p003）と合わせて 1 本。基点の SHA は p004 で決め、全部の phase.md の `<p004 の基点の SHA>` を置き換える。
