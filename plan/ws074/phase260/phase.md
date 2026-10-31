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
  | ours-dev30（4078977d5、2026-10-31、q1727-i02、4 runner を測り直し。static は static-subset.py の全件を並列で。後退あり、下の記述） | 998 | 1,137 | 236 | 41 | 2,412（69.47%） |
  | ours-dev31（a6fad1068、2026-10-31、q1727-i02、4 runner を測り直し。後退は scripted の 2 件だけ） | 998 | 1,146 | 242 | 41 | 2,427（69.90%） |

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

- 全件の測定（ours-dev30、source 4078977d5、`build/ws074-cmp/ours-dev30`）: 2,412 / 3,472 = 69.47%（dev28 は 2,386）。gap 1,060: harness 479、static 415、scripted 160、crash 6。
  Acid3 100/100・pixel 差 0（exact match）、Acid2 99.27%（基準と同じ）、host build は `-Wall -Wextra -Werror` で warning 0。
  **後退あり（dev28 の gap に無く dev30 の gap にある ID）: harness 0、crash 0、static 9、scripted 8**。原因は backdrop-filter の実装で、これまで「test も reference も backdrop-filter を無視して偶然同じ絵」で通っていた物:
  - static: css-backdrop-filters-animation-{blur,brightness,contrast,grayscale,hue-rotate,invert,saturate,sepia}（8）は reference が backdrop-filter を使い、test が @keyframes の animation（paused、delay -2s）で backdrop-filter を動かす。engine に @keyframes が無いので test に効かない。
    + css-view-transitions/view-transition-name-is-backdrop-filter-root（1）。
  - scripted: filter-effects/backdrop-filter-{basic-blur,boundary,edge-clipping-2,edge-mirror,edge-pixels-2}（5）は blur の端（blur は box の外へ滲む、mirror の端）が Chromium と違い maxDifference を超える。
    css-view-transitions/{backdrop-filter-animated,new-element-on-start,old-content-captures-different-size}（3）は、old-content-captures-different-size は単独で PASS（測定中の負荷による時間切れの疑い）、new-element-on-start は単独でも FAIL（20,000 画素差）、backdrop-filter-animated は @keyframes。
  次の Attempt は、(1) @keyframes（parser.c の `@position-try` と同じ形で名前付きの rule を sheet に持ち、animation-name から progress を求めて backdrop-filter・filter を補間する）、(2) blur の端（box の外へ滲む filter と mirror の backdrop）で後退を 0 にする。
  net の増分は +26 だが、後退 17 ID を直すまで dev30 を「後退 0」とは言えない。
- 追加で直した物（4078977d5）: 入れ子の column の flex で item の主軸の大きさが定まるのは flex-basis が長さの時（basis が auto なら height が長さの時）だけにした（広げすぎて percentage-heights-005〜007・flex-basis-012 が後退したのを直した）。
  画像の content-based minimum は row だけ（column は比の無い SVG で後退した）。`filter`（同じ関数群、border box に後から適用）と `will-change`（transform・perspective・filter・backdrop-filter は containing block と backdrop root を作る）を足した。

- **最終の測定（ours-dev31、source a6fad1068、`build/ws074-cmp/ours-dev31`）: 2,427 / 3,472 = 69.90%**（dev28 は 2,386、+41）。gap 1,045: harness 479、static 406、scripted 154、crash 6。
  dev28 の gap に無く dev31 の gap にある ID（後退）は **harness 0・static 0・crash 0・scripted 2**。上の dev30 の後退 17 件のうち 15 件を、blur を描かない（Gauss の blur は box の外へ滲まず端が mirror でないため、無い方が reference に近かった）、
  `@keyframes`（parser.c の `parser_keyframes`、cascade.c の `cascade_animation`: 名前・delay・duration から時刻 0 の進みを求め、backdrop-filter・filter だけを前後の keyframe の間で補間）、
  view-transition-name を持つ祖先を backdrop root として直した。残る 2 件（scripted）:
  `css-view-transitions/new-element-on-start`（view-transition の pseudo-element の `filter: invert(1)` が image-pair の描きに入る。以前は filter を無視した偶然の一致）と
  `css-view-transitions/backdrop-filter-animated`（pseudo-element の animation で backdrop-filter を動かし `getAnimations()` の currentTime を使う）。次の Attempt で直す。
  Acid3 100/100・pixel 差 0、Acid2 99.27%（基準と同じ）、host build は `-Wall -Wextra -Werror` で warning 0（dev30 の時点。dev31 の source の差は css・paint の追加だけで、build は warning 0）。
  gap 一覧: `build/tmp/p8/gap31.json`。
