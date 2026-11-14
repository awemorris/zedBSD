<!-- awesome-plan project=zedbsd record=ws328-design -->

# WS328 の設計: libbrowser のベクトル描画（path の塗りと線、CSS の角丸・gradient・影）と SVG の描画

- 書いた者: P4（ws328-p001、Queue q2091-i01）。日付は host の `date`（2026-11-14）。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。module（vector/・svg/）は Sonnet、統合（paint・list・Vulkan・page）は Opus。
- この文書は p001 の成果で、header（`userland/desktop/libbrowser/vector/vec-*.h`・`svg/svg-*.h`）と同じ commit で凍結する。header の comment が仕様で、この文書は分け方・理由・統合の設計を書く。

## 1. 目的と範囲

目的（[ws.md](ws.md)）: libbrowser の描画にベクトル描画を足し、それで CSS の描画（角丸・gradient・影）と libbrowser の中の SVG の描画を作る。既存の library は使わない。描画は Vulkan（GPU）で行う（2026-10-30 のユーザーの決定）。

範囲の中:

| 分野 | 中身 | 基づく標準 |
| --- | --- | --- |
| path の幾何 | move・line・二次と三次の Bézier・楕円弧（三次に変換）・close、affine 変換、tight な bounding box、平坦化 | SVG 2 §9・付録 B.2、CSS Transforms 1 §9.1・§14 |
| 線 | 幅、cap（butt・round・square）、join（miter・round・bevel、miterlimit）、dash（配列・offset・pathLength） | SVG 2 §13.5 |
| 塗り | nonzero・evenodd、anti-alias の coverage（16 本の副走査線で x は厳密） | SVG 2 §13.4.2 |
| gradient | 線形・放射（焦点の円付き）・円錐、pad・reflect・repeat、CSS の stop の fixup と hint、premultiplied（CSS）と straight（SVG）の補間 | CSS Images 3 §3、CSS Images 4 §3.3・§3.5、SVG 2 §14.2、HTML §4.12.5.1.10、CSS Color 4 §12 |
| CSS の装飾 | 角丸（楕円の角、半径の縮小）の背景・border（色が辺ごとに違う物、dashed・dotted・double・groove・ridge・inset・outset）、box-shadow（外と inset、blur と spread）、gradient の背景（linear・radial・conic・repeating）、角丸の overflow の clip と置換要素の角丸 | CSS Backgrounds 3 §3・§4・§6.1、CSS Images 3・4 |
| SVG | svg（入れ子）・g・defs・use・symbol・switch、rect・circle・ellipse・line・polyline・polygon・path、fill・stroke の性質、transform、viewBox・preserveAspectRatio、linearGradient・radialGradient（href の継承）、clipPath、opacity、inline の `<svg>`・`<img src=*.svg>`・CSS の背景の SVG | SVG 2 §3・§5・§8・§9・§10・§13・§14、CSS Masking 1 §5.1・§6 |
| 描画の道 | CPU の参照の描画（software.c）と Vulkan の描画（vulkan.c）が同じ display list を同じ式で描く（画素の比べ） | — |

範囲の外（後で。§14 の一覧）: SVG の text・image・marker・mask・pattern・filter・foreignObject、SVG に対する style sheet の cascade（`<style>` と頁の sheet）、HTML の箱の回転・skew の transform、CSS の clip-path の形（circle()・polygon()・path()）で HTML の中身を切る事、opacity・filter の group を GPU の layer で描く事（今は各 item の alpha を掛ける近似）、Canvas 2D、border-image、`<color-interpolation-method>`（oklab などの補間の色空間）、DPI の倍率（devicePixelRatio は今 1）。

## 2. 乖離の確かめ（2026-11-14、ws.md は 2026-10-30）

ws.md の「今の状態」と今の main（c5b8a10d24）の比べ。

| ws.md の記述（2026-10-30） | 今（2026-11-14） | 扱い |
| --- | --- | --- |
| display list の種類は RECT・TEXT・CLIP・UNCLIP・IMAGE だけ、角丸・gradient は無い | 種類は RECT・TEXT・CLIP・UNCLIP・IMAGE・BACKDROP・GROUP・GROUP_END・FRAME（WS074 の後の Phase）。角丸は `list.c` が各 pixel 行を矩形 1 つで塗る近似（`list_round_fill`）、border の角丸も行ごと（`list_round_borders`）、box-shadow は 10 枚の半透明の重ね（`list_shadow`、inset は描かない）、linear gradient（repeating も）は `paint/gradient.c` が CPU で tile の大きさの bitmap に描いて IMAGE の item にする。radial・conic の CSS の解析は無い。clip-path は inset() と clip: rect() だけ。transform は translate と scale だけ | ベクトルの item（§7）で置き換える。行の矩形・重ね・CPU の bitmap は消す（統合の Phase で、試験の期待の値も直す） |
| SVG は DOM・解析・大きさ・JS の bind だけで図形の描画は無い、`<img>` の SVG も無い | `page/svg.c`（2492 行、ws074-p228 以後）が inline の `<svg>` と `<img>` の SVG を CPU で bitmap（一辺 512 px まで）に描く。g・path（弧も）・rect・circle・ellipse・polygon・polyline の塗り、viewBox、transform、fill の性質、gradient は最初の stop の色だけ、線（join は全部 round の近似、dash 無し）、clip-path、use。file の先頭の comment は「線・clip・use は描かない」と古いまま | `page/svg.c` の描画を svg/ の module で置き換える（§10）。file の comment の古さは置き換えで消える |
| CPU の参照（software.c）と Vulkan（vulkan.c）が同じ display list を描く | 同じ。ただし GPU は BACKDROP・GROUP（filter）を描かない（CPU だけ）。矩形は端を整数の pixel に丸めて描く（`gpu_stage_rect` の `floorf(x + 0.5)`）。GPU の shader は i915 の native の compiler と Raspberry Pi の V3D の制約（gl_VertexIndex・instance の頂点入力・texelFetch・fragment の gl_FragCoord を使わない）で書かれている | 新しい shader も同じ制約で書く（§8.4） |
| （記述無し） | WS345: display list は style の thread で作り、確定した frame を `paint_scene` にして描画の thread（shell が作る）で記録・提出する。scene の item は scene の arena と保持した物だけを指す | ベクトルの item の data（平坦化した線・gradient の stop）は scene の arena に置く。coverage の tile は描画の thread で作り GPU の cache に置く（§6） |
| （記述無し） | WS346（layout の cache）・WS074 p263 の変更 | 描画の口は変わらない。影響なし |
| build: `make` の対象 | build system は作り直された（top の Makefile と `mk/platform-<機種>/`）。libbrowser の source の一覧は `userland/desktop/libbrowser/sources.mk`（browser-win32 の Makefile も読む） | 新しい .c を sources.mk に足すのは make の変更なので、差分の案を出して承認の後（§13）。それまで module は試験の script だけが compile する |
| 「WS074 と file の分け方を調整」 | WS074 は止まっている（Q1、2026-11-14） | 調整は要らない。WS328 は WS074 の file（`paint/`・`page/svg.c`・`page/images.c`・`css/`）を変える。WS074 の再開の時は WS328 の統合の後の形を前提にする（§12） |
| 画像の処理の方針（guardrail、2026-10-12） | frame ごとの経路で CPU が画素を触らない。CPU は内容が変わった時の一度だけの用意 | 今の gradient の CPU の bitmap・SVG の bitmap の再描画はこれに合わない（内容が変わった時に描くので一度だけの用意とは言えるが、大きな gradient を毎回 bitmap にする）。新しい形は §4 |

