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
  | ours-dev27（ad3b33639、2026-10-31、q1727-i01、4 runner を測り直し） | 986 | 1,107 | 235 | 41 | 2,369（68.23%） |
  | ours-dev28（5b032c17b、harness・scripted・crash を測り直し。static は同じ source の部分測定の再実行 1,111 pass で後退 0、数えは 1,107） | 996 | 1,107 | 242 | 41 | 2,386（68.72%） |

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

### q1727-i01 の途中経過（2026-10-30〜31、P8）

- 道具（plan/ws074/tests/）: `ref-one.py ID...`（static reftest の test・ref・差の地図を横に並べた PNG、`--chrome` で Chromium も）、
  `static-subset.py PREFIX...`（manifest の prefix だけの static reftest を並列に流して前回の全件の結果との REGRESSION・FIXED を出す）、
  `scripted-one.py ID...`（scripted reftest 1 件の test・ref の capture と差、`--chrome` あり）、
  `scripted-subset.py PREFIX...`（scripted reftest の prefix だけ。runner に `--only` を足した）。いずれも program を複写して流す（流している間に build し直してよい）。
  build は `CC=clang sh plan/ws074/tests/host-build.sh plain`（増分で約 40 秒）。
- grid の abspos の静的位置（layout/grid.c）: grid container が containing block でない（position: static）時は grid-area を使わず content box を矩形にして
  justify-self・align-self で揃える（`grid_static_content_box`）。containing block の時の area への揃えも `grid_abspos_areas` に足した（`grid_static_alignment`）。
  static の css-grid/abspos 59 → 81（subset）、全 static で 1089 → 1111、後退 0。
- View Transitions（css-view-transitions、scripted 2 → 120 前後 / 205、後退 0）:
  - JS（plan/ws074/phase230/prelude.js → `make-prelude.py`）: `document.startViewTransition`（callback・{update, types}）、ViewTransition（ready・finished・updateCallbackDone・skipTransition・types）、
    `document.activeViewTransition`。native.viewTransition(1|2|0)（old を capture・new を測って木を出す・終わり）と viewTransitionDuration（bind/prelude.c → bind_host.view_transition・view_transition_duration）。
  - page/transition.c: 状態（page.transition）。old: 今の layout で名前付き box を測り（layout/transition.c の `layout_transition_measure`、名前の重複は失敗、match-element は element から名前を作る）、
    root（全体、名前付き box を除く）と各 box の painting を snapshot（paint/list.c の `paint_transition_capture`、items の複写）に取る。new: 木なしで測り直して木を出す（`show`）。
  - layout/box.c `box_build_transition`: root の box の最後の子に `::view-transition` → group → image-pair → old・new の box を作る。group の大きさと位置は old・new の rect を
    animation の進み（`layout_transition_progress`: delay・duration・play-state・timing function ease/linear/steps/cubic-bezier、fill both、時刻は 0）で補間、
    transform の平行移動は UA の物（`CSS_VT_TRANSLATE_PENDING`、作者の transform が置き換える）。old・new の opacity は UA の fade の進み。
    画像の高さは幅と元の縦横比から、object-fit・object-position 対応。group は paint 順（stacking order）に並べる。
  - css/: `::view-transition`・`-group`・`-image-pair`・`-old`・`-new`（名前・`*`・`.class`）のセレクタ（名前は type、class はまとめて 1 class の詳細度）、
    `css_engine_compute_vt_pseudo`、UA sheet（css/ua.c）に木の既定、`:root { view-transition-name: root }`、`:first/last/only-child` は兄弟の位置（CSS_VT_FIRST・LAST）。
    新しい property: animation（shorthand）・animation-name・-duration・-delay・-play-state・-timing-function、mix-blend-mode（plus-lighter だけ区別）。keyframes は未実装（UA の名前だけを解釈）。
  - paint/list.c: root が名前付きなら木だけを白の上に描く（無ければ普通の painting で名前付き box だけ除く）。old は snapshot、new は live の box を描いて group の box へ移して縮尺を合わせる（text は縮尺しない）。
  - getComputedStyle(el, '::view-transition-*(name)')（bind/computed.c、page_vt_pseudo_style）と animation-* の computed。
  - 未対応（残り）: inline 要素の capture（inline-element-size・new-content-is-inline など約 10）、縦書きの massive element、iframe の transition（約 10）、shadow の ::part（6）、
    Web Animations（getAnimations・set-current-time・web-animations-api 約 8）、dialog・top layer、scrollbar・scroll の rebase、harness の pseudo-elements-valid の URL variant。