- 調べて直さなかった物（次の候補）:
  - 直交 flow の flex item（縦書きの item を横の flex に置く）は layout_logical_wanted が flex の item を物理座標のままにするため、base size が inline の max-content になる（flexbox-writing-mode-009〜016、align-self-vert-rtl、align-self-horiz 約 25 件）。
    正しくは item を論理座標の領域にして block size を base にする。縦の行は折り返さない簡易実装（`vertical_inline`）が前提なので、まず flex の item を領域にする設計が要る。
  - table の `height` は caption を除く grid box の高さ（caption は外）。flex が与える高さは wrapper 全体（caption を含む）なので両者を区別する印が要る（table-as-item-inflexible-in-row-2、min-height-1）。
  - backdrop-filter の parse: drop-shadow()・url("...") の保持、canonical な直列化（blur(0px)・hue-rotate(0deg)・100% の上限）。
  - flexbox の SVG の viewBox と片方の寸法から決まる固有の大きさ、`min-height: min-content`。

### q1727-i03 の途中経過（2026-10-31、P8、host の / の空きが少ないので測定の出力は /tmp/p8out）

- 測定の道具（plan/ws074/tests/）: `measure-snapshot.sh NAME [JOBS]`（host build の browser・wpt-ready-render・libbrowser.so を /tmp/p8out/NAME/bin に複写して 4 runner を測り、static は static-subset.py の並列で流して合わせ、Chromium（build/ws074-cmp/chrome156）と比べて gap.json を作る。約 3 分。build し直してよい）、
  `gap-diff.py OLD.json NEW.json`（gap に入った ID = 後退、出た ID = 修正）、`harness-fails.py --gap G --ours DIR PREFIX...`（gap の harness の ID の失敗した subtest と message）、
  `static-subset.py`・`scripted-subset.py` に `--gap gapNN.json`（前回の全件の結果が無くても、Chromium が通り gap に無かった ID を基準にして REGRESSION を出す）、`host-build.sh` に libmedia の h264sw を足した。
- 測定（gap は build/tmp/p8/gap*.json に残した）:

  | 測定 | 内容 | harness | static | scripted | crash | 計（3,472 のうち） |
  | --- | --- | --- | --- | --- | --- | --- |
  | dev32（3c8ebb185） | 隔離した filter の group | 998 | 1,146 | 243 | 41 | 2,428（69.93%） |
  | dev36（edbd03c1a） | 要素の scroll、anchor の解決、popover | 1,006 | 1,146 | 261 | 41 | 2,454（70.68%） |
  | dev37（1d2ad3ef0） | flex・grid の baseline、fieldset、overflow の clamp | 1,012 | 1,148 | 261 | 41 | 2,462（70.91%） |

  | dev38（813ef1a6a） | CSS logical の CSSOM（shorthand の longhand と computed の直列化、inline-size など）、groove・ridge | 1,021 | 1,149 | 261 | 41 | 2,472（71.20%） |
  | dev39（zoom まで） | zoom（cascade）、css-viewport/zoom +11 | 1,022 | 1,160 | 263 | 41 | 2,486（71.60%） |
  | dev40（efd1d8313） | navigation-api の DataCloneError・intercept・detach、Streams・WebAssembly.Memory の stub | 1,039 | 1,160 | 263 | 41 | 2,503（72.09%） |
  | dev42（cfd40a883） | grid の abspos の descendant、display: contents の blockify、grid-*-gap の computed | 1,041 | 1,160 | 263 | 41 | 2,505（72.15%） |
  | dev43（hit test の変更まで、source は 0c8adb187 の直前の commit） | flex・grid の hit test の順 | 1,043 | 1,160 | 263 | 41 | 2,507（72.21%） |

  どの測定も後退 0（dev31 または直前の gap との差で、4 runner とも gap に新しく入った ID は無い）。
