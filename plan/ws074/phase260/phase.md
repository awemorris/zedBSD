<!-- awesome-plan project=zedbsd record=ws074-p260 -->

# ws074-p260: Interop 2025 の Chromium が通る試験を全部通す

Status: in-progress
Disposition: normal
Parent: [WS074](../ws.md)
Primary Milestone: MG006
Queue: なし（ユーザーの直接の指示で、Claude Code on the web のセッションが進める。Codex の q55 とは別）
Origin: 2026-10-27 ユーザーの指示。

## ユーザーの決定

- 2026-10-27 ユーザーの決定: WPT Interop 2025 のうち Chromium が成功している項目に限り、browser も 100% 通す事を目標にする。
- 同日の作業の範囲: Web ブラウザの改良（WS074）を担当し、master.md には書かず ws.md と下位の phase だけに書く。commit したらこの branch
  （`origin/libbrowser`）へ自動で push してよい。
- 2026-10-30 ユーザーの決定: WS074 の実行は P8 の phase-runner-low（Sonnet 5.5・high）で行う。

## 目標と受け入れ

- 分母: 固定の Interop 2025 の選定（`plan/ws074/phase173/evidence/20261013-q1012/manifest.json`、3,805 ID、WPT 2d66b9b7998b）のうち、
  同じ runner・同じ条件で Chromium が PASS する ID（基準は Chrome for Testing 153.0.8010.12。2026-10-30 からは local の Debian の Chromium 156.0.8078.73、PASS の数は同じ 3,472）。
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
- 全件の測定（2026-10-27）:

  | 測定 | harness | static | scripted | crash | 計（3,472 のうち） |
  | --- | --- | --- | --- | --- | --- |
  | ours-base（基準） | 496 | 841 | 103 | 39 | 1,479（42.60%） |
  | ours-dev3（Navigation API の merge の後） | 680 | 881 | 104 | 39 | 1,704（49.08%） |
  | ours-dev6（69502eb73） | 725 | 894 | 104 | 39 | 1,762（50.75%） |
  | ours-dev11（276c2ff10） | 767 | 1,021 | 107 | 39 | 1,934（55.70%） |
  | ours-dev15（5bb9f3310、2026-10-28） | 812 | 1,035 | 113 | 39 | 1,999（57.57%） |
  | ours-dev16（2026-10-28） | 841 | 1,035 | 113 | 39 | 2,028（58.41%） |
  | ours-dev17（864752041、2026-10-28） | 862 | 1,038 | 113 | 39 | 2,052（59.10%） |
  | ours-dev18（2026-10-28） | 869 | 1,056 | 112 | 39 | 2,076（59.79%） |
  | ours-dev19（ce9775f01、2026-10-28） | 876 | 1,067 | 114 | 39 | 2,096（60.37%） |
  | ours-dev20（2ba50ae52、2026-10-30、Chromium 156 の PASS で数え直し） | 880 | 1,071 | 115 | 39 | 2,105（60.63%） |
  | ours-dev24（q1708-i02 の途中、gradient・logical・overflow まで） | 964 | 1,085 | 115 | 39 | 2,203（63.45%） |
  | ours-dev25（66e610b8a、module・view-transition の parse・zoom まで） | 973 | 1,085 | 115 | 39 | 2,212（63.71%） |
  | ours-dev26（7727cb0b0、harness だけ測り直し。static・scripted・crash は dev25 のまま） | 976 | 1,085 | 115 | 39 | 2,215（63.80%） |

  dev3 → dev6 の主な修正: grid の明示の grid より前の暗黙の線（負の線番号）、auto-repeat の中の名前付きの線、
  直交する item を持つ縦書きの flex・grid は元の物理座標の処理、縦書きの flex での abspos の静的な位置の揃え、
  縦書きの root を viewport の右へ、匿名 box が writing-mode を継承、UA の margin を logical に、
  vertical-lr と vertical-rl の入れ子を同じ domain で、vertical-rl の safe の戻り先。
  dev6 → dev11: HTMLCollection などの反復、grid の auto の最小の track、grid・flex の shorthand の longhand の読み書き、
  place-items・place-self・place-content、grid の order と z-index の描画順、auto-fit で畳まれた track と content の分配、
  置換要素の min/max と比、column の flex の自動の最小、縦書きの行の中の置換要素。
  dev11 → dev15: anchor positioning（anchor()・anchor-size()・position-area・position-try・anchor-center・anchor-scope）。
  dev15 → dev16: script から見える画像の読み込み（img の load・error、complete、naturalWidth/Height）、LargestContentfulPaint、
  Node.baseURI。後退 0。
  dev16 → dev17: form の submit()・requestSubmit()、:target、button.form、grid の minmax() の maximize、repeat() の線名の重複、
  areas による明示の grid の大きさ、min-height を含む auto-fill の行、grid item の % の高さ（内在の行では不定）。後退 0。
  dev17 → dev18: flex の baseline・last baseline と交差軸の auto margin、grid の行の baseline 群（内在の行は baseline で伸びる）、
  flex・grid の container の baseline、br の clear、subgrid の構文、要素の scrollWidth・scrollHeight。
  後退は subgrid の偶然の合格 5 件（subgrid の配置は未実装）と grid-baseline-001（直交の item、dev19 で修正）。
  dev18 → dev19: 直交の item の baseline、scrollWidth の flex の向きと abspos、flex の画像の比の転写と column の stretch、
  block の button の fit-content、abspos の flex・grid の top/bottom の高さ、相対位置の縦の % と inline box の相対位置。
  後退 5 件（flex の画像の最小幅、自身の高さを持つ画像の比の転写が原因）は dev19 の後に修正。
