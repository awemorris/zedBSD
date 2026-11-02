<!-- awesome-plan project=zedbsd record=ws074-p263 -->

# ws074-p263: 実際の Web サイトでの UAT の指摘への対処

Status: in-progress
Disposition: normal
Parent: [WS074](../ws.md)
Primary Milestone: MG006
Queue: [q2000](../../queue.md)（Q1 が自分で開発する。サブエージェントは使わない）
Origin: 2026-11-03 ユーザーから、WPT Interop 2025 の Phase（p260 ほか）とは別に、実際の Web サイトでの UAT の指摘に対処する Phase を立て、Q1 が直に開発する、という趣旨の指示を受けた。

## 目標

ユーザーが実際の Web サイトで見つけた libbrowser の表示・動作の不具合（UAT の指摘）を、指摘ごとに原因を調べて直す。
p260（Interop 2025 の Chromium が通る試験を全部通す）の目標・測り方は変えない。

## 範囲

- `userland/desktop/libbrowser/` と、必要なら `userland/desktop/browser/`（shell）。
- 指摘ごとの最小の再現（`plan/ws074/phase263/repro/`）と host の試験。
- 範囲の外: HAL・toolchain・compositor（`userland/desktop/wayland/`）。compositor の側の原因は根拠を記録して Q1 の別の作業にする。
  Windows 版の shell（WS334）の固有の不具合は WS334 の Bug にする。

## 受け入れ

- 指摘ごとに、host の browser（`plan/ws074/tests/host-build.sh plain`）でその site（または保存した頁・最小の再現）が直ったことを、
  Chromium との並べた画面（`live-compare.py`・`render-compare.py`）か動作の確かめで示す。
- host build の warning 0。p260 の測定で後退 0（全件の測り直しはまとまった単位で、細かい修正ごとには流さない、AGENTS §9）。
- 直せなかった指摘は理由と要る判断を記録する。

## 指摘の一覧

| # | 日付 | site | 指摘 | 状態 |
| --- | --- | --- | --- | --- |
| U1 | 2026-11-03 | https://www.amazon.co.jp/ | 画面が真っ白（Windows 版で確認） | 調査済み・ユーザーの判断待ち |

## 状態

2026-11-03 01:4x: Phase を立てた（Q1）。

2026-11-03 01:47: U1 を host で再現（main 010f487aa1 の host build、warning 0。`--render --async` 1280x900 で画面は白の 1 色）。
- amazon.co.jp は本当の頁ではなく **AWS WAF の bot の確認の頁**（`challenge.js` を読み、`AwsWafIntegration.getToken()` の後に
  `location.reload(true)` で開き直す）を返した。この頁は `<div id="challenge-container">` だけで、Chromium でも確認の間は白い。
- engine の誤り: `ReferenceError: FileReader is not defined` と `TypeError: Cannot read properties of undefined or null (at 1:646274)`
  （challenge.js の中）。token が取れず、開き直しに進まないので白のまま。
- AGENTS・master の規則「bot の確認に当たったら止めて報告、User-Agent の変更・別の取り方の再試行はしない」に当たるので、
  先へ進む前にユーザーに方針を聞く。
