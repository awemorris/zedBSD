<!-- awesome-plan project=zedbsd record=ws334-p009 -->

# ws334-p009 Windows の時刻帯の名前の表: os-zones（表の data）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.4 の時刻の行（略号の表が Windows で使えない理由）。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libbrowser/base/os-zones.h`。それが include する header も読む。
4. 仕様: Unicode CLDR 48（Unicode License V3）: `common/supplemental/windowsZones.xml`（SHA-256 `9cf3db6a31fb382fee21b70be6feba1e82766b0fcd06e6261fb7936a73e537ff`）、`common/supplemental/metaZones.xml`（`985247ef22cfb5d1af6c91ceb34f255e392a3569247904afb7839ba3bc1a12d6`）、`common/main/en.xml`（`416fb34c500e969a9e3e9bd1e363f8af5938df3cdda72aa7e636fa7e72123aae`）。置き場は `/home/awe/zedBSD-claude1/build/sources/specs/cldr-48/`（2026-11-01 Q1 が置き SHA-256 を照合した。git に入れない。元は `https://raw.githubusercontent.com/unicode-org/cldr/release-48/<path>`）。
   - license: CLDR の data は Unicode License V3（CLDR の repository の `LICENSE`、release-48 で "UNICODE LICENSE V3"、Copyright © 2004-2025 Unicode, Inc.。2026-11-01 P7 が確かめた）。data から作った表を配る時は copyright と許諾の文を同梱する: libbrowser は既に `userland/base/licenses/browser/Unicode-License-V3`（Unicode の case の表の物、libbrowser と一緒に入る）を持ち、Windows の配る形（p031）も同じ file を `share\licenses\` に入れる。この Phase は表の block の comment に出典と license の名前を書くだけで、license の file を足さない（同じ Unicode License V3 の文。年の範囲は file の方が新しい）。

## 作る物

- `userland/desktop/libbrowser/base/os-zones.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-os-zones.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 表: `wb_os_zone_table` の全部の key が strcmp の順に並び重複が無い、key が空でない、standard が NULL でない、daylight は NULL か空でない。数は windowsZones.xml の territory "001" の mapZone のうち名前の求まる物の数（試験の中に結果の数を書き、xml を数えた手順を結果の節に書く）。
- 手で xml から求めた値（試験に書く）: "Tokyo Standard Time" → "Japan Standard Time"・"Japan Daylight Time"、"Pacific Standard Time" → "Pacific Standard Time"・"Pacific Daylight Time"、"W. Europe Standard Time"（Europe/Berlin → Europe_Central）→ "Central European Standard Time"・"Central European Summer Time"、"GMT Standard Time"（Europe/London → GMT と zone の daylight）→ "Greenwich Mean Time"・"British Summer Time"、"UTC"（Etc/UTC の zone の名前）→ "Coordinated Universal Time"・NULL、"India Standard Time"（Asia/Calcutta → India）→ "India Standard Time"・NULL、"Arabian Standard Time"（Asia/Dubai → Gulf）→ "Gulf Standard Time"・NULL（daylight の無い metazone の 2 つ）、"China Standard Time"（Asia/Shanghai → China）→ "China Standard Time"・"China Daylight Time"、"AUS Eastern Standard Time"（Australia/Sydney → Australia_Eastern）→ "Australian Eastern Standard Time"・"Australian Eastern Daylight Time"、"Eastern Standard Time"（America/New_York → America_Eastern）→ "Eastern Standard Time"・"Eastern Daylight Time"（2026-11-01 の design-reviewer の指摘で直し、P7 が CLDR 48 の file で確かめた。metazone の `<short>` の名前は使わない）。zone の自分の名前が metazone の名前を上書きする規則は header（`struct wb_os_zone_names` の comment）にある。
- `wb_os_zone_find`: 全部の key で自分の entry、NULL・""・"tokyo standard time"（大小の違い）・"Tokyo Standard Time "（空白）→ NULL。

## 注意

- 名前の決め方（header の通り）: key → windowsZones.xml の `mapZone other=key territory="001"` の type（IANA の zone、空白で区切った複数なら最初）→ metaZones.xml の `<timezone type=...>` の `usesMetazone` のうち `to` の無い物の mzone → en.xml の `<metazone type=mzone><long>` の `<standard>`・`<daylight>`。en.xml に `<zone type=IANA><long>` があれば、その field がある物はそれで上書き（London の daylight、Etc/UTC の standard）。standard が求まらない key（Etc/GMT+12 などの metazone の無い zone）は表に入れない（os-time が system の名前に落ちる）。
- 表は data。block の前に「Values referenced: Unicode CLDR 48 common/supplemental/windowsZones.xml, metaZones.xml and common/main/en.xml (Unicode License V3)」の複数行の comment。名前は ASCII（CLDR の en の long の名前は ASCII。非 ASCII があれば止めて報告）。
- 表の値は 2 つの方法で照合する（手順の §5.3）: (1) xml を script（`python3 -I`、試験の道具ではなく自分の確かめ、git に入れない）で読んで作った一覧と、(2) 手で数十件を xml から読んだ物。照合の方法を結果の節に書く。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh os-zones` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=<p004 の基点の SHA（ws.md の Resume point と p004 の phase.md に記録）> timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。