- 再開点（2026-10-28 の停止）:
  - 最後の全件の測定は ours-dev19（2,096）。その後の commit 7d096964d（flex の画像の比の転写の後退の修正、grid の stretch の
    max-height・max-width、auto margin の画像、align-items: auto の拒否、safe の content の揃え、内容で決まる行の stretch の除外、
    fit-content() の列、grid の scrollWidth）は flex・grid の部分の測定（後退 0）と対象の試験だけで確認し、全件の測定は未実施。
    dev20 の測定は途中で止めて消した。次は全件の測定（dev20）から。
  - 次の候補: grid-template-columns の要素が grid でない時の計算値（repeat() をそのまま直列化する。nogrid の試験 2 件）。
  - 残りの大きな塊: grid の column 軸（justify）の baseline と縦書きの baseline、grid の track sizing の仕様どおりの書き直し、
    subgrid の配置、View Transitions、Navigation API の文書をまたぐ遷移・iframe、grid・grid-template の shorthand の
    longhand からの直列化、Error の生成位置（navigateerror の lineno）。

### q1708-i01 の途中経過（2026-10-30、P8、local の Debian 13 host）

- 比較の環境: `sh plan/ws074/tests/debian-setup.sh`（commit 5e20c8da9）。apt で nodejs・npm・chromium-headless-shell を入れ、
  playwright-core を `build/ws074-chrome/node` に入れて `playwright` module を headless shell に向け、`build/ws074-chrome/bin/chromium` の wrapper と fonts.conf を作る。
  使う時は `export NODE_PATH=$PWD/build/ws074-chrome/node/node_modules PATH=$PWD/build/ws074-chrome/bin:$PATH`。
- Chromium の版: apt の更新で chromium が 153.0.8010.52 から **156.0.8078.73** に上がった（chromium-headless-shell が同じ版を要求したため）。前回の .12 とは版が違う。
- WPT: `build/ws074-suites/wpt` に全体を取得、commit 2d66b9b7998bb58c336138c178323ddee857b586、licence 確認済み。
- host build: `CC=clang sh plan/ws074/tests/host-build.sh plain` 成功（wpt-ready-render も build 済み）。
- 測定（背景で実行中だった。結果は未確認）: `build/ws074-cmp/ours-dev20`（`--kind ours --jobs 8`、source は 2ba50ae52 = 7d096964d を含む）、
  `build/ws074-cmp/chrome156`（`--kind chrome --jobs 6`、NODE_PATH・PATH は上のとおり）。`runs.json` ができたら完了。
  次は `python3 plan/ws074/tests/interop2025-chrome-gap.py compare --chrome build/ws074-cmp/chrome156 --ours build/ws074-cmp/ours-dev20`、
  Chromium PASS の数（前回 3,472）と dev20（dev19 は 2,096）を上の表に足す。
