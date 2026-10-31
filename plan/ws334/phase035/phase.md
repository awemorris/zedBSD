<!-- awesome-plan project=zedbsd record=ws334-p035 -->

# ws334-p035 規約の全文の見直しと header の持ち越しの直し

- 親: [WS334](../ws.md)。AGENTS.md §2（code を作る WS の最後の Phase）、手順 docs/agent/spec-module-delegation.md の §4.6（header の持ち越し）。
- 状態: planned（2026-11-01 p003 で作成）。
- 依存: p033・p034。
- 担当の想定: conformance-reviewer（書き方だけ、動作を変えない）。

## 中身

- WS334 が足した・変えた C の全部（`git diff <p004 の基点> --stat -- '*.c' '*.h'` の一覧。module・統合・apps・試験の道具）を plan/coding-style.md の全文と §14 の checklist で読み、書き方を直す。
- 触れた block の開発の管理情報の comment（`ws074-pNNN` など、今の file にある物）を guardrail の規則で直す（design §9.1 の最後）。
- 実装の間に記録された header の comment の誤り（各 phase.md の結果の節と Q1 の受け入れの記録）を header に反映する（この Phase だけが header を変えてよい。直した後に `check-skeleton.sh` を BASE 無しで、`run-unit.sh all`・`cross-build.sh`・`wine-run.sh` を流す）。
- 結果: 直した file の数、checklist の項ごとの数、header の直しの一覧。commit、ws.md の行。

## 持ち越し（Q1）

- 2026-11-01 p009: `base/os-zones.h` の `struct wb_os_zone_names` の comment「名前は ASCII」を直す（CLDR 48 の Türkiye が UTF-8）。
- 2026-11-01 p013: `plan/ws334/tests/run-unit.sh` が win32 の試験で locale を `C.UTF-8` にする（host の ja_JP.UTF-8 が無いと Wine が英字でない名前の file を作れない）。
- 2026-11-01 p008: `base/os-time.h` の Windows の時差の comment の `SystemTimeToTzSpecificLocalTimeEx` を、実装の `GetTimeZoneInformationForYear`＋`SystemTimeToTzSpecificLocalTime` に合わせる（前者は Wine で stub）。DST の試験を harness に入れる方法（Wine の TZ）も検討する。
