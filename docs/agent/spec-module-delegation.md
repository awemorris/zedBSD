# 外部の標準仕様に基づく実装の進め方: module の設計（Opus）と実装の委託（Sonnet 5.5 high）

この文書は、エージェント（Q1・設計の担当・実装の担当）が読み、同じ手順を漏れなく再現するための標準の手順である。
人間向けの読みやすさより、手順・条件・成果物の網羅を優先する。

## 0. 用語

| 用語 | 意味 |
| --- | --- |
| Q1 | main の checkout の統括のセッション。計画・Queue・merge・判定を行う。 |
| 設計の担当 | module の設計の Phase を行うエージェント。model は Opus（定義 phase-runner）。 |
| 実装の担当 | module の .c と単体試験を書くエージェント。model は Sonnet 5.5、effort high。1 Phase = 1 回の起動。 |
| design-reviewer | 設計の敵対的レビューのエージェント。設計の担当が 1 本起動する。 |
| 仕様 | 外部の標準の文書（例: ITU-T H.264、ISO/IEC の規格、RFC、Khronos の仕様）。 |
| 基点の SHA | 設計の Phase が header を確定させた commit。実装の Phase は header がこの SHA から変わっていないことを照合する。 |

## 1. 適用の条件

次の全部を満たす時にこの手順を使う。

1. 作る物の振る舞いが外部の標準仕様で決まっている（解析・decode・encode・protocol・file の形式など）。
2. 仕様が手元に置ける（PDF・HTML・text）。版と節・表・式の番号で参照できる。
3. 全体を、入力と出力が明確な module に分けられる（純粋な関数・明確な状態の構造体）。
4. module の多くを host（OS の外）で単体試験できる。

満たさない時（設計を探りながら作る物、hardware の挙動を試しながら作る driver、UI）は、この手順を使わず普通の Phase で進める。

## 2. 全体の流れ

```
WS を立てる（Q1）
  → p001 設計の Phase（設計の担当 = Opus、1 回）
      module の分割・header（interface と仕様の comment）・.c の骨格・試験の道具・
      適合の試験の道・実装の Phase の表と各 phase.md・design-reviewer
  → Q1 が p001 を merge し、判断の問いに答える
  → 実装の Phase（実装の担当 = Sonnet 5.5 high、Phase ごとに 1 回、依存が満ちた物から並行）
      header を変えずに .c と単体試験を書く
  → Q1 が受け入れ（確かめ・merge・cleared）、次の Phase を起動
  → 統合・適合の試験・速さ・規約の見直しの Phase（Opus を推す）
```

## 3. WS を立てる（Q1）

ws.md に次を書く。

- 目標と範囲: 仕様の名前と版、対応する profile・level・形式。範囲の外（例: interlace、10 bit）を明記する。範囲の外の入力をどう扱うか（誤りで断る、次の backend に回す）も書く。
- ユーザーの決定の原文（この手順を使う指示を含む）。
- 置き場所（source の directory、関数の接頭辞）。
- 既存の library・backend の中での位置（例: libmedia の backend の順）。
- Phase の表は p001 だけを置き、p002 以降は p001 が作る。

## 4. 設計の Phase（p001、設計の担当 = Opus）

### 4.1 入力の資料

- 仕様の本文を、消えない場所に置く。git には入れない（著作物）。例: main の checkout の `build/sources/specs/<名前>.pdf`。
  - 置き場所の path と SHA-256 を、設計の文書と全ての phase.md に書く。
  - PDF の頁と印刷の頁の換算（例: 印刷の頁 N = PDF の頁 N + 22）を書く。
  - 読み方: `pdftotext -f N -l M`、表は `pdftotext -layout`、座標は `pdftotext -bbox`。折り返した表の cell は画像（Read の tool で PDF の頁）を見る。
- 適合の試験の data（公式の conformance の bitstream など）の取得の道と、cache の置き場所（git の外、消えない共有の場所。例: `build/sources/<名前>-conformance/`）。

### 4.2 module の分け方

- 仕様の節の構造と、data の流れ（入力 → 解析 → 状態 → 出力）に沿って分ける。
- 依存は一方向にする。循環させない。
- 1 module = 1 header（＋ 1 .c）。構造体だけの header（.c 無し）も可。
- **1 実装 Phase = 実装の担当の 1 回の起動で済む大きさ**: header 1〜3 個、目安は仕様の 1〜2 節。
- 表の値（数百行の data）は、独立した module か Phase に分ける。値の照合を独立に行うため。
- 共有する補助（bit の読み、誤りの値の約束、上限の定数）は最初の module に置く。