- 直した物:
  1. filter を隔離した group で描く（paint/paint.h の `PAINT_GROUP`・`PAINT_GROUP_END`、paint/software.c の `software_group`: group の items を黒の上と白の上に 2 回描いて各画素の色と coverage を求め、filter の関数を色に掛けて page の上に重ねる）。
     box の `filter` が box 自身の painting にだけ効く（以前は後ろの backdrop ごと filter していた）。view-transition の木の layer には、子孫の layer まで 1 つの group で包む。backdrop-filter を持つ box は従来の形（backdrop ごと）。
     css-view-transitions/new-element-on-start が PASS。`document.startViewTransition` を文書の parse 中に呼ぶと css engine が無く segfault していたのを直した（page/transition.c）。
  2. 要素の scroll: dom_element に `scroll_x`・`scroll_y`、bind/geometry.c の scrollTop・scrollLeft・scrollTo・scroll・scrollBy（root 以外）が overflow を持つ block を範囲内（scroll 幅 - client 幅）で動かし、文書の generation を進める。
     layout/scroll.c の `layout_scroll_apply` が layout と abspos の配置の後に container の中身（in-flow の部分木と、containing block が container の中の abspos）を scroll 分ずらす（clip は動かない）。
     anchor（layout/anchor.c の anchor_find）は、anchor を含む scroll container のうち対象の containing block を含まない物の scroll 分だけずれた rect を返す（対象はその位置で配置され、scroll に付いて動く。position-try の溢れの判定もずれた位置で）。
     scrollWidth・scrollHeight は text の line の端と block の子の終端側の margin を含む。anchor の名前の衝突は、scroller の中の abspos の anchor は対象より後にあれば取らない。
     css-anchor-position の scripted・harness の anchor-scroll 系 + position-visibility の一部（scripted +18、harness +8）。
  3. popover（prelude.js、css/ua.c、css/cascade.c）: `popover` 属性、showPopover・hidePopover・togglePopover、`:popover-open`、UA sheet（open は fixed・fit-content・margin auto・z-index 最大。top layer は z-index で近似）。
     position-area の `inset: 0` は auto と同じ揃えにする（popover の UA の `inset: 0` が position-area を壊さないように）。
  4. `document.fonts.load()`・`check()`、`Range.getClientRects()`・`getBoundingClientRect()`（text node の rect の和）。
  5. flex・grid container の baseline の書き出し（layout/inline.c `inline_items_baseline`）: 「最初」は物理的に先頭の line（wrap-reverse・rtl の column の line の逆順を見る）の物理的に先頭の item（row-reverse・rtl の row・column-reverse は逆）、「最後」は反対側。
     last baseline の flex・grid container の対応。overflow が visible でない box の baseline は border box の中に clamp。table の baseline は caption を除く。fieldset の baseline は legend を除く。
     fieldset の legend（最初の legend の子）は上の border の位置に載り、border の高さと legend の高さの大きい方だけが上を占める（layout/block.c `block_fieldset_legend`）。
     flex-align-baseline-001・flex-flex-001・fieldset-001 と grid の同等の物、overflow-001 が PASS。
  6. CSS logical box properties（bind/style.c `style_box_get`: margin・padding・inset・border-width・style・color と inline・block の shorthand と border の shorthand から longhand を inline style で読む。bind/computed.c `computed_box_shorthand`:
     shorthand の computed を longhand から作る（4 辺は短い形）、`inline-size`・`block-size` と min・max の computed、`grid-row-gap`・`grid-column-gap`）、sideways-lr の inline-start は下（cascade.c `cascade_margin_property`）、border-style の groove・ridge を区別。
     css-logical の logical-box-margin・padding・inset・border-width・color・style・shorthands・size が PASS（border-radius は未）。
  7. zoom（css/cascade.c）: 要素の effective zoom（祖先の積）を `css_style.zoom_eff` に持ち、cascade の長さ（px・pt・in・vw など、calc の px、font-size の em・%・keyword、継承した font-size・line-height・text-indent・border-spacing・text-shadow、rem は root の zoom で割って要素の zoom を掛ける）に掛ける。
     computed の px の出力は zoom で割る（bind/computed.c `computed_zoom`、`computed_pixels`）。画像の自然な大きさ（layout/replaced.c）も掛ける。css-viewport/zoom の static が 2 → 14。
  8. navigation-api（plan/ws074/phase230/prelude.js）: structuredClone が EventTarget・Event らしい object を DataCloneError にする（realm をまたいでも）、ReadableStream・WritableStream・TransformStream と WebAssembly.Memory（shared の buffer は clone 不可）の stub、
     unserializable な state の rejection の順、intercept() が detach 後に InvalidStateError、navigateerror の ErrorEvent の lineno・colno（Error に位置が無いので 1・1）、NavigationHistoryEntry・NavigationDestination の key・id・url・index が detach 後に空・-1。harness +17。
  9. grid の abspos: grid が containing block の abspos の descendant（親が item の block）も grid-placement の area を containing block にする（layout/grid.c `grid_abspos_deeper`。入れ子の grid・subgrid の下と、名前付きの線は除く）。
     flex・grid の hit test は item を order の逆（上に描く物から）に探す（layout/hit.c `hit_items`）。computed の blockify は display: contents の親を飛ばす。`::view-transition` の UA の position は absolute。
- 調べて直さなかった物（次の候補）:
  - 直交 flow の flex item（`/tmp/p8out/flex-ortho-exp1.patch`: `layout_logical_wanted` が flex の item を物理のままにする所を外すと static の css-flexbox で +5/-5。後退は fit-content-item-002〜004（auto の cross を持つ縦書きの item の inline size が viewport になる）・ortho-table-item-001・stretching-orthogonal-flows）。
    必要な事: 縦書きの item の cross 方向の inline size を fit-content にする（logical_fits_content が flex の item を除外している所）、stretch された cross の再 layout。
  - 縦書きの flex・grid の baseline（flex-align-baseline-002・004・fieldset-002/003・grid-self-baseline の vertical 系、約 60）は縦の container での baseline の軸と item の synthesized baseline の規則が要る。
  - multicol（columns）は未実装（flex-align-baseline-multicol 6、anchor-position-multicol 約 10）。subgrid の配置は未実装（subgrid の grid-gap・line-names・repeat-auto-fill 約 30 は subgrid の track を親の track にする設計が要る）。
  - CSS Transitions・Animations・Web Animations が computed style を補間しない（flex-grow・flex-basis・order・grid-template-*・backdrop-filter の interpolation・composition、約 25）。prelude.js の `Animation` は時刻の管理だけ。
  - css-logical の shorthand（margin-inline など）を inline style の longhand に展開して直列化する（logical-box-margin・padding・inset・border-width・border-color が各 11 subtest）。position-try の last-successful の状態（約 10）。navigation-api の window.open・popup・複数 frame（約 80）。