今の方針と矛盾し上書きの判断が要る記述は無かった（ws.md の Phase の見込みは「p001 の表で置き換える」と書かれている）。

## 3. 全体の形

```
          （style の thread）                                  （描画の thread）
CSS の style ─┐                                          paint_scene ──► software.c（CPU の参照）
               ├─► paint/list.c ──► display list ──► scene ─┤
SVG の DOM ──► svg/（picture）─┘   （path の item は                     └► vulkan.c（GPU）
                                    平坦化した線を arena に）                  │
                                                                              ├ 角丸・影・gradient: shader の式
            vector/（純粋な計算: path・平坦化・線・coverage・gradient・角丸）   └ path: coverage の tile を texture に cache
```

- **vector/**（`vec_` 接頭辞）: 数値と memory だけを扱う純粋な module。engine の header を include しない（check-skeleton.sh の 3 で確かめる）。host で単体試験する。
- **svg/**（`svg_` 接頭辞）: SVG の値の解析・style・図形・paint server・clip・picture（描く命令の列）・描画（sink へ渡す）・CPU の bitmap。DOM は `struct svg_tree`（関数の表）越しに読むので、host の試験は小さな markup の木（`plan/ws328/tests/unit/svg-test-tree.h`）で行う。
- **paint/**: display list に新しい item（§7）を足し、software.c と vulkan.c がそれを描く。CPU の参照と GPU は vector/ の同じ関数・同じ式を使う。
- **page/**: inline の `<svg>`・`<img>` の SVG・背景の SVG を svg/ に繋ぐ。`page/svg.c` の自前の rasterizer は消す。

依存は一方向: vector ← svg ← page・paint。vector と svg は libbrowser の他の部分を知らない。

## 4. GPU と CPU の分担（ユーザーの決定「Vulkan で描画」と guardrail の「画像の処理は GPU で」）

frame ごとに CPU が画素を触らない。描画の各 pixel の色は GPU の fragment shader が出す。

| 描く物 | GPU の frame ごとの仕事 | CPU の仕事（内容が変わった時だけ） |
| --- | --- | --- |
| 角丸の背景・border の ring・辺ごとの色の境 | shader が `vec_rrect_coverage`・`vec_rrect_ring_coverage`・`vec_rrect_side_share` と同じ式で coverage を出す | 無し（item の数値だけ） |
| box-shadow（外・inset、blur・spread） | shader が `vec_rrect_shadow` の式（erf の積と角の帯の数値積分）で coverage を出す | 無し |
| gradient（線形・放射・円錐） | shader が各 pixel の t を `vec_gradient_parameter`・`vec_gradient_spread` の式で出し、ramp の texture を引く | ramp（最大 2048 texel の 1 行）を `vec_gradient_ramp` で作り texture に置く（item ごとに一度、内容の hash で cache） |
| 一般の path（SVG の図形・線の輪郭・dashed などの border・clip-path） | coverage の tile（16×16 の 8 bit）を texture から引き、paint（色か gradient）を掛ける。中が全部覆われた tile は矩形として描く | 平坦化と線の展開は list を作る時（style の thread）。coverage の tile は描画の thread が cache に無い時だけ `vec_rasterize` で作る（scroll では作り直さない） |

coverage を GPU の shader で計算しない理由（技術の判断、ユーザーへの確かめは [user-questions.md](user-questions.md) の U1）:
1. zedBSD の自前の driver（i915 の native の compiler、V3D、PowerVR、GHCI）は shader の機能がまちまちで、今の display の shader もそれに合わせて制限している（texelFetch 無し、instance の頂点入力無し）。path の coverage を GPU で出すには、辺の data を storage buffer か浮動小数の texture で渡し、fragment で辺を回す loop か compute shader が要り、どの driver でも動く保証が無い。
2. 文字の glyph も CPU で一度描いて atlas に置き GPU が合成する。同じ形で path も扱えば、CPU の参照と GPU の coverage が bit 単位で一致する。
3. frame ごとに CPU が画素を触らない（cache が当たれば CPU は何もしない）ので guardrail に合う。
4. 後で driver の機能が揃えば、tile の coverage を GPU で作る形に替えられる（item の形は変えずに vulkan.c の中だけで）。

## 5. module の表

| header | 中身 | 仕様 | 実装 Phase |
| --- | --- | --- | --- |
| vector/vec-base.h | 点・矩形・affine 行列（合成・逆・適用・最大の伸び・変換関数）、誤りの値と算術の約束 | SVG 2 §8.5、CSS Transforms 1 §9.1・§14 | p002 |
| vector/vec-path.h | path の builder（誤りは sticky）、弧 → 三次、矩形・角丸矩形・楕円、変換、view、tight な bbox | SVG 2 §9.3・§9.5.1・付録 B.2・§10.2・§10.4・§8.10 | p002 |
| vector/vec-flatten.h | 折れ線（contour・頂点の CORNER の印）、平坦化（二階差分の上界で分割数）、変換、bbox | SVG 2 §9.3.6・§9.3.7・§9.5.3 | p003 |
| vector/vec-stroke.h | 線の輪郭（線分の矩形・join・cap・dash を正の向きの多角形の和として） | SVG 2 §13.5.3〜§13.5.7・§9.5.3 | p004 |
| vector/vec-raster.h | coverage（副走査線 16 本 × x の厳密な区間）、tile（16×16）と全被覆の run、積と和 | SVG 2 §13.4.2、CSS Masking 1 §5.1・§6.1 | p005 |
| vector/vec-gradient.h | stop の fixup（CSS・SVG）、repeating の写し、色の補間（premultiplied／straight、hint）、平均、ramp、t の計算（線形・二円の放射・円錐）、spread、標本 | CSS Images 3 §3.1〜§3.4、CSS Images 4 §3.3・§3.5、SVG 2 §14.2、HTML §4.12.5.1.10、CSS Color 4 §12.1・§12.3 | p006 |
| vector/vec-rrect.h | 角丸矩形の正規化・inset・spread、coverage・ring・辺の分け前・erf・blur の影 | CSS Backgrounds 3 §4.1〜§4.5・§6.1.1・§6.1.2 | p007 |
| svg/svg-tree.h | DOM を読む関数の表（.c 無し） | SVG 2 §5.5（href） | （p008 と共に） |
| svg/svg-values.h | 数・長さ・単位・list・transform の属性・viewBox・preserveAspectRatio・viewBox の変換・url・paint | SVG 2 §4.2・§8.2・§8.6・§8.7・§8.9・§13.2、CSS Transforms 1 §7.2・§7.3、CSS Values 3 §5 | p008 |
| svg/svg-pathdata.h | `d` の文法と意味、誤りの扱い | SVG 2 §9.3・§9.5 | p009 |
| svg/svg-style.h | 表示の性質（継承・初期値）、表示の属性と style 属性 | SVG 2 §6.6・§13・§3.2.3・§3.6.1、CSS Masking 1 §5.1・§6.2 | p010 |
| svg/svg-shape.h | 図形の要素の等価な path | SVG 2 §10・§9.2 | p011 |
| svg/svg-pserver.h | gradient の要素 → vector の gradient（href の継承、単位、変換、stop） | SVG 2 §14.1.1・§14.2・§8.11 | p012 |
| svg/svg-picture.h | 描く命令の列（fill・stroke・group）、木の walk（svg・g・use・symbol・switch）、style・paint・clip の解決 | SVG 2 §3・§5・§8.8・§13.8 | p013 |
| svg/svg-clip.h | clipPath → picture の clip（和、clip-rule、単位、入れ子） | CSS Masking 1 §6.1・§6.2 | p014 |
| svg/svg-draw.h | picture → device の折れ線と paint と clip を sink へ | （上の組み合わせ） | p015 |
| svg/svg-bitmap.h | CPU の bitmap への描画（source-over、group の layer） | Compositing and Blending 1 §5.1・§8.2・§9.1.4 | p016 |

module の依存（実装の Phase の依存は §11 の表）: vec-base ← vec-path ← vec-flatten ← vec-stroke・vec-raster；vec-base ← vec-gradient・vec-rrect；svg-tree・vec-base ← svg-values ← svg-pathdata（vec-path）・svg-style（vec-raster・vec-stroke の enum）← svg-shape・svg-pserver（vec-gradient）← svg-picture ← svg-clip；svg-picture・vec-flatten・vec-stroke・vec-raster ← svg-draw ← svg-bitmap（vec-gradient）。

## 6. 共通の約束（vec-base.h の file の comment が正）

- 誤りの値: EINVAL（壊れた入力: NaN・無限・範囲の外の座標・負の大きさ・逆行列の無い行列）、ENOBUFS（固定の上限の超え）、ENOMEM、ENOTSUP（形は正しいが扱わない物）。骨格は ENOSYS。
- 座標の上限 `VEC_COORD_MAX` = 2^24（float で 1 pixel が区別できる範囲）。全ての上限（点の数・線分の数・raster の一辺）は header の定数で、超えた入力は断る（作業量の上限も兼ねる）。
- 算術: float。入力は `vec_finite` で確かめ、割り算は除数を、平方根は引数を先に確かめる。float → 整数は範囲に clamp してから。
- 所有: builder の struct（`vec_path`・`vec_lines`・`vec_coverage`・`svg_picture`・`svg_server`）は realloc の配列を持ち、`*_release` で放す。読む側は view か const の struct を取る。scene の arena に置くのは統合の側（paint）の仕事で、module は arena を知らない。
- 信用できない入力: 全ての reader は配列の数の整合を自分で確かめる（`vec_path_view_check`・`vec_lines_check`）。壊れた入力でも範囲の外を読み書きせず、上限で必ず終わる。
- C: libbrowser は `-std=c11` で build される（`userland/base/package.mk`）。新しい code は ANSI C の書き方（宣言は関数の先頭、`for` の中で宣言しない）で、規約（plan/coding-style.md）の全文に従う。

## 7. display list への追加（paint.h、統合の Phase p018 が当てる案。module の header ではないので凍結しない）

```c
enum paint_kind {
	...,
	/* A filled path: item->vector->path (polylines in pixels from (x, y)) with its paint, rule and clips. */
	PAINT_PATH,
	/* A rounded rectangle filled, or one side of a border ring (item->vector->rrect). */
	PAINT_RRECT,
	/* A box shadow: a blurred rounded rectangle, outside or inside a rounded box (item->vector->shadow). */
	PAINT_SHADOW
};