### q1727-i02 の途中経過（2026-10-31、P8）

- 確かめ（image・QEMU）: `flock /tmp/zedbsd-image-build.lock sh plan/ws074/tests/build-browser-image.sh build/amd64` が exit 0（libbrowser の warning 無し）。
  Venus の guest（runtime build/ws074-run）で wayland --glass と `/bin/browser` が起動し、`browser-start.sh` が PASS（start → about → 戻る、zdesktop に ERROR 無し）。
  browser-start.sh は古かった（既定の頁が google.com に変わった事、待ちが早すぎた事）ので直した（start.html を URL に渡し、wayland の socket と window の map を待つ。7215eef0b）。
  引数無しの起動は https://www.google.com/ を network から読んで描いた（画面で確認）。
- 直した物（commit 6aed8bc06、ec73c1d7a、7f65d6e32）:
  1. flex の自動の最小（min-width: auto）は行が収まる時も、flex-basis より大きい時も効く（収まる時の省略と base での頭打ちを外した）。flex: 0 0 0% の項目が内容の幅になる。
  2. flex・grid の item は inline-block のように 1 つずつ丸ごと描く（背景を先に全部描いてから文字、をやめた）。隣の item に食い込む文字の重なり順が合う（flexbox_flex-0-* など）。
  3. table: `caption-side`（top・bottom）、table の height・min-height は行が分け合う（caption を除く）、column の flex で stretch された table は幅を満たす。
  4. flex の `justify-content: start|end`（flex-start・flex-end でない）は container の writing mode の左・右（column は上・下）で、flex-direction の逆でも変わらない。
  5. flex-shrink の合計が 1 未満なら、不足の一部（合計倍）だけ縮む（9.7 の規則）。
  6. 入れ子の column の flex で、auto の高さの container の中の item の % の高さは auto 扱い（`height_indefinite`）。
  7. 画像の flex item の content-based minimum は、定まった cross size に自然な比を掛けた値。
  8. `backdrop-filter`（blur・brightness・contrast・grayscale・hue-rotate・invert・opacity・saturate・sepia、url() は読み飛ばし）の parse・cascade・computed、
     表示 list の PAINT_BACKDROP と software renderer の色行列・Gauss blur（GPU は描かない）。`will-change: transform|perspective|filter|backdrop-filter` と backdrop-filter は fixed・absolute の containing block を作る。
     filter-effects の static 51 のうち gap だった 20 → 10（残りは border-radius の clip、zoom、opacity と backdrop root、transform、SVG の rect の CSS の幅）。
- 部分の確認: css-flexbox・css-grid・css-align・css-position・css-writing-modes・css-tables の static（1,413 件の subset）で後退 0、FIXED +16（flexbox 以外の abspos 22 は q1727-i01 の分）。
- 調べて直さなかった物（次の候補）:
  - 直交 flow の flex item（縦書きの item を横の flex に置く）は layout_logical_wanted が flex の item を物理座標のままにするため、base size が inline の max-content になる（flexbox-writing-mode-009〜016、align-self-vert-rtl、align-self-horiz 約 25 件）。
    正しくは item を論理座標の領域にして block size を base にする。縦の行は折り返さない簡易実装（`vertical_inline`）が前提なので、まず flex の item を領域にする設計が要る。
  - table の `height` は caption を除く grid box の高さ（caption は外）。flex が与える高さは wrapper 全体（caption を含む）なので両者を区別する印が要る（table-as-item-inflexible-in-row-2、min-height-1）。
  - backdrop-filter の parse: drop-shadow()・url("...") の保持、canonical な直列化（blur(0px)・hue-rotate(0deg)・100% の上限）。
  - flexbox の SVG の viewBox と片方の寸法から決まる固有の大きさ、`min-height: min-content`。

