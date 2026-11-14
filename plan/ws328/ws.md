<!-- awesome-plan project=zedbsd record=ws328 -->

# WS328 libbrowser のベクトル描画（path の塗りと線、CSS の角丸・gradient など）と、それを使った SVG の描画

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-15 p001 の設計 cleared、header の基点 57bf94e62f。実装は p002・p017 から（標準廉価実装手順）。sources.mk の差分（U5）と U1〜U4 はユーザーへ）
- 目標: libbrowser の描画（`userland/desktop/libbrowser/paint/`）にベクトル描画を足し、それで CSS の描画（角丸・gradient・影など）と、libbrowser の中の SVG の描画を作る。既存の library は使わない（libbrowser は単体の library）。

## ユーザーの決定（要点）

- 2026-10-30 ユーザーから、次の趣旨の指示を受けた: CSS の描画のためのベクトル描画が libbrowser に無ければ内部に作り、それを使って libbrowser の中に SVG の描画を作る。WebP・AVIF は後で libwebp-compat のような自前の実装にしたいが、安い model で H.264 の decoder が作れるかの実験の結果を待つ。
- 2026-10-30 ユーザーから、WS328 は Vulkan で描画する形にする、という趣旨の決定を受けた → path の塗り・線・gradient などの描画は Vulkan（GPU）で行う（CPU の参照の描画との関係は設計で示す）。

## 今の状態（2026-11-14 P4 が p001 で確かめ直した。2026-10-30 の Q1 の調べを置き換え）

- 描画の display list（`paint/paint.h`）の種類は RECT・TEXT・CLIP・UNCLIP・IMAGE・BACKDROP・GROUP・GROUP_END・FRAME。角丸は pixel 行ごとの矩形の近似、box-shadow は 10 枚の半透明の重ね（inset は描かない）、linear gradient は CPU で bitmap にして IMAGE の item（`paint/gradient.c`）。radial・conic は解析も無い。GPU（vulkan.c）は filter の group を描かない。
- SVG は `page/svg.c`（ws074-p228 以後）が inline の `<svg>` と `<img>` の SVG を CPU で bitmap（一辺 512 px まで）に描く（図形の塗り、近似の線、clip-path、use、gradient は最初の stop の色）。
- 描画は WS345 の形: display list を style の thread で作り、確定した `paint_scene` を描画の thread で描く。build system は作り直された（libbrowser の source の一覧は `sources.mk`）。
- 詳しい比べは [design.md](design.md) §2。

## Phase（2026-10-30 Q1 が標準廉価実装手順の形に）

2026-10-30 ユーザーから、WS328 に標準廉価実装手順を適用して Phase を書く、という趣旨の指示を受けた。手順は [docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)。設計の担当は Opus 5.5、実装の担当は Sonnet 5.5 high を Phase ごとに別の worktree で。
基づく標準: SVG 2（W3C）、CSS Backgrounds and Borders 3（border-radius・box-shadow）、CSS Images 3（gradient）、Compositing and Blending 1。WPT の該当の試験を照合に使う。
module と実装の Phase の分け方は p001 が決めた（[design.md](design.md) §5・§11）。WS074 との file の調整は、WS074 が止まっているので今は要らない（WS074 の再開の時に design.md §12 の形を前提にする）。