### q1727-i04 の途中経過（2026-10-31、P8）

- 測定（gap は build/tmp/p8/gap44〜47.json、測定の出力は /tmp/p8out/devNN）:

  | 測定 | 内容 | harness | static | scripted | crash | 計（3,472 のうち） |
  | --- | --- | --- | --- | --- | --- | --- |
  | dev44（4bd58180d） | 直交 flex item、position-try の last-successful・position-area のはみ出し | 1,045 | 1,164 | 263 | 41 | 2,513（72.38%） |
  | dev45（3220b8c7c） | sideways-lr、table の direction:rtl | 1,047 | 1,193 | 264 | 41 | 2,544（73.27%） |
  | dev46（9b45c337b） | thead/tfoot の順、position-area の直列化、anchor の scope・containing block の連鎖・logical 側・margin の anchor-size、IMCB での fit、offsetParent の transform | 1,059 | 1,204 | 265 | 41 | 2,569（73.99%） |
  | dev47（2e179c668） | 縦の行の replaced を central baseline に、minmax の最小が最大より大きい track、image-set（ここまで） | 1,061 | 1,205 | 266 | 41 | 2,573（74.11%） |

  | dev48（08f927f15） | position-visibility、position: fixed を viewport に付ける、dialog、top layer の anchor | 1,062 | 1,209 | 281 | 41 | 2,593（74.68%） |
  | dev49（e04957d55） | 縦書きの root の view transition の old/new の大きさ（inline-size・block-size）、logical な border radius | 1,063 | 1,209 | 293 | 41 | 2,606（75.06%） |
  | dev50（36a244fec） | 非 grid の repeat() の computed、table の高さ（caption を除く）、flex の中の table の最小幅 | 1,065 | 1,210 | 293 | 41 | 2,609（75.14%） |

  dev43 → dev47 の差: 後退は static の css-writing-modes/table-column-order-slr-007 の 1 件だけ（sideways-lr で正しい配置になり、以前の合格は
  縦書きの rtl の偶然の一致。この試験は rtl の中の数字の run を視覚順に並べ替える bidi が要る。engine には bidi が無い）。他の runner は後退 0。
- 順の判断: gap の数が大きい物から直した。subgrid（約 60）は track の共有の設計が要るので後回し、直交 flex は layout_logical_wanted を
  flex の item にも使う（直交の item は fit-content の inline size、column の stretch、self-start/self-end、table の最小幅）で +6、
  flexbox-writing-mode-009〜016（縦の flex container + 横の item）は container が物理座標で layout される経路で未対応。
- 直した物:
  1. sideways-lr（layout/logical.c）: vertical-lr の領域で inline の軸を反転する mode（`LOGICAL_SIDEWAYS_LR`）。logical の左＝物理の下。unflip で box の y を
     親の inline の大きさから反転、行は `vertical_inline = 2`。描画は paint/list.c で反時計回りの glyph（paint_item.vertical = 2、software.c と GPU の shader に
     kind 4 を足して paint/shaders.h を再生成）。css-writing-modes の slr 系 +25、wm-propagation-body-* 8 件（縦の行の replaced は central baseline、sideways は alphabetic で行の下から上へ）。
  2. table（layout/table.c）: `direction: rtl` の列を右から、thead の行を先頭・tfoot を末尾（row-progression 系 3 件）。
  3. position-try（layout/position.c）: 最後に fit した fallback を要素に覚え（`dom_element.anchor_last`、fallbacks の一覧が変わると忘れる）、どれも fit しない時はそれを使う。
     position-area の normal の揃えではみ出した box は containing block の中へ戻す（scroll container の中は戻さない）。fallback が fit する範囲は inset を引いた
     containing block（IMCB）。getComputedStyle の offsetParent は transform を持つ祖先も数える。
  4. anchor（layout/anchor.c、css/anchor.c）: anchor-scope（同じ scope の中だけで anchor が見える）、anchor の containing block の連鎖の最後が abspos で後ろにある時は取らない、
     anchor() の start/end/% と anchor-size() の block/inline を containing block の writing mode と direction で、margin の anchor-size()、閉じ括弧の無い関数、
     position-area の setter の直列化（宣言の形を保つ）。rtl の block の max-content は左 margin の余りを数えない。
  5. grid: repeat(auto-fill, minmax(30px, 6.5%)) のように最大が最小より小さい track は最小を使う。
  6. image-set()（background-image の最初の候補とその解像度）、zoom を背景画像の固有の大きさに掛ける。