/* What a vector item paints with: a color (the item's color) or a gradient whose stops live in the list's arena. */
struct paint_fill {
	int gradient;                    /* 0: item->color; 1: the gradient */
	struct vec_gradient shape;       /* to_gradient maps the item's pixels (from its x, y) to gradient space */
	uint64_t key;                    /* hash of the stops and the spread, the ramp's cache key */
};

/* The vector part of an item (in the list's arena; the item points at it). */
struct paint_vector {
	struct paint_fill fill;
	/* PAINT_PATH */
	struct vec_lines path;           /* vertices and contours in the arena, pixels from the item's (x, y) */
	enum vec_fill_rule rule;
	const struct vec_lines *clip;    /* an SVG clip met into the coverage (NULL for none) */
	enum vec_fill_rule clip_rule;
	uint64_t key;                    /* hash of the geometry, the rule and the clip: the coverage's cache key */
	/* PAINT_RRECT */
	struct vec_rrect rrect;          /* in pixels from the item's (x, y) */
	struct vec_rrect inner;          /* a ring's inner edge (ring set) */
	float widths[4];
	int ring;
	int side;                        /* -1 for all sides, else the side whose share is painted */
	/* PAINT_SHADOW */
	float sigma;
	int inset;                       /* 0: outer, painted outside box; 1: inset, painted inside box */
	struct vec_rrect box;            /* the border box (outer) or padding box (inset) the shadow is cut by */
};