| Phase | 目的 | 担当 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| [ws328-p001](phase001/phase.md) | 設計（module の分け方、header と骨格、試験の道具、実装の Phase、design-reviewer） | Opus（P4） | cleared（2026-11-15 Q1、3c892233f8 を main に merge c9d22fa09d。2 回の design-reviewer の 83 件を全部反映、header の基点 57bf94e62f、check-skeleton・harness PASS） | — |
| [ws328-p002](phase002/phase.md) | vec-base・vec-path（行列・path・弧・bbox・atan2・exp） | Sonnet | cleared（2026-11-15 Q1、907f2af3ba を main に merge 4360704e39。test-base 204220・test-path 348081 checks、cc・clang・ASan・UBSan PASS、check-skeleton PASS、壊す 30 通り中 28 FAIL・2 は等価） | p001 |
| [ws328-p003](phase003/phase.md) | vec-flatten（平坦化） | Sonnet | cleared（2026-11-15 Q1、d807992aaf を main に merge。test-flatten 93774 checks、cc・clang・ASan・UBSan、check-skeleton PASS、壊す 21 通り全部 FAIL） | p002 |
| [ws328-p004](phase004/phase.md) | vec-stroke（線の輪郭・join・cap・dash） | Sonnet | cleared（2026-11-15 Q1、4e5cc05296 を main に merge。test-stroke cc 28392433・clang 27637132 checks、ASan・UBSan、check-skeleton PASS、壊す 42 通り中 41 FAIL・1 は等価） | p003 |
| [ws328-p005](phase005/phase.md) | vec-raster（coverage の tile） | Sonnet | cleared（2026-11-15 Q1、d155fe2ba2 を main に merge。test-raster 1055903 checks、cc・clang・ASan・UBSan、check-skeleton は Q1 が toolchain のある worktree で check 6 まで流す、壊す 44 通り中 40 FAIL・4 は等価） | p003 |
| [ws328-p006](phase006/phase.md) | vec-gradient（補間・hint・ramp・t の計算） | Sonnet | cleared（2026-11-15 Q1、18eec6e3ce を main に merge。test-gradient 277264 checks、cc・clang・ASan・UBSan、check-skeleton PASS（check 6 込み）、壊す 42 通り中 40 FAIL・2 は等価） | p002 |
| [ws328-p007](phase007/phase.md) | vec-rrect（角丸・ring・辺・影） | Sonnet | cleared（2026-11-15 Q1、2b93739794 を main に merge。test-rrect 419610 checks、cc・clang・ASan・UBSan、影は数値積分と最大差 0.0088、check-skeleton は Q1 が check 6 まで流す、壊す 34 通り全部 FAIL（4 は試験を足して）） | p002 |
| [ws328-p008](phase008/phase.md) | svg-values（値の文法・viewBox の変換） | Sonnet | cleared（2026-11-15 Q1、0be12c1675 を main に merge。test-values 741102 checks、cc・clang・ASan・UBSan、check-skeleton は Q1 が check 6 まで流す、壊す 54 通り中 51 FAIL・残り 3 は試験の追加と死んだ確認の削除で処理） | p002 |
| [ws328-p009](phase009/phase.md) | svg-pathdata（`d`） | Sonnet | cleared（Q1 の受け入れ待ち。2026-11-15 P2 q2129、実装済み・試験 PASS: test-pathdata 391002 checks、cc・clang・ASan・UBSan、check-skeleton PASS（check 6 込み）、壊す 71 通り全て FAIL） | p008 |
| [ws328-p010](phase010/phase.md) | svg-style（表示の性質、祖先からの style） | Sonnet | in-progress（2026-11-15 P7 q2130） | p008 |
| [ws328-p011](phase011/phase.md) | svg-shape（図形の等価な path） | Sonnet | planned | p009・p010 |
| [ws328-p012](phase012/phase.md) | svg-pserver（gradient の要素） | Sonnet | planned | p006・p010 |
| [ws328-p013](phase013/phase.md) | svg-picture の核（root・g・図形・style・paint・opacity・上限） | Sonnet | planned | p011・p012 |
| [ws328-p029](phase029/phase.md) | svg-picture の use・symbol・入れ子の svg・switch | Sonnet | planned | p013 |
| [ws328-p014](phase014/phase.md) | svg-clip（clipPath） | Sonnet | planned | p013 |
| [ws328-p015](phase015/phase.md) | svg-draw（picture → sink） | Sonnet | planned | p004・p005・p014 |
| [ws328-p016](phase016/phase.md) | svg-bitmap（CPU の bitmap） | Sonnet | planned | p006・p015 |
| [ws328-p017](phase017/phase.md) | CSS の gradient の解析（radial・conic・repeating・hint） | Sonnet | cleared（2026-11-15 Q1、17826171e0 を main に merge。host-css-gradient 223/223（cc・clang、ASan・UBSan）、既存の host 試験 7 本 PASS、壊す 43 通り中 42 FAIL・1 は等価。linear の px の stop に zoom を掛ける直しは Q1 が採った） | p001 |
| [ws328-p018](phase018/phase.md) | paint の統合（新しい item、CPU の参照、coverage の cache、倍率、snapshot の深い写し、WPT の基準） | Opus | planned | p005・p006・p007 |
| [ws328-p020](phase020/phase.md) | Vulkan の pipeline（rrect・ring・影・gradient） | Opus | planned | p018 |
| [ws328-p026](phase026/phase.md) | Vulkan の path（coverage の atlas・tile の表・予算） | Opus | planned | p020 |
| [ws328-p027](phase027/phase.md) | 角丸の clip の下の矩形・glyph・画像、QEMU の Venus | Opus | planned | p026 |
| [ws328-p019](phase019/phase.md) | CSS の角丸の背景・border・角丸の clip・置換要素 | Opus | planned | p004・p027 |
| [ws328-p024](phase024/phase.md) | CSS の box-shadow | Opus | planned | p019 |
| [ws328-p025](phase025/phase.md) | CSS の gradient の背景 | Opus | planned | p017・p019 |
| [ws328-p021](phase021/phase.md) | SVG の統合（adapter・継承の口・inline の `<svg>`） | Opus | planned | p016・p026・p029 |
| [ws328-p028](phase028/phase.md) | `<img>`・背景の SVG、page/svg.c の rasterizer を消す | Opus | planned | p021 |
| [ws328-p022](phase022/phase.md) | 統合の試験（WPT・GPU と CPU・QEMU・実機の一覧） | Opus | planned | p024・p025・p028 |
| [ws328-p023](phase023/phase.md) | 規約の全文の見直し（書き方だけ） | Sonnet（conformance-reviewer） | planned | p022 |