- （q1708-i02 で実施済み。下の結果を参照。）

### q1708-i02 の結果（2026-10-30、P8）

- Chromium 156.0.8078.73 の PASS は harness 1,477・static 1,552・scripted 396・crash 47 の計 3,472（153 と同数）。測定は `build/ws074-cmp/chrome156`。
- host build は `-Wall -Wextra -Werror` で、全 file を新しい出力先に build した log に warning 0（exit 0）。Acid3 は 100/100 で pixel 差 0（Acid2 は 99.27% のまま、基準と同じ）。
- 全件の測定で後退 0（dev20 → dev26、4 runner とも dev20 の PASS が全て PASS のまま。dev25 で view-transition の `default` の拒否が足りず 2 件が後退し、dev26 で直した）。
  harness +96、static +14。
- 原因の分け方と直した物（commit 4c9ac8262 … 7727cb0b0）:
  1. 測定の環境: runner が `browser --run` を `--async` 無しで呼ぶため web の iframe が一度も読み込まれず、navigation-api の no_output 99 件などが落ちていた
     （at-once の読み込みは web の frame を読まない）。runner（`run-interop2025-harness.py`）に `--async` を付けた。Chromium の adapter は無視する。
     これだけだと load が画像・style sheet を待たず 62 件が後退したので、window の load を frame・画像・style sheet・web font の完了まで保留し
     （`page_fire_load` の保留と `page_fire_load_resume`、`page_load_waiting`）、style sheet が後続の script を止める（`page_wait_sheets`）ようにした。
  2. iframe の文書をまたぐ navigation（p261 に詳細）、iframe の timer、別 realm の promise の await、beforeunload・pagehide、initial about:blank、detached。
     navigation-api の Chromium が通す harness 158 件のうち 80 件が PASS（この 158 件は dev20 で全て非 PASS）。
  3. CSS: `linear-gradient`・`repeating-linear-gradient`（`css/gradient.c`、`paint/gradient.c`、背景 image の bitmap に描く。flexbox の legacy 試験 +14）、
     overflow の computed（visible は他の軸が scroll・hidden なら auto）と shorthand の正規化、declaration の comment の除去、
     flow-relative な margin・padding・inset・border の computed、`border-inline`・`border-block` と各 width・style・color の shorthand、
     `view-transition-name`・`view-transition-class`・`zoom` の parse と computed（zoom は layout に効かない）。
  4. JS の module: default import・default export・`export { a as b }`・`import "x"`・外部の `<script type=module src>`・import attributes
     （key が type 以外なら SyntaxError、type が json 以外なら TypeError）・JSON module。module の失敗は error event で報告する
     （`bind_report_exception_at`）。html/semantics の import-attributes・json-module のうち 6 件が PASS。
- 道具: `plan/ws074/tests/wpt-one.sh <ID>...`（1 ID ずつ runner と同じ条件で実行して callback を見る）。

再開点（2026-10-30、q1708-i02 の停止）:
- 最後の全件の測定は ours-dev25（static・scripted・crash は 66e610b8a、harness は dev26 の 7727cb0b0）。その後に commit は無い。
- 残りの大きな塊（Chromium が通す ID、dev26 で gap 1,257）: scripted reftest 281、grid の alignment（baseline、harness 68）、
  anchor positioning の scroll（anchor-scroll・anchor-scroll-position-try 19）と anchor() の CSSOM の正規化（parse-valid 2）、flexbox の baseline（約 25）、
  View Transitions の `document.startViewTransition` と pseudo-element の selector（約 35）、Web Animations の `Element.animate`（grid・flex の補間の試験約 20）、
  CSS の zoom の layout、`css-logical` の shorthand を CSSOM の longhand から読む（9）、grid の computed の repeat() の保持（2）、
  動的 `import()` と module の namespace import、`window.open`・popup・joint session history を使う navigation-api、writing-modes の sideways（static 約 40）。
- 次の候補（効きが大きい順の見込み）: scripted reftest の gap の原因分け（281 件。今回は未調査）、`Element.animate`、`startViewTransition`。