- dev47 の後に直した物（dev48〜dev50）:
  7. position-visibility（layout/anchor.c `layout_anchor_hides`、position.c）: anchors-visible は default anchor が間にある scroll container の padding box の外へ出た時・anchor 自身が hidden の時に箱を隠す（paint と hit test は `anchor_hidden` の箱を飛ばす）、no-overflow は fallback の後でも IMCB をはみ出す時。
  8. position: fixed の箱を viewport に付ける（layout/sticky.c: fixed の箱を sticky と同じ list に入れ、scroll の分だけ下げる。anchor が scroll する物に付いた箱は動かさない）。view の scroll の要求を load 中に取りこぼさない（view/view.c）。
  9. `<dialog>`（HTMLDialogElement を interface に足し、prelude.js に show・showModal・close・open・returnValue、UA の sheet、top layer は popover と同じ flag に開いた順の番号）、`document.scrollingElement`、top layer の要素を anchor にできるのは先に top layer に入った物だけ。
  10. view transition の old/new の大きさを UA の sheet の `inline-size: 100%; block-size: auto` に（縦書きの root で高さが 100%、幅は snapshot の比から。layout/box.c）。massive-element-* 10 件と root の縦書き 2 件ほか。
  11. 非 grid の要素の `grid-template-columns/rows` に repeat() がある時は書いた形で返す（css/values.c `values_declared_text`、cascade.c `cascade_declared`で em を px に）、border-*-radius の logical 4 つ（物理の corner への対応は cascade.c と bind/computed.c）と computed の radius。
  12. table: `height` は caption を除く grid box の高さ（flex が全体の高さを与える時は `table_wrapper_height` で caption を引く）、flex の row の中の table は columns の最小幅より狭くならない、stretch で内容より低くならない。
  13. @layer（css/parser.c: layer の名前を sheet の順に、statement と block と無名、`css_rule.layer`。cascade.c: layer の重み、通常の宣言は layer 無しが最強・後の layer が強く、!important は逆）。
- 調べて直さなかった物: position-try-order（most-width 等）・@position-try の CSSOM（CSSPositionTryRule の class と insertRule）・var() を持つ try rule、
  navigation-api の initial about:blank（prelude の `initialBlank()` が iframe の作成中は parent を持たず常に false。直すと activation 等が使う経路に入るが、
  iframe の onload が出ず no_output になる試験が残り、得は無かったので戻した）、table-cell を writing-mode の root にする試験（srl-057 など 4 件）、
  shape-outside（engine に無い。css-shapes の選定 16 件は全部 sideways の変種で、clip-path も要る）、bidi。

### q1727-i04 の後半（2026-10-31、P8。dev51〜dev68）

- 測定（gap は build/tmp/p8/gap51〜gap6x.json、出力は /tmp/p8out/devNN。計は Chromium が通す 3,472 のうちの数）:

  | 測定 | source の先頭 | 計 | 内容 |
  | --- | --- | --- | --- |
  | dev51 | 684f73098 | 2,613（75.26%） | 絶対配置の self-alignment（両 inset がある時の justify-self・align-self）、flip の揃えの入れ替え |
  | dev52 | 18ba7630f | 2,617 | fixed の scroll を `page_scroll_to` で、`getComputedStyle(el, '::before')` と content、background 短縮形の image-set、LCP の生成 text |
  | dev53 | 8f1360999 | 2,618 | -webkit-text-stroke-width |
  | dev54 | d546261c4 より前 | 2,621 | `lh`・`rlh` の単位、非定高の percent row-gap の 2 回目の解決 |
  | dev55 | d546261c4 | 2,628 | `<canvas>` を replaced に（自然の大きさは属性、既定 300x150）、grid の ratio 寄与 |
  | dev56 | 82126c0a1 | 2,640 | SVG の percentage の長さ（rect の 100% など）、bare svg の stretch-fit |
  | dev57 | a5a73f1d9 | 2,645 | zoom の inherit、table の余りの高さは auto の行に先に、canvas の flex-basis: content |
  | dev58 | 7531d473b | 2,649 | flex・grid の空白だけの text（white-space: pre でも item にしない）、LCP の data URI の背景 |
  | dev59 | 13d5d98b0 より前 | 2,652 | インライン replaced の getBoundingClientRect は margin を除く、grid の percent の min-width、flex-grow が 1 未満の合計、flex の最小サイズ |
  | dev60 | （同上の続き） | 2,650 | position-area の auto inset を 0 に（scroll 等で後退 4、dev61 で直した） |
  | dev61 | ba09c56b9 の前 | 2,656 | auto inset は size が auto の時だけ 0 |
  | dev62 | ba09c56b9 | 2,661 | HTMLCanvasElement の width・height、チェックボックス・ラジオの zoom、content-visibility、`revert` |
  | dev63 | 9774120ff | 2,664 | `::details-content`、border-spacing と text-indent の calc() |
  | dev64 | 71ad40960 | 2,669（76.87%） | details-content: display contents、単一行 flex の line の cross size を min/max-height で挟む |
  | dev65 | eab76be30 | 2,672（76.96%） | percent の flex-basis が決まらない時は content と同じ、`::details-content` の display: contents |
  | dev66 | （dev65 の続き） | 2,674（77.02%） | column の ratio の最小（img の content size suggestion）、min/max-width の percent は測定の広い幅では無視 |
  | dev67 | 上の続き | 2,679（77.16%） | auto 高さの column の max-height、overflow が片軸だけ visible でない item の auto minimum、bitmap の `no_ratio` |
  | dev68 | 3d50a809f（source は dev67 の続き） | 2,680（77.19%、gap 792） | min-width/flex-basis の intrinsic keyword、flex-wrap の min-content |

  dev43 → dev64 の後退は static の css-writing-modes/table-column-order-slr-007 と、dev56 で scripted の grid-item-*-stretch 7 件（SVG の `cx="50%"` の円を描くようになり、
  natural size のままの bitmap を拡大するので縁がぼやけ、Chromium の vector の拡大と一致しない。以前は円が描かれず偶然合っていた）。
  それ以外の step の後退は 0（dev60 の 4 件は dev61 で直した）。