Phase の番号はレビューで分けた物に新しい番号（p024〜p029）を付けたので表の順と違う。GPU の側（p020 → p026 → p027）を CSS・SVG の切り替えより先に main に入れる（design.md §11）。

## Resume point

- 2026-11-15 03:3x Q1: p005〜p008 cleared。Q1 が main（62f2d0737d）で run-unit all と check-skeleton（check 6 込み）を流して PASS。p008 の数・transform の書き方は仕様を文字どおり（`5.` は誤り、transform の関数の間は comma-wsp が要る。CSS Transforms 1 §7.2 の文法と同じ）を Q1 が確かめて採った。p006 の所見（焦点の半径 0 の放射で中心の点が塗られない）は p018 で決める。p018 以後は U1〜U5 の答えの後。

- 2026-11-15 Q1: p004 cleared（4e5cc05296、merge dbf5fc40d9）。header の疑問への Q1 の判断: (1) offset が dash の境にぴったりの時は SVG 2 §13.5.7 を文字どおりに当て、始めの長さ 0 の dash に round・square の cap の点を出す（実装のまま）。(2) 閉じた contour で最初の dash が長さ 0 なら継がない（実装のまま）。(3) vec-path.h の `VEC_LINES_VERTICES_MAX` の comment の「8 MiB」は約 24 MiB の誤り。header の凍結を崩さないため、最後の規約の見直しの Phase で直す。

- 2026-11-15 Q1: p003 cleared。header の持ち越し（再凍結の時、p023 で）: vec-flatten.h の `VEC_LINES_VERTICES_MAX` の comment「8 MiB of vertices」は struct vec_vertex 12 byte × 2^21 で 24 MiB（contour も 24 MiB）。動作には関わらない。次は p004（vec-stroke、P2 q2122）。

- 2026-11-15 Q1: p001 cleared（merge c9d22fa09d、基点 57bf94e62f）。次は p002（vec-base と vec-path）と p017（CSS の radial・conic の解析）を Sonnet で。p018 以後は U1〜U5 の答えの後。

- 2026-11-15 P4（q2098-i01）: design-reviewer の 2 回の結果（前の担当の起動分 High 7・Medium 15・Low 11 を Q1 の要約で、今回の起動分 重大 6・中 21・軽 23）を design.md §18 に記録し、全部を反映した（header・design.md・phase.md・user-questions・試験の道具）。**基点の案: 57bf94e62f**（header と design.md の反映の commit。p001 の cleared の時に Q1 が確かめてここを基点に決める）。Phase を分けて p024〜p029 を足した。user-questions に U5（sources.mk）を足し、U1 に案 C（stencil）と A の例外、U3・U4 に補足。次は p002 と p017（並べられる）、p002 の後に p003・p006・p007・p008。

- 2026-11-14 23:2x Q1: p001 の素案（f5d868d46c・7cc9eca796）を main に merge c315f3799f。design-reviewer の反映は P4 q2098。Q1 の判断: WS328 の統合の Phase は WS074 の host の試験を直してよい（WS074 は止めている）。`sources.mk`（make の断片）に .c を足す差分は Makefile と同じ扱いで、案としてユーザーへ。仕様の資料は main の build/ に写さず、担当の worktree の build/ に置く（前の worktree から写す）。

- 2026-11-14 P4（q2091-i01）: p001 の設計を作った（[design.md](design.md)、header と骨格、試験の道具、p002〜p023 の phase.md、[user-questions.md](user-questions.md)）。header の基点の SHA は p001 の cleared の時に Q1 がここに書く（実装の Phase は `BASE=<その SHA> sh plan/ws328/tests/check-skeleton.sh`）。次は p002・p006・p007・p008・p017 から（並べられる）。

- 2026-11-14 Q1: ユーザーの指示で着手（WS074 を今進めていないので file の調整は要らない）。P4 q2091: 乖離の確かめ → p001 の設計。

- 2026-10-30 Q1: 作成。WS074 と調整してから始める。

- 2026-10-30 Q1: ユーザーの指示で Phase を標準廉価実装手順の形にした。p001 の設計（Opus）から、着手の時期は未定（WS074 と調整）。

## 日程

2026-10-30 ユーザーから、WS074 を止めて WS328 を先に実装する、という趣旨の指示を受けた（Codex の解約で WS074 は Claude の管理下に移った後の判断）→ WS074 は止めたまま、WS328 を p001（設計、Opus）から進める。WS074 の再開は WS328 の後。
