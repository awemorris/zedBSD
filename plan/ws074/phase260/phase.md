<!-- awesome-plan project=zedbsd record=ws074-p260 -->

# ws074-p260: Interop 2025 の Chromium が通る試験を全部通す

Status: in-progress
Disposition: normal
Parent: [WS074](../ws.md)
Primary Milestone: MG006
Queue: なし（ユーザーの直接の指示で、Claude Code on the web のセッションが進める。Codex の q55 とは別）
Origin: 2026-10-27 ユーザーの指示。

## ユーザーの決定（原文）

2026-10-27: 「WPT Interop 2025で、chromeが成功している項目に限り、100%を目指しましょう。」

同日の作業の範囲: 「あなたはWebブラウザの改良、ws074を担当します。master.mdには書き込まず、ws074/ws.mdと下位のphaseにのみ書き込めます。」
「以後、commitしたらこのブランチには自動でpushしてOKです。」（branch は `origin/libbrowser`）

## 目標と受け入れ

- 分母: 固定の Interop 2025 の選定（`plan/ws074/phase173/evidence/20261013-q1012/manifest.json`、3,805 ID、WPT 2d66b9b7998b）のうち、
  同じ runner・同じ条件で Chromium（Chrome for Testing 153.0.8010.12）が PASS する ID。
- 受け入れ: その全部で browser も PASS（`interop2025-chrome-gap.py compare` の gap が 0）。Chromium が落とす ID は目標に入れない。
- runner がまだ扱えない ID（https・testdriver・worker など、どの runner の対象でもない物）は、Chromium の結果が無いので今の分母の外。
  分母の外にある事を「未測定」として数で残し、PASS とは数えない。runner を広げる時は Chromium も同じ runner で測り直す。

## 方法

1. 構築は ws.md の「クラウド環境での構築と Chromium との比較」。WPT は全体を取る（sparse では Interop の html/・event-timing/ などが欠け、
   harness の対象が 1,508 → 1,072、static が 1,561 → 1,536 に減っていた）。
2. Chromium は `plan/ws074/tests/chrome-run.js` が browser と同じ command line（`--run`、`--render`、wpt-ready-render の位置引数）で答える。
   runner は変えずに `--program` だけ差し替える。runner の側で直したのは、program の隣に libbrowser.so が無い時に hash を None にする所だけ
   （run-interop2025-scripted-reftests.py）。
3. 測定: `python3 plan/ws074/tests/interop2025-chrome-gap.py measure --kind chrome --out build/ws074-cmp/chrome`（Chromium は一度だけ）、
   `... measure --kind ours --out build/ws074-cmp/<名前>`、比較は `... compare --chrome build/ws074-cmp/chrome --ours build/ws074-cmp/<名前>`。
4. gap を原因ごとに分け、1 回の修正で多くの ID が通る原因から engine を直す。直すたびに host-build（warning 0）と、変えた所の試験、
   区切りごとに全件の測定で後退が無い事を確かめる。

## 状態

### 基準（2026-10-27、main の 0500e8c8 の source を build/ws074-host/plain に build）

`interop2025-chrome-gap.py compare --chrome build/ws074-cmp/chrome --ours build/ws074-cmp/ours-base`:

| runner | 測った ID | Chromium PASS | browser PASS | gap |
| --- | --- | --- | --- | --- |
| harness | 1,508 | 1,477 | 496 | 981 |
| static reftest | 1,561 | 1,552 | 841 | 711 |
| scripted reftest | 413 | 396 | 103 | 293 |
| crash | 48 | 47 | 39 | 8 |
| 計 | 3,530 | 3,472 | 1,479（42.60%） | 1,993 |

どの runner の対象でもない ID は 275（https・testdriver・worker など）。Chromium の結果が無いので分母の外（未測定）。

### gap の内訳と順（大きい物から）

- grid・flexbox・writing-modes（harness と reftest で約 960 件）。うち約 390 件が縦書き、約 230 件が baseline。→ [p262](../phase262/phase.md)（縦書きを logical の座標で layout）、
  次に baseline の model、grid の個別の不足。
- Navigation API（harness 約 330 件、`window.navigation` が無い）→ [p261](../phase261/phase.md)（別の agent、同じ文書の中から）。
- anchor positioning（約 157 件）、View Transitions（約 41 件）、@scope（22 件）、CSSOM の rule の object（CSSScopeRule など）、CSS nesting、@layer の順。

### 進み

- grid の名前付きの線・area・grid-template/grid の shorthand（3d0dfc2bc ほか）: harness +12 / -0（共通の 1,072 ID で比較、後退の 2 件は直した）。
- @scope（56f0203cd ほか）: css-cascade の @scope の 22 件のうち 10 件が PASS。
- 縦書き（p262）: css-writing-modes の static reftest 11 → 25（2 件の後退を調査中）。