再開点（2026-10-31、q1727-i01 の停止）:
- 最後の全件の測定は ours-dev28（source 5b032c17b、`build/ws074-cmp/ours-dev28` の harness・scripted・crash と、static は `static-subset.py` の全件の再実行）。2,386 / 3,472 = 68.72%、
  4 runner とも dev25/26 の PASS に対する後退 0。その後に commit は無い（この記録の commit だけ）。Acid3 100/100・pixel 差 0、Acid2 99.27%（基準と同じ）。
  gap 1,086: harness 481、static 445、scripted 154、crash 6。
- 残りの大きな塊（dev28 の gap）: css-grid（harness 約 160: alignment の baseline 68・animation 12・subgrid、static 約 180: subgrid 44・alignment・abspos の orthogonal）、
  css-flexbox（harness 約 60: baseline 31、static 約 144）、css-anchor-position（約 160: 要素の scroll 約 36、top layer 6、position-try の細部）、css-view-transitions（scripted 約 75 残り:
  inline 要素の capture 約 10、iframe 約 10、縦書き、shadow ::part 6、Web Animations 約 8、dialog・top layer、scrollbar、harness の pseudo-elements-valid/invalid の URL variant・
  querySelector での pseudo-element の SyntaxError・hit-test）、navigation-api（約 80: window.open・popup・joint session history）、css-viewport の zoom（static 21 と harness 3、
  実効 zoom を cascade の長さに掛ける設計が要る）、css-writing-modes の sideways（static 約 40）、backdrop-filter（約 30）、shape-outside（16）。
- 要素の scroll（overflow の scrollTop・scrollLeft）が無い（`bind_scroll_top_set` は root だけ）。anchor-scroll・position-visibility など約 36 件に効くが、行の座標が block 相対で paint・hit・geometry に散るため
  設計が要る（layout_absolute の content 原点を scroll 分ずらす案は、inline の行・fragment が block 相対なので paint・hit・bounds も直す必要がある）。次に取り組む価値が大きい。
- 道具: 一部の失敗の原因は ref-one.py（static）・scripted-one.py（scripted、`--chrome` で Chromium と見比べ）で PNG を見るのが早い。wpt-one.sh は URL variant の ID を扱えない。

再開点（2026-10-30、q1708-i02 の停止）:
- 最後の全件の測定は ours-dev25（static・scripted・crash は 66e610b8a、harness は dev26 の 7727cb0b0）。その後に commit は無い。
- 残りの大きな塊（Chromium が通す ID、dev26 で gap 1,257）: scripted reftest 281、grid の alignment（baseline、harness 68）、
  anchor positioning の scroll（anchor-scroll・anchor-scroll-position-try 19）と anchor() の CSSOM の正規化（parse-valid 2）、flexbox の baseline（約 25）、
  View Transitions の `document.startViewTransition` と pseudo-element の selector（約 35）、Web Animations の `Element.animate`（grid・flex の補間の試験約 20）、
  CSS の zoom の layout、`css-logical` の shorthand を CSSOM の longhand から読む（9）、grid の computed の repeat() の保持（2）、
  動的 `import()` と module の namespace import、`window.open`・popup・joint session history を使う navigation-api、writing-modes の sideways（static 約 40）。
- 次の候補（効きが大きい順の見込み）: scripted reftest の gap の原因分け（281 件。今回は未調査）、`Element.animate`、`startViewTransition`。