- 結果（2026-10-31 10:26 の host `date`）: 2,507（dev43）→ 2,680 / 3,472 = 77.19%。dev43 から dev68 の差は harness +42・static +103・scripted +36、後退は static 1（slr-007）と scripted 7（grid-item-*-stretch）。
  最後の source での Acid3 100/100・pixel 差 0（exact match）、Acid2 99.27%（基準と同じ）、host build は `-Wall -Wextra -Werror` で warning 0。dev69 以降は測定していない（dev68 の後の source の commit は無い）。
  Acid の runner が WPT の tree に残した一時 file（`build/ws074-suites/wpt/acid/acid2/zedbsd-acid2-*.html` 8 個、`acid3/zedbsd-acid3-reference-*.html`・`zedbsd-acid3-diagnostic-*.html` 各 4 個）は Q1 が消す。
  作業用の `build/tmp/p8/sub-1793396413`・`build/tmp/p8/refa`・`build/tmp/p8/refw` も同様。
- 直した物（続き。14 以降）:
  14. 絶対配置（layout/position.c）: 両 inset がある時の self-alignment（justify-self・align-self の start・end・center）、fallback の flip で LEFT/RIGHT・FLEX_* の揃えを入れ替え、対角の flip は direction で left/right を直す。position-area の下で size が auto の box は auto inset を 0 に（area の縁まで広がる）。
  15. 要素の `::before`・`::after` の getComputedStyle（content の computed）、background 短縮形の image-set()、`-webkit-text-stroke-width`、LCP の生成 text の幅・data URI の背景（plan/ws074/phase230/prelude.js）。
  16. `lh`・`rlh`（css/cascade.c: engine に `line_px`・`root_line_px`）。calc() の中は未対応。
  17. grid: percent の row-gap で高さが auto の時、行を固定部分だけで決めた高さに percent をかけて直す。items の percent の min-width は area の割合（stretch の時）。replaced（img・canvas）の ratio を固定の行から寄与として使う（`grid_ratio_width`）。
  18. `<canvas>`（layout/box.c・replaced.c・flex.c）: replaced の box、自然の大きさは width・height 属性（無ければ 300x150）で ratio を持つ（presentational hint ではないので属性は style の寸法にしない）、flex-basis: content、column の stretch で ratio から高さ。HTMLCanvasElement の interface と width・height の reflect（getContext は無い）。
  19. SVG の percentage の長さ（page/svg.c `svg_percent_length`: viewBox（無ければ bitmap）の幅・高さ・半径用の対角に対して）。viewBox だけの bare svg の stretch-fit は margin を引く。
  20. zoom: `width: inherit` の px は子の zoom を掛ける、チェックボックスとラジオの自然の大きさに zoom、calc() が border-spacing・text-indent で通る。
  21. table: 余りの高さは高さの指定が無い行に先に配る、caption の min-content が table の最小幅（anonymous table に caption も入れる）。
  22. flex: flex-grow の合計が 1 未満の時は最初の自由な空間に合計をかけた量が上限（2 周目以降も）、flex-basis が自分の値の item は max-content の測定でも min-content で持ち上げる、percent の flex-basis が決まらない時は content と同じ（高さの指定を無視）、単一行の line の cross size を container の min/max-height で挟む。flex・grid で空白だけの text は white-space: pre でも item にしない。
  23. getBoundingClientRect: インラインの replaced の piece は margin box なので margin を除く（layout/bounds.c。flex の radiobutton-min-size など）。
  24. @position-try: layer を持ち、同じ名前は最も高い layer の最後の rule（layer 無しが最強）。
  26. flex の column: auto の高さが max-height を超える時はその高さで flex する（flex-minimum-size-002）。img の content size suggestion を column にも（ratio の無い SVG は除く: `img_bitmap.no_ratio`）。overflow の一方が visible でないと自動の最小は 0。min/max-width の percent は測定の広い幅（2^24）では限界にしない。
  27. min-width の `min-content`・`max-content`・`fit-content`（layout/block.c `block_min_intrinsic`）、flex-basis の `min-content`・`max-content`・`fit-content`（row の非 replaced item）、flex-wrap の container を狭く測った時の幅は最も広い item。
  25. `content-visibility`（visible・auto・hidden。hidden は内容の box を作らない）、UA の sheet の `[hidden=until-found]`、`revert`（author の宣言を UA の宣言に戻す。css/cascade.c `cascade_reverted`）、`::details-content`（css/parser.c・layout/box.c `box_details_content`: summary の後の box を包む。display: contents なら包まない）。