struct paint_item { ...; const struct paint_vector *vector; };
```

- 位置: item の (x, y) は今と同じく document の layout の単位。vector の幾何は item の (x, y) を原点とした pixel。`scene_resolve`（frame の移動）は (x, y) だけを動かせばよい。
- 画素の位相: CPU と GPU は item の原点を scroll の後に整数の pixel に丸め（`floorf(px + 0.5)`、今の矩形と同じ）、その位相 0 で描く。だから coverage の tile は scroll で作り直さない（cache の key に位相が入らない）。角丸矩形と影も同じ丸めの原点から測る。
- clip: PAINT_CLIP の item に角丸（`rounded`・`radius`、今は BACKDROP が使う field）を持たせ、`paint_clips` の top に「矩形 ∩ 一番内側の角丸矩形 1 つ」を持つ。外側にもう一つ角丸の clip がある時は、その矩形だけを使う（既知の差。§14）。vector の item・矩形・glyph・画像は角丸の clip を shader の式（`vec_rrect_coverage`）で掛ける。
- SVG の clip は path の item ごとの `clip`（clip の折れ線）で、coverage を作る時に `vec_coverage_intersect` で掛ける（GPU の layer を使わない）。clip の和（child の clip-rule が違う時）は `vec_coverage_unite`。
- 透明度の group: SVG の opacity と CSS の opacity は、今と同じく list が item の alpha に掛ける（重なりでは近似）。`svg_picture_render`（CPU の bitmap）だけは本物の layer で描く。
- `paint_dump`（試験の text の dump）: 新しい種類ごとに 1 行（種類・原点・bbox・paint の色か gradient の種類と stop の数・key）。

## 8. Vulkan の描画（p020、Opus）

### 8.1 pipeline

今の display の pipeline（矩形・glyph・画像、instance 5 個の vec4）はそのまま残し、ベクトル用の pipeline を 1 本足す。同じ render pass の中で list の順に描き、同じ pipeline の続く item を 1 回の draw にまとめる（pipeline の切り替えで順序は保たれる）。理由: glyph の多い頁の instance の data を増やさない（今の 96 byte/頂点 × 6 を保つ）。

ベクトルの pipeline の instance（頂点ごとに繰り返す、V3D の制約）: 12 個の vec4 ＋ corner の vec4 = 52 語（GHCI の attribute の 60 語の上限の内、[ws290 の調べ](../ws290/vk10-fixed-gaps.md)）。fragment の flat の入力は 12 vec4 ＋ pixel の位置 vec2 = 50 成分（Vulkan 1.0 の maxFragmentInputComponents の最小 64 の内）。

| vec4 | 中身 |
| --- | --- |
| 0 rect | 描く範囲（pixel、左上右下） |
| 1 color | 単色（straight）、gradient の時は alpha の係数 |
| 2 kind | 種類（rrect の塗り・ring・影・coverage の tile・全被覆の run・角丸の clip の下の矩形・glyph・画像）、辺、inset、tile の channel |
| 3 radii_a | 角丸の左上・右上の半径 |
| 4 radii_b | 右下・左下の半径 |
| 5 extra | ring の幅（上右下左）、影なら sigma と影の箱の矩形の一部 |
| 6 box | 影を切る箱・ring の内側の矩形 |
| 7 clip | 矩形の clip（pixel） |
| 8 clip_radii_a | 角丸の clip の半径 |
| 9 clip_radii_b | 同 |
| 10 paint_a | gradient の行列（to_gradient の a b c d） |
| 11 paint_b | gradient の行列の e f、ramp の行、ramp の幅、spread・種類 ＋ 幾何（線形の start・end、放射の焦点と中心と半径、円錐の中心と角度）は item の種類で詰め方を決める |

詳しい詰め方は p020 が決め、vulkan.c の comment に書く（module の header ではない）。半径の楕円・影の箱など 12 個に入らない組み合わせ（例: 角丸の clip の下の影）は、影の item を clip の矩形だけで描く（既知の差として dump と試験に書く）。

### 8.2 texture

- binding 0: 今の atlas（glyph・画像、2048²、BGRA8）。
- binding 1: coverage の atlas。2048² の BGRA8 を 16×16 の slot 128×128 個に分け、各 slot の 4 channel に別々の tile を置く（65536 tile）。tile は `vec_coverage` の partial tile。cache は path の key と tile の位置で引き、frame ごとの使用の印で LRU に捨てる。1 frame で足りない時は、足りない path を描かず数を数える（frame は欠けるが止まらない。cache の大きさを試験で測り、要れば 4096² に増やす）。
- binding 2: ramp の texture。2048 × 256 の BGRA8、1 行が 1 つの ramp。gradient の key で引き、LRU。
- 3 つとも今の atlas と同じく linear・host が書く・nearest の sampler・texel の中心で `texture()`（texelFetch を使わない）。frame の間に host が書き、fence の後に使う（今の約束のまま）。

### 8.3 shader の式

fragment shader は §4 の表の式を vector/ の関数と同じ順・同じ定数で書く（`vec_rrect_coverage`・`vec_rrect_ring_coverage`・`vec_rrect_side_share`・`vec_rrect_shadow`（erf は A&S 7.1.26、角の帯は 8 点の中点則）・`vec_gradient_parameter`・`vec_gradient_spread`）。CPU の参照の描画（software.c）は vector/ の関数そのものを呼ぶ。許容は WS074 の gpu-compare.py の基準（channel の差 ≤ 2、超える pixel が 0.1% まで）。

### 8.4 driver の制約

新しい shader も、今の display.vert・display.frag と同じ制約で書く: gl_VertexIndex を使わない、instance の頂点入力を使わない、texelFetch を使わない、fragment で gl_FragCoord を使わない。加えて: loop は回数が定数の物だけ（影の 8 回、ring の 4 辺）、`atan(y, x)`（円錐）・`exp`（影の Gauss の重み）・`sqrt` を使う。p020 は host の lavapipe と QEMU の Venus で確かめ、i915・V3D の実機は Q1 の割り当ての時に試験の一覧に載せる（未実施を明記）。円錐の gradient で `atan` が使えない driver があれば、その driver だけ円錐を ramp でなく CPU の bitmap にする道を p020 が用意する（第二の道は driver の能力で選ぶ）。

## 9. CSS の装飾（paint/list.c、p019、Opus）

| CSS | 今 | 新しい形 |
| --- | --- | --- |
| 角丸の背景（border-radius） | 行ごとの矩形 | PAINT_RRECT（塗り）。半径は `vec_rrect_normalize`（§4.5）。background-clip が padding・content の時は `vec_rrect_inset` |
| border（solid、全辺同じ色） | 行ごとの矩形 | PAINT_RRECT の ring 1 つ |
| border（辺ごとに色が違う） | 行ごとに辺の色 | ring 4 つ、各々 `side` の分け前（§4.4） |
| border-style dashed・dotted | solid で描く | 辺の中心線を path にして `vec_stroke`（dashed は幅 × 3 の dash と幅の gap、dotted は長さ 0 の dash と round cap、角の丸みに沿って）→ PAINT_PATH。長さの選び方は CSS Backgrounds 3 §3.2 が UA に任せる所で、p019 が決め dump に出す |
| double・groove・ridge・inset・outset | solid | double は ring 2 つ（幅の 1/3 ずつ）、groove・ridge は ring 2 つで明暗を逆に、inset・outset は辺の色を明暗に（色の明暗の式は p019 が決める） |
| box-shadow（外） | 10 枚の重ね、border box の下にも描く | PAINT_SHADOW（outer）。shape は `vec_rrect_spread` で、border box の外だけに描く（§6.1.1 の knockout） |
| box-shadow（inset） | 描かない | PAINT_SHADOW（inset）。padding box の中だけ、影の shape は padding box を spread で縮めた物の外側 |
| gradient の背景 | CPU の bitmap を IMAGE で | PAINT_RRECT（角丸が無ければ半径 0）か矩形を paint の gradient で。tile の繰り返し（background-repeat・size）は、tile ごとに item を作る（今の tile の上限 `LIST_TILES_MAX` を保つ） |
| radial・conic・repeating-radial・repeating-conic・hint | 解析しない | css/ の解析を足す（p017）。`struct css_gradient` に種類・形・大きさ・位置・始まりの角・hint を足す（§9.1） |
| overflow の clip ＋ border-radius | 矩形の clip | 角丸の clip（§7）。padding box の角丸（inner radius） |
| 置換要素（img など）の角丸 | 角丸は背景だけ | 画像の item に角丸の clip（content box の角丸、§4.3 の corner clipping） |

`paint/gradient.c` と `list_round_*`・`list_shadow` の近似の code は p019 で消す（削除は Q1 の手順: 担当は path と理由を Q1 へ。git の file の削除は `git rm` の一覧を Q1 に送り OK の後）。

### 9.1 css.h の案（p017 が当てる）

```c
enum css_gradient_kind { CSS_GRADIENT_LINEAR, CSS_GRADIENT_RADIAL, CSS_GRADIENT_CONIC };
struct css_gradient_stop { uint32_t color; float position; int unit; float position2; int unit2; int hint; };
/* (hint 1: this entry is a transition hint between the stops around it; position2 for the two-position stop syntax.) */
struct css_gradient {
	int kind;
	float angle;                     /* linear: as now; conic: the from angle in degrees */
	unsigned corner;                 /* linear: as now */
	unsigned repeating;
	int circle;                      /* radial: circle (1) or ellipse (0) */
	int size_keyword;                /* radial: closest-side, closest-corner, farthest-side, farthest-corner, or 0 for explicit */
	struct css_length radius[2];     /* radial: explicit size */
	struct css_length at[2];         /* radial and conic: the position (center by default) */
	unsigned count;
	struct css_gradient_stop stops[CSS_GRADIENT_STOPS];
};
```

conic の stop の位置は角度（deg・turn・rad・grad）か %。p017 は CSS Images 3 §3.2・CSS Images 4 §3.3・§3.4・§3.5.1 の文法を解析し、computed value の直列化（getComputedStyle）も直す。

## 10. SVG の統合（p021、Opus）

- **木の adapter**: `struct svg_tree` を DOM の上に作る（`page/` の新しい file、例 `page/svg-tree.c`）。名前は SVG の名前空間の element の local name、属性は UTF-16 の vm_string を UTF-8 にして build の間だけ arena に置く、`find_id` は document の id の表、`parse_color` は css の tokenizer と `css_parse_color`（currentcolor の印 0x00000001 は来ない約束）。
- **inline の `<svg>`**: 今は `images_svg` が bitmap を作り IMAGE の item。新しくは、layout が今と同じ大きさを決め、list.c が `svg_picture_build`（その大きさで）→ `svg_picture_draw`（置き場所の行列、tolerance 0.1 px）→ sink が PAINT_PATH の item を arena に作る。picture は DOM が変わるまで page が保持する（変わったら作り直す: 今の `images_svg` の「要素の番地で引く表」を picture の表にする）。
- **`<img src=*.svg>`**: decode の時に markup を保持する（今もしている）。表示の大きさで picture を作り、inline と同じく PAINT_PATH を出す。画像の cache は picture を持つ。
- **CSS の背景・list-style などの画像としての SVG**: tile が多いので bitmap の道（`svg_picture_render`、一度だけ、大きさが変わった時に作り直す）。今の `page_svg_decode` の口を保つ。
- **group の opacity**: list の sink は group の opacity を中の item の alpha に掛ける（近似、§7）。bitmap の道は本物の layer。
- `page/svg.c` の rasterizer（svg_fill・svg_stroke など）は消し、`page_svg_render`・`page_svg_decode` は svg/ の上の薄い口にする。`SVG_SIDE_MAX` 512 の制限は vector の道には無い（bitmap の道は `SVG_BITMAP_SIDE_MAX`）。

## 11. 実装の Phase

基点（header の凍結）: この文書と同じ commit（design-reviewer の反映の後の commit。SHA は ws.md の Resume point と各 phase.md に Q1 が書く）。

| Phase | 内容 | 担当 | 単体試験 | 依存 |
| --- | --- | --- | --- | --- |
| ws328-p002 | vec-base.c・vec-path.c | Sonnet | test-base・test-path | — |
| ws328-p003 | vec-flatten.c | Sonnet | test-flatten | p002 |
| ws328-p004 | vec-stroke.c | Sonnet | test-stroke | p003 |
| ws328-p005 | vec-raster.c | Sonnet | test-raster（参照の rasterizer と照合） | p003 |
| ws328-p006 | vec-gradient.c | Sonnet | test-gradient | p002 |
| ws328-p007 | vec-rrect.c | Sonnet | test-rrect | p002 |
| ws328-p008 | svg-values.c | Sonnet | test-values | p002 |
| ws328-p009 | svg-pathdata.c | Sonnet | test-pathdata | p008 |
| ws328-p010 | svg-style.c | Sonnet | test-style | p008 |
| ws328-p011 | svg-shape.c | Sonnet | test-shape | p009・p010 |
| ws328-p012 | svg-pserver.c | Sonnet | test-pserver | p006・p010 |
| ws328-p013 | svg-picture.c | Sonnet | test-picture | p011・p012 |
| ws328-p014 | svg-clip.c | Sonnet | test-clip | p013 |
| ws328-p015 | svg-draw.c | Sonnet | test-draw | p004・p005・p014 |
| ws328-p016 | svg-bitmap.c | Sonnet | test-bitmap | p006・p015 |
| ws328-p017 | CSS の gradient の解析（radial・conic・repeating・hint、css.h の §9.1） | Sonnet | WS074 の host の試験に足す（host-css-gradient.c） | — |
| ws328-p018 | paint の統合（paint.h の §7、software.c が新しい item を描く、dump、scene、sources.mk の承認済みの差分） | Opus | WS074 の host の試験（paint の dump と CPU の画素） | p005・p006・p007 |
| ws328-p019 | CSS の装飾を新しい item で（list.c、§9） | Opus | host の試験・WPT の該当の reftest（CPU） | p004・p017・p018 |
| ws328-p020 | Vulkan のベクトルの pipeline（§8） | Opus | gpu-compare（lavapipe）、QEMU の Venus | p018・p019 |
| ws328-p021 | SVG の統合（§10）、page/svg.c の置き換え | Opus | host の試験・WPT の SVG の reftest | p016・p018 |
| ws328-p022 | 統合の試験: WPT の該当の試験の集計、gpu-compare の頁の集合、QEMU の画面、実機の試験の一覧 | Opus | — | p019・p020・p021 |
| ws328-p023 | 規約の全文の見直し（header の持ち越しの直しを含む） | Sonnet（conformance-reviewer） | 全部 | p022 |

並べられる組: p002 の後に p003・p006・p007・p008 と p017。p003 の後に p004・p005。p008 の後に p009・p010。

各 phase.md は `plan/ws328/phaseNNN/phase.md`（spec-module-delegation の §4.6 の 7 節）。

## 12. WS074 との関係（再開の時に合わせる）

- WS328 は WS074 の file を変える: `paint/paint.h`・`list.c`・`software.c`・`vulkan.c`・`shaders/`・`gradient.c`（消す）、`page/svg.c`・`page/images.c`、`css/css.h`・`css/values.c` ほか、`sources.mk`。
- WS074 の再開の時の前提: display list の装飾は §7 の item、CSS の gradient は §9.1 の形、SVG は svg/ の module。WS074 の design.md §8.1・§8.2 の「SDF の角丸を shader で」「offscreen の layer で opacity・filter」のうち、前者は WS328 で実現し、後者（GPU の layer）は WS074 の側に残る（§14）。
- WS074 の host の試験（`plan/ws074/tests/`）の期待の値（paint の dump、画素）で WS328 の変更が変える物は、WS328 の統合の Phase が直し、各 phase.md の結果に file を全部書く（WS345 と同じ扱い。Q1 の許可を p018 の前に得る: user-questions でなく Q1 への報告）。

## 13. make の変更の案（承認が要る）

新しい .c を libbrowser の build に入れる差分（`userland/desktop/libbrowser/sources.mk`、paint の行の前）。p018 の始めに当てる。承認まで module は試験の script だけが compile する。

```make
	$(LIBBROWSER_DIR)/vector/vec-base.c \
	$(LIBBROWSER_DIR)/vector/vec-path.c \
	$(LIBBROWSER_DIR)/vector/vec-flatten.c \
	$(LIBBROWSER_DIR)/vector/vec-stroke.c \
	$(LIBBROWSER_DIR)/vector/vec-raster.c \
	$(LIBBROWSER_DIR)/vector/vec-gradient.c \
	$(LIBBROWSER_DIR)/vector/vec-rrect.c \
	$(LIBBROWSER_DIR)/svg/svg-values.c \
	$(LIBBROWSER_DIR)/svg/svg-pathdata.c \
	$(LIBBROWSER_DIR)/svg/svg-style.c \
	$(LIBBROWSER_DIR)/svg/svg-shape.c \
	$(LIBBROWSER_DIR)/svg/svg-pserver.c \
	$(LIBBROWSER_DIR)/svg/svg-picture.c \
	$(LIBBROWSER_DIR)/svg/svg-clip.c \
	$(LIBBROWSER_DIR)/svg/svg-draw.c \
	$(LIBBROWSER_DIR)/svg/svg-bitmap.c \