### 4.3 header の書き方（この手順の要）

header の comment が仕様である。実装の担当は header と仕様書だけを読んで書く。

- **定数**: 名前・値・単位・出典（仕様の節・表）。上限は仕様の level の表などから決め、その根拠を書く。
- **構造体**: 各 field の意味・単位・範囲・初期値・誰が書き誰が読むか。所有（誰が確保し誰が解放するか）と寿命。
- **関数**: 各関数の直前の comment に次を全部書く。
  1. 何をするか（1〜3 文）。
  2. 入力: 各引数の意味・範囲・NULL を許すか。
  3. 出力: 戻り値と、書く引数。成功の時と誤りの時のそれぞれで、出力の状態がどうなるか。
  4. 前提: 呼ぶ前に必要な状態。
  5. 誤り: 返す誤りの値（例: EINVAL・ENOTSUP・ENOMEM・ENOBUFS・EBUSY）と、それぞれの条件。
  6. 仕様の該当: 節・表・式の番号（版を明記）。
  7. 信用できない入力: 入力が壊れていても、範囲の外を読み書きせず、必ず終わること。
- **共通の約束**（最初の header に置く）:
  - 誤りの値の意味（不正な入力 = EINVAL、範囲の外の機能 = ENOTSUP など）。
  - 算術の未定義の動作の禁止（積は広い型で、負の値の右 shift は floor の補助関数で、左 shift は乗算で、範囲の中間値はその場で clamp）。
  - 使ってよい C の版と規約（このプロジェクトでは ANSI C、plan/coding-style.md）。
- header の先頭に出典を書かない。出典は該当の block の comment に書く（Guardrail の出典の規則）。
- source の comment に開発の管理情報（WS・Phase・Bug の番号、plan/ への参照、日付）を書かない。

### 4.4 .c の骨格

- 各 module の .c を作る。include と、全関数の本体を「未実装の印」（例: `return ENOSYS;`、void の関数は何もしない）にする。
- build が通る形にする（Makefile に source を足す）。既存の表（例: backend の表）には、まだ登録しない。

### 4.5 試験の道具（設計の担当が作り、実装の担当は変えない）

- **単体試験の実行の script**（例: `run-unit.sh NAME...|all`）: host の compiler で、ASan・UBSan を付けて build し流す。最後の行を `run-unit: PASS` か `FAIL` にする。
- **骨格の照合の script**（例: `check-skeleton.sh`、`BASE=<基点の SHA>`）:
  - header が基点の SHA から変わっていないことを照合する。
  - 各 header が単独で compile できること、全 .c が 2 つの compiler（cc と clang）で警告なしに通ることを確かめる。
- **試験の補助の header**:
  - 判定の macro（`check.h` の類）。
  - **逆方向の独立の実装**（例: bit を書く `bitwriter.h`、算術符号の encoder `cabacwriter.h`）。仕様の図・式から、decoder の code を使わずに書く。試験の入力を作り、往復で確かめるために使う。
- **適合の試験**:
  - 公式の test の一覧（名前・profile・期待の hash の出所）。
  - 取得の script（パラメータの無い GET、SHA-256 の照合、cache への保存）。
  - 照合の方法（出力の hash と、公式の参照の出力の hash）。

### 4.6 実装の Phase の表と phase.md

ws.md に Phase の表（Phase・module・単体試験・受け入れ・依存）を足す。依存は module の依存から決める。
各 phase.md に次の節を必ず書く。実装の担当は phase.md だけを手順として読む。