- 調べて直さなかった物（理由）:
  - SVG を layout の大きさの vector で描く（bitmap は自然の大きさで作り、layout と paint が `image->width` を自然の大きさとして使う）。grid-item-*-stretch 7 件、css-viewport/zoom の svg 5 件、
    さらに数件に効く。直すには bitmap に自然の大きさと密度を持たせて layout・paint の約 12 か所を直す。
  - grid の `CSS_TRACKS`（24）を超える repeat()（grid-limits-001）、table-layout: fixed（flexbox_table-fixed-layout）、quirks mode の percent 高さ、`::details-content` の `::first-letter`/`::first-line`（details-pseudo-elements-005）、
    @scope の CSSOM（CSSScopeRule・CSSNestedDeclarations）と nesting、flex・grid の baseline（縦書きの item・fieldset・multicol・table の baseline の合成で約 45 件）、
    scrollIntoView の sideways（要素の負の scrollTop と縦の inline overflow の scrollHeight）、subgrid、computed value の補間、grid の calc() の整数（sibling-index()・sign() を含む）。
- 次の候補（効きの見込み）: (1) SVG の vector 描画（約 13）、(2) flex・grid の baseline（縦書きの合成 baseline、約 45）、(3) @scope の CSSOM と nesting（約 12）、
  (4) subgrid の track の共有（約 60）、(5) computed value の補間エンジン（grid・flex の animation、約 25）、(6) `::first-letter`/`::first-line` の pseudo-element、
  (7) table-layout: fixed、(8) CSS_TRACKS の動的な確保。

### q1727-i05 の途中経過（2026-10-31、P8。dev69〜dev73）

- 測定（gap は build/tmp/p8/gap69〜gap73.json、出力は /tmp/p8out/devNN）:

  | 測定 | 内容 | harness | static | scripted | crash | 計（3,472 のうち） |
  | --- | --- | --- | --- | --- | --- | --- |
  | dev69 | grid の直交 item を論理座標で layout | 1,084 | 1,262 | 292 | 41 | 2,679（後退 4 は dev70 で解消） |
  | dev70 | 上に加えて、spanning item の baseline の行・縦の item の justify baseline | 1,090 | 1,262 | 292 | 41 | 2,685 |
  | dev71 | position-area の inset 内の揃え・anchor が外にある area・containing block の writing mode、`inset: unset` | 1,095 | 1,265 | 292 | 41 | 2,693 |
  | dev72 | try-tactic の論理 flip、position-try-order、vertical の abspos の static 位置 | 1,098 | 1,267 | 292 | 41 | 2,698（77.71%、gap 774） |
  | dev73 | flip-start の揃えを入れ替えのままにする | 1,099 | 1,267 | 292 | 41 | 2,699 |
  | dev74 | flex の column の縦の item の baseline（左・右に寄せる group、wrap-reverse で反転、横の item は合成の baseline で加わる） | 1,102 | 1,267 | 292 | 41 | 2,702 |
  | dev75 | auto-fill・auto-fit の繰り返しの数を min・max の大きさから（max が最優先、min だけなら切り上げ、percent の min・max-height） | 1,105 | 1,267 | 292 | 41 | 2,705 |
  | dev76 | aspect-ratio が min-height を幅へ（auto の幅と繰り返しの数） | 1,105 | 1,268 | 294 | 41 | 2,708（78.00%、gap 764。ただし grid-auto-repeat-min-max-size-001 が後退して dev77 で直した） |
  | dev77 | 上の後退の修正（max-width を測定の間だけ隠している時は切り上げにしない）、最後の source | 1,106 | 1,268 | 294 | 41 | 2,709（78.02%、gap 763） |

  後退（dev68 に対し、dev77 まで）は static の css-grid/subgrid/orthogonal-writing-mode-001 の 1 件だけ（下記）。dev68 から harness +21・static +7（-1）・scripted +2。それ以外の後退は途中の測定で出た物を全て直した。Acid3 は 100/100・pixel 差 0（exact match）、Acid2 は 99.27% のまま（基準と同じ）、host build は warning 0（2026-10-31 11:3x の host `date`）。Acid の runner が残した一時 file は build/ws074-suites/wpt/acid/ の下に増えたので Q1 が消す。