```

p019 で `$(LIBBROWSER_DIR)/paint/gradient.c` の行を消す差分、p021 で `page/svg-tree.c` を足す差分（page/svg.c は残して薄くする）も同じく案として出す。browser-win32 の Makefile は sources.mk を読むので、そちらは変えずに済む見込み（p018 が確かめる）。WS074 の `plan/ws074/tests/host-build.sh` は libbrowser の Makefile の一覧を読むので変えずに済む見込み。

## 14. 範囲の外と既知の差（後で実装する物の一覧）

| 項目 | 今の扱い | 後で |
| --- | --- | --- |
| GPU の layer（opacity・filter・mask の group） | opacity は item の alpha に掛ける近似、filter は CPU だけ | WS074 か新しい WS |
| CSS の clip-path の形（circle・ellipse・polygon・path）で HTML の中身を切る | inset() と rect() だけ（今のまま） | GPU の layer の後 |
| HTML の箱の rotate・skew・matrix の transform | translate・scale だけ（今のまま） | WS074 |
| 入れ子の角丸の clip（二重） | 内側の 1 つだけ角丸、外側は矩形 | 要れば coverage の mask |
| SVG の text・image・marker・mask・pattern・filter・foreignObject | 描かない | 個別の Phase か WS |
| SVG への style sheet の cascade（`<style>`・class） | 属性と style 属性だけ | CSS の engine の computed style を svg_style に写す |
| 入れ子の svg の viewport の clip、clipPath の子の clip-path | しない | svg-picture・svg-clip の拡張 |
| SVG の miter-clip・arcs の join | miter で描く | — |
| `<color-interpolation-method>`、CSS の色空間（oklab など） | sRGB の補間だけ | CSS Color 4 の実装の後 |
| Canvas 2D | 無い | vector/ を土台にできる |
| devicePixelRatio ≠ 1 | 1 のまま | DPI の倍率の導入の時（guardrail の論理 px） |

## 15. 試験の道具（p001 が作り、実装の担当は変えない）

| file | 役目 |
| --- | --- |
| [tests/run-unit.sh](tests/run-unit.sh) | `test-<名前>.c` を全 module（未実装は骨格）と ASan・UBSan・`-std=c11 -Wall -Wextra -Werror` で build して流す。最後の行 `run-unit: PASS`／`FAIL`。`CC=clang` も |
| [tests/check-skeleton.sh](tests/check-skeleton.sh) | header の単独の compile（cc・clang、二重の include）、全 .c の compile、engine の header を include しない事、開発の管理情報の不在、`BASE=<SHA>` で header の不変と追加の無さ、zedBSD の clang と sysroot での compile（`WS328_ZEDBSD_CC`・`WS328_ZEDBSD_SYSROOT`） |
| [tests/unit/check.h](tests/unit/check.h) | 判定の macro（整数・float の近さ・色の近さ）、決定的な乱数（xorshift32） |
| [tests/unit/raster-reference.h](tests/unit/raster-reference.h) | coverage の定義の独立の参照（各副走査線で 256 点の winding を数える力ずく）。vec-raster の試験の照合に使う |
| [tests/unit/svg-test-tree.h](tests/unit/svg-test-tree.h) | 試験用の小さな markup の reader と `struct svg_tree` の関数、試験用の色の parser |
| [tests/unit/test-harness.c](tests/unit/test-harness.c) | 道具の自己試験（手の計算の coverage、markup、乱数）。骨格のままで PASS |

統合の試験（p018〜p022 が作る）: WS074 の `gpu-compare.py`（CPU と GPU の画素の比べ、host の lavapipe）に頁の集合 `plan/ws328/tests/pages/*.html` を足す（角丸・border の各 style・影・各 gradient・SVG の図形・線・dash・gradient・clip）。WPT は WS074 の `fetch-suites.sh wpt` と `run-wpt-reftests.py` で、`css/css-backgrounds/`（border-radius・box-shadow）、`css/css-images/`（gradient）、`svg/shapes/`・`svg/painting/`・`svg/pservers/`・`svg/coordinate-systems/`・`svg/paths/`・`css/css-masking/clip-path/`（SVG の clipPath）の reftest を流して数える（p022 で前と後の数）。

## 16. 資料

仕様は git に入れない。p001 が取得した物は担当の worktree の `build/sources/specs/ws328/`（`SHA256SUMS` 付き、text にした物は `txt/`）。Q1 が main の checkout の `build/sources/specs/ws328/` に写す（担当は main の build/ に書かない）。実装の担当は自分の worktree に同じ URL から取り、`SHA256SUMS` で照合する（W3C の TR の版は日付で固定される。照合が合わない時は Q1 に報告）。

| file | URL | SHA-256（先頭 16 桁） |
| --- | --- | --- |
| svg2-paths.html | https://www.w3.org/TR/SVG2/paths.html | 0324612f634d9f80 |
| svg2-shapes.html | https://www.w3.org/TR/SVG2/shapes.html | 1cadcaa1a0a1c92c |
| svg2-painting.html | https://www.w3.org/TR/SVG2/painting.html | 49f3f71568cec71d |
| svg2-pservers.html | https://www.w3.org/TR/SVG2/pservers.html | 32cfc537ba710606 |
| svg2-coords.html | https://www.w3.org/TR/SVG2/coords.html | 2ec3740e09f4d616 |
| svg2-struct.html | https://www.w3.org/TR/SVG2/struct.html | 66fbee96b6bd0b04 |
| svg2-styling.html | https://www.w3.org/TR/SVG2/styling.html | 1a6bafeafff998a9 |
| svg2-implnote.html | https://www.w3.org/TR/SVG2/implnote.html | 29828aef79c32d07 |
| svg2-render.html | https://www.w3.org/TR/SVG2/render.html | 6e8c7a5557707f5f |
| svg2-types.html | https://www.w3.org/TR/SVG2/types.html | a15ac151a35aaf70 |
| css-backgrounds-3.html | https://www.w3.org/TR/css-backgrounds-3/ | 8a291792b2fd351e |
| css-images-3.html | https://www.w3.org/TR/css-images-3/ | 74eb4b475aa0845a |
| css-images-4.html | https://www.w3.org/TR/css-images-4/ | 2784dc56db108985 |
| css-masking-1.html | https://www.w3.org/TR/css-masking-1/ | 79ff953b47472ae5 |
| css-color-4.html | https://www.w3.org/TR/css-color-4/ | 11adfa51f61d8859 |
| css-transforms-1.html | https://www.w3.org/TR/css-transforms-1/ | 4672e86844bfbe7a |
| css-values-3.html | https://www.w3.org/TR/css-values-3/ | dcce1f2ff24d66a0 |
| compositing-1.html | https://www.w3.org/TR/compositing-1/ | 0fb91119308e706a |
| html-canvas.html | https://html.spec.whatwg.org/multipage/canvas.html | 042d6f60b712f6f9 |

HTML の仕様は living standard なので、照合が合わない時は §4.12.5.1.10 の createRadialGradient の手順が変わっていない事を見て進めてよい。数学の文献: 誤差関数は Abramowitz and Stegun, Handbook of Mathematical Functions, 式 7.1.26。Bézier の平坦化の分割数は二階差分の上界（vec-flatten.h に導出を書いた）。乱数は Marsaglia, "Xorshift RNGs"（試験の道具だけ）。

## 17. 技術の判断（担当が決めた物と理由）

| # | 判断 | 理由 |
| --- | --- | --- |
| D1 | 一般の path の coverage は CPU で一度作り、GPU は tile を引いて合成する（§4） | §4 の 1〜4。ユーザーの「Vulkan で描画」の解釈は U1 で確かめる |
| D2 | 角丸・ring・影・gradient は shader の閉じた式 | frame ごとに CPU が画素を触らない（guardrail）。atlas を食わない。CPU の参照は同じ関数 |
| D3 | coverage は副走査線 16 本 × x の厳密な区間（面積の累積でない） | 線の輪郭を部品の和で作るので、重なりと継ぎ目で数え過ぎない事が要る。累積（符号付き面積の和）は和の形で境の pixel を二重に数える。縦の量子化 1/16 は 8 bit の 16 段で目に見えない |
| D4 | 線は輪郭の offset 曲線でなく、線分の矩形・join・cap の多角形の和 | 単純で頑丈（自己交差・急な曲線でも正しい和）。D3 と組む |
| D5 | 曲線の内側の頂点の join は round | 平坦化の折れ目に miter の棘が出ない |
| D6 | gradient は ramp（1 行、最大 2048 texel）を引く。CSS は premultiplied、SVG は straight の補間 | 硬い stop の位置の誤差が半 pixel に収まる。補間は CSS Images 3 §3.4.2・CSS Color 4 §12.1 と SVG 2 §14.2.1 の式のとおり |
| D7 | 影は erf の積（真ん中の帯は厳密）と角の帯の 8 点の中点則 | §6.1.2 の 5% の許容の内。帯の境で積分の段差が出ない |
| D8 | 新しい pipeline を足し、今の display の pipeline を残す | glyph の多い頁の頂点の data を増やさない |
| D9 | SVG は DOM を関数の表越しに読む | module を host で試験でき、DOM の変更から切り離せる |
| D10 | SVG の色は CSS の色の parser を callback で使う | 名前付きの色の表を二重に持たない |
| D11 | CSS の dashed・dotted は stroker の dash で | 角の丸みに沿う。長さは UA の任意（§3.2）なので p019 が決める |
| D12 | inline・`<img>` の SVG はベクトルの item（大きさに依らない）、背景の SVG は bitmap | tile の多い背景で item が増え過ぎない |
| D13 | 新しい module は make の承認まで試験の script だけが compile | Makefile・mk の変更は承認が要る（AGENTS.md §7） |

## 18. design-reviewer の指摘と反映

（design-reviewer の後に書く。）