1. **読む物**: AGENTS.md の該当の節、規約の全文、設計の文書の該当の節、実装する header の path、仕様の PDF の path と SHA-256、読む節・表・式の番号。
2. **作る物**: .c の path、単体試験の path、phase.md の「結果」の節、ws.md の表のこの行。
3. **試験の項目**: 確かめる場合の列挙。正常・境界・誤りの入力・途中で切れた入力・乱数の入力（ASan・UBSan で落ちない）・往復。期待の値の作り方（仕様の表か手の計算、他の実装の出力を使わない）。
4. **注意**: その module に固有の落とし穴（例: 既存の関数の癖、仕様の版の違い）。
5. **変えてはいけない物**: 全ての header、他の module の .c、Makefile、試験の道具、HAL・toolchain・共有の build。header の不備を見つけた時は直さずに止め、どの header のどの関数で何が足りないかを返す。
6. **確かめの command**: 単体試験（この module と all）、骨格の照合（`BASE=<基点の SHA>`）、target の build（警告 0）、`git diff --check`。
7. **終わりの条件**: 上の command が全部 PASS（出力を自分で読んで確かめる）、規約の checklist で自分の diff を見直した、表の block ごとの出典の comment、commit（`git commit -m WIP -- <path>`、新しい file は先に `git add` か `git add -N`）、結果の節（日付は host の `date`、command・件数・未実施・自分で決めた点）、ws.md の行は「実装済み・試験 PASS」（cleared は Q1 が決める）。

最後に置く Phase:
- 統合（module をつないで全体の decode）、適合の試験、既存の library への組み込み、速さ: 推論が多いので Opus を推す。
- 規約の全文の見直し: header の持ち越しの直し（実装の間に見つかった header の comment の誤りなど）もここで行う。

### 4.7 design-reviewer

設計の担当は design-reviewer を 1 本起動し、header・設計の文書・phase.md を敵対的に読ませる。
観点: 仕様との食い違い、所有と寿命、誤りの扱い、信用できない入力での範囲の外の読み書き、未定義の動作、module の境の過不足、Phase の大きさと依存、試験の項目の漏れ。
指摘は全部を設計の文書に表で記録し、反映するか理由を付けて見送る。

### 4.8 Q1 に返す物

commit の SHA（骨格、header の確定の SHA = 基点、plan）、module の一覧、Phase の表、確かめた事、Q1 の判断が要る点（範囲の解釈、置き場所など）。

## 5. 実装の Phase（実装の担当 = Sonnet 5.5 high）

### 5.1 Q1 の起動の手順

1. 依存の Phase が全部 main に merge され cleared になっていることを確かめる。
2. Phase ごとに新しい worktree と branch を main から作る（例: `/home/awe/zedBSD-worktrees/s-<WS>-<Phase>`、branch `agent/s-<WS>-<Phase>`）。依存の満ちた Phase は並べて起動してよい（同時の数は Q1 が決める。目安 3〜5）。
3. エージェントを `subagent_type: general-purpose`、`model: sonnet`、`effort: high`（ユーザーが Sonnet 5.5 High を指定した時）で、background で起動する。
4. prompt は 5.2 の型に、Phase の名前と worktree と、その Phase に固有の強調点（仕様の節）を入れる。

### 5.2 prompt の型

```
You are an implementation agent for the zedBSD project, <WS> phase <pNNN> (<module の説明>).
Your worktree is <path> (branch <branch>; it already contains the finished <済んだ Phase の一覧>). Work only there.

Read and follow exactly: plan/<ws>/phase<NNN>/phase.md in that worktree. It lists what to read (AGENTS.md
sections, plan/coding-style.md, the design document, the headers that are the specification, and the spec PDF
at <path>, SHA-256 starting <先頭>; use `pdftotext -f N -l M`, and view page images for tables that wrap),
what to write, the tests, what you must not change (all headers, other modules, Makefile, test tools), the
verification commands, and the completion conditions.

Key rules: implement only the files and unit tests named in phase.md; write from the spec (<節>); table values
are data cited per block and cross-checked against the PDF; expected values in tests come from the spec or hand
calculation, never another implementation; do not copy code, comments or structure from other implementations
(<既知の実装の名前>); no development-management info in source comments; no undefined behaviour on hostile
input; commit with exactly `git commit -m WIP -- <paths>` (git add new files first); never push; never delete
files with rm. If a header is wrong or missing something, do not edit it: stop and report exactly what is
needed. Where the header is silent, decide, and record the choice in the phase.md result section. If phase.md
contradicts the spec, follow the spec and report it. After the tests pass, deliberately break the
implementation a few ways (outside git) to confirm the tests catch it, then restore.

When done, report back: commit SHA, the commands you ran and their final lines, judgement calls, what was not done.
```