- 直した物:
  1. grid の直交 item（縦書きの item を横の grid に）: layout/logical.c の `layout_logical_wanted` が grid の item の直交を論理座標で layout する（subgrid・replaced・atomic は除く）。
     item の inline 方向の room は固定の行の高さ（無ければ viewport）。stretch で行が確かな時は row の高さ、そうでなければ fit-content（`logical_fits_content`）。
     track の寄与は block size（`layout_max_content`・`layout_min_content` が `position_orthogonal_width`）。grid-align-baseline-003/004 ほか。
  2. grid の baseline: 複数の行にまたがる item は最初の baseline を最初の行、最後の baseline を最後の行の group に入れる（grid_row_baselines）。
     縦の item の justify-self: baseline（行の方向、`grid_column_baseline`）: 最初の baseline は item の block-start 側、最後は block-end 側に寄せる group で、各 group が行に沿って
     最初・最後の line の中央の baseline を共有する（左に寄せる group・右に寄せる group）。grid-justify-baseline-002、grid-align-baseline-003/004 ほか。
  3. position-area（layout/anchor.c・position.c・css/anchor.c）: inset が両側にある box は inset を引いた area の中で揃える（span-all は anchor に中央）。anchor が containing block の外にある時は area の
     線が anchor と CB の和（anchor-outside・partially-outside）。start・end・x-start・y-start は containing block の writing mode・direction、self- は box 自身の物（sideways-lr は mode 3）。
     grid area が containing block の時はその area から切る。`inset: unset|initial|inherit` の CSS-wide keyword を展開（values.c）。直交の abspos は left・right（top・bottom）の間を block size（inline size）で埋める。
  4. try-tactic: flip-block・flip-inline は containing block の writing mode に従う、flip-start は block-start と inline-start の間の対角（右上・左下の対角は 2 つの flip を重ねる。揃えは入れ替えのまま）。
     position-try-order（most-width・most-height・most-block-size・most-inline-size）は各 fallback の IMCB の大きさで安定に並べ替える。
  6. flex の column（layout/flex.c `flex_column_baseline`・`flex_column_has_vertical`）: 縦の item が cross 軸（左右）で最初・最後の line の中央の baseline を共有する。group は item の block-start 側（最初）・
     block-end 側（最後）に寄せ、左に寄せる group と右に寄せる group がそれぞれ baseline の両側の余白を line の大きさに足す。wrap-reverse は全ての group を反転。縦の item がある line では横の item も
     border box の左端（最後は右端）を baseline として加わる。共有の `layout_vertical_baseline`（inline.c）。flex-align-baseline-001/004、align-items-baseline-column-horz ほか。
  7. grid・flex の auto-fill・auto-fit（layout/grid.c `grid_expand_repeats`・`grid_expand_axis`）: 繰り返しの数は確かな幅・高さ、無ければ max（min が大きくても max 側の床）、min だけなら届く数（切り上げ）。
     percent の min・max-height は親の高さ（px）から。aspect-ratio の box は min-height から幅の min を作る（layout/position.c の shrink-to-fit も）。
  5. 縦書きで children_inline だが行が無い box の abspos の子の static 位置を保つ（layout/box.c `layout_absolute`）、block 配下の abspos の static の揃えの値を毎回初期化（block.c）。
- 後退として残した物: css-grid/subgrid/orthogonal-writing-mode-001（static）。以前は subgrid を実装しない grid の item を旧経路で layout し、reference も同じ旧経路で偶然一致していた。今は
  reference の方が正しく layout され（行の高さ 21.61）、test の subgrid は track を共有しないので 1 行（10 画素）の縁の濃さが 1/64 px ずれる。subgrid の実装で解消する見込み。
- 調べて直さなかった物（次の候補）:
  - flex の column の baseline（flex-align-baseline-001〜004・align-items-baseline-column/row-vert。縦の item が横の cross 軸で central baseline を共有する）と、縦書きの grid（writing-mode が縦の container）の
    align-self: baseline（grid-align-baseline-001/002、grid-justify-baseline-003/004）。grid の direct な列の baseline と同じ group の考え方（item の block-start 側に寄せる group）で書ける見込み。
  - subgrid（約 58）。方式の案: subgrid の軸では親の track を借りる。親の track の大きさへの寄与は subgrid の item の寄与を足し（edge の border・padding・margin と gap の差の半分を item の margin に足す）、
    subgrid の layout では親の track の大きさを注入する。grid.c の列・行の sizing は heuristic の積み重ねなので、先に spec の track sizing（§12.5 から §12.8）を独立の関数に書き直す方が安全。
  - track sizing の書き直し（grid-definition・layout-algorithm の約 50 ID: 高さが minmax の限界・span の配分・flex の寄与・auto-repeat・percent の row）。
  - anchor の parse・serialize（`anchor(--foo inside)` の canonical の順、入れ子の anchor() の fallback、`anchor(min(50%, 100%))`）、position-anchor の `normal`・`none`。
  - @position-try の CSSOM（CSSPositionTryRule）・@scope の CSSOM は rule object の model が要る（CSSRule の class は無い）。

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