### 5.3 実装の担当が守ること（prompt と phase.md に重ねて書く）

- header を変えない。不備は止めて報告する。
- header が黙っている所は自分で決め、phase.md の結果の節に列挙する（Q1 が見て確認する）。
- phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 表の値は仕様から取り、**独立した 2 つの方法で照合**する（例: `pdftotext -layout` と `-bbox` の座標、text と頁の画像）。text で取れない値（折り返した cell）は画像から取り、それが照合の弱点であることを記録する。
- 試験の期待の値は、仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 試験が通った後、実装をわざと数通りに壊し（git の外で）、試験が FAIL することを確かめ、元に戻す（`cmp` などで戻ったことを確かめる）。見逃しがあれば試験を足す。
- 規約の checklist で自分の diff を見直す。
- 報告: commit の SHA、実行した command と最後の行、自分で決めた点、未実施。

## 6. Q1 の受け入れ

実装の担当の報告ごとに:

1. `git cat-file -t <SHA>` で commit であることを確かめる。
2. main の checkout で `git merge --no-ff --no-commit <SHA>`。競合（よく起きるのは ws.md の Phase の表の行）は、両方の行の新しい方を取って解く。conflict marker が残っていないことを確かめて `git commit -m WIP`、`git log -1 --format='%h %p'` で親が 2 つ。
3. phase.md の状態と ws.md の行を `cleared（日付 Q1、main <merge の SHA>。<試験の名前と件数>）` にする。
4. 報告の「自分で決めた点」を読み、仕様に反する物があれば次の Phase か直しの Phase で扱う。
5. 報告で見つかった食い違いの扱い:
   - phase.md や設計の文書の誤り（例: 仕様の表の性質の思い違い）は、仕様を正として phase.md に記録する。
   - header の comment の誤りは、実装の間は header を固定する（骨格の照合の BASE が変わるため）ので、規約の見直しの Phase に持ち越して記録する。
   - 試験の道具の小さな不具合（例: 補助の header の未定義の動作）は Q1 がその場で直し、単体試験の all を流して確かめる。
6. 依存が満ちた次の Phase の worktree を main から作り、起動する。
7. 使い捨ての出力（worktree の build/tmp など）は Q1 が消す。

## 7. 出典と license の規則（全ての Phase）

- 仕様の本文から独立に書く。他の実装（例: FFmpeg・openh264・JM・x264・Mesa）の code・comment・構造・名前を写さない。
- 表・定数は data として使ってよい。block ごとに直前の複数行の comment で「Values referenced: <仕様の名前と版> Table <番号>」の形で出典を書く。GPL の file から取った値も、data として出典を示せば使ってよい（ただし code・comment・構造は写さない）。
- 出典は file の先頭でなく、該当の block に書く。
- 名前（型・定数・関数）は独自の物にする。仕様の名前（syntax element の名前など）は使ってよい。他の実装の内部の名前に似たら変える。
- 仕様の PDF は著作物なので git に入れない。

## 8. 実績と教訓（WS326、H.264 の software の decoder、2026-10-30）

- 設計（p001）: Opus が 26 header・25 .c の骨格・試験の道具・24 の実装 Phase を作り、design-reviewer の 23 件を反映した。
- 実装: Sonnet 5.5 high が Phase ごとに 1 回の起動で、header を変えずに通した（最初の 10 Phase で header の変更は 0）。単体試験の件数は Phase ごとに数百〜20 万。
- 教訓:
  - phase.md に書いた仕様の性質が誤っていたことがある（「全ての VLC の表の Kraft の和が 1」は誤り、使わない符号のある表がある）。実装の担当は仕様に従って直し、報告した。phase.md の記述は仕様を言い換えた物で、仕様が正。
  - `pdftotext` は表の値を落とすことがある（例: 図の中の 1 つの値）。表は画像でも照合する。
  - わざと壊す確かめで、試験の見逃し（符号の誤り 1 か所）が見つかり、試験を足して塞いだ。この確かめは必須にする。
  - 新しい file は `git commit -- <path>` の前に `git add`（か `git add -N`）が要る。
  - 並行の Phase は main の同じ時点から作るので、ws.md の表の行が競合する。Q1 が merge の時に解く。
  - 試験の補助の header にも未定義の動作がありうる（32 bit の shift）。実装の担当が見つけたら Q1 が直す。
