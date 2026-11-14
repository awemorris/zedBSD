<!-- awesome-plan project=zedbsd record=ws328-design -->

# WS328 の設計: libbrowser のベクトル描画（path の塗りと線、CSS の角丸・gradient・影）と SVG の描画

- 書いた者: P4（ws328-p001、Queue q2091-i01）。日付は host の `date`（2026-11-14）。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。module（vector/・svg/）は Sonnet、統合（paint・list・Vulkan・page）は Opus。
- この文書は p001 の成果で、header（`userland/desktop/libbrowser/vector/vec-*.h`・`svg/svg-*.h`）と同じ commit で凍結する。header の comment が仕様で、この文書は分け方・理由・統合の設計を書く。
- 2026-11-14〜15 P4（q2098-i01）が design-reviewer の 2 回のレビュー（計 83 件）を反映した（§18）。

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
| 一般の path（SVG の図形・線の輪郭・dashed などの border・clip-path） | path の item 1 つを bbox の quad 1 枚で描き、fragment が tile の表（texture）から coverage の slot を引いて atlas を読み、paint（色か gradient）を掛ける（§8.1・§8.2） | 平坦化と線の展開は list を作る時（style の thread）。coverage の tile は描画の thread が cache に無い時だけ `vec_rasterize` で作る（scroll では作り直さない。下の「cache の粒度と予算」） |

**cache の粒度と予算**（描画の thread、CPU の参照と GPU で同じ cache の module を使う）:
- 1 つの path について、見える範囲（viewport に上下 1 画面の余白）の tile の矩形を tile の格子に合わせて、`vec_rasterize` を 1 回呼んで作る（clip は tile に揃えるので、隣の窓を後で作っても既にある tile は正しいまま）。cache の entry は path の key・scale・作った tile の矩形を持ち、scroll で窓の外に出た部分だけを足して作る。
- 大きな path（地図など）は窓だけを作るので `VEC_RASTER_SIDE_MAX`・`VEC_RASTER_TILES_MAX` に当たらない。当たった時（窓の中だけで超える物）は、その path を描かず数を数える（dump と統計に出す）。
- 1 frame で新しく作る coverage の予算: 線分の交差の数で `VEC_RASTER_CROSSINGS_MAX / 8`（Pi 4 で数 ms の見込み、p026 が測って決め直す）。超えた分の path は次の frame に回し、それまでは前の coverage（あれば。scale が違えば最も近い scale の物を伸ばして）か何も描かない。SVG の animation・CSS の scale の変化（毎 frame 幾何が変わる）はこの予算の中で CPU が作り直す: U1 の「frame ごとに CPU が画素を触らない」の例外で、上限がある事を U1 に書いた。
- key の衝突: 64 bit の hash で引き、当たった時は entry が持つ幾何の写し（頂点・rule・clip の部品）と比べてから使う（ramp も stop の列を比べる）。

coverage を GPU の shader で計算しない理由（技術の判断、ユーザーへの確かめは [user-questions.md](user-questions.md) の U1）:
1. zedBSD の自前の driver（i915 の native の compiler、V3D、PowerVR、GHCI）は shader の機能がまちまちで、今の display の shader もそれに合わせて制限している（texelFetch 無し、instance の頂点入力無し）。path の coverage を GPU で出すには、辺の data を storage buffer か浮動小数の texture で渡し、fragment で辺を回す loop か compute shader が要り、どの driver でも動く保証が無い。loop も buffer も要らない stencil-then-cover（三角形の扇を stencil に書いて cover の quad で塗る、AA は MSAA）もあるが、今の render pass に stencil の attachment が無く、各 driver の stencil と MSAA の対応は確かめていない（U1 の案 C）。
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

/* One clip of the list's clip table (an SVG clip): the union of its parts, met with its parent. */
struct paint_vector_clip {
	const struct vec_lines *parts;   /* part_count polylines in the arena, pixels from the item's (x, y) */
	const enum vec_fill_rule *rules; /* the rule of each part */
	size_t part_count;
	int parent;                      /* index in the clip table, -1 for none */
	uint64_t key;                    /* hash of the parts, rules and the parent's key */
};

/* The vector part of an item (in the list's arena; the item points at it). */
struct paint_vector {
	struct paint_fill fill;
	float scale_x;                   /* the item's CSS scale (1 for none): the geometry is drawn scaled */
	float scale_y;
	/* PAINT_PATH */
	struct vec_lines path;           /* vertices and contours in the arena, pixels from the item's (x, y) */
	enum vec_fill_rule rule;
	int clip;                        /* index in the list's clip table, -1 for none */
	uint64_t key;                    /* hash of the geometry and the rule (the clip's key joins it in the cache) */
	/* PAINT_RRECT */
	struct vec_rrect rrect;          /* in pixels from the item's (x, y), edges whole pixels */
	struct vec_rrect inner;          /* a ring's inner edge (ring set) */
	float widths[4];
	int ring;
	int side;                        /* -1 for all sides, else the side whose share is painted */
	/* PAINT_SHADOW */
	float sigma;
	float spread;
	struct vec_point offset;         /* the shadow's offset from the box */
	int inset;                       /* 0: outer, painted outside box; 1: inset, painted inside box */
	struct vec_rrect box;            /* the border box (outer) or padding box (inset): the shadow's shape is
	                                    vec_rrect_spread(box, spread (outer) or -spread (inset)) moved by offset,
	                                    and the box with its own radii cuts it (the knockout of §6.1.1) */
};

struct paint_item { ...; const struct paint_vector *vector; };
```

- 位置: item の (x, y) は今と同じく document の layout の単位。vector の幾何は item の (x, y) を原点とした pixel。`scene_resolve`（frame の移動）は (x, y) だけを動かせばよい。
- 倍率: item に `scale_x`・`scale_y`（既定 1）を持たせる。list の CSS の `transform: scale()`（今 `list.c` が x・y・width・height と glyph を倍にしている所）と view transition の写像（`list_vt_remap`）は、ベクトルの item では幾何を書き換えずにこの倍率を掛ける。renderer は rrect・影・gradient の式を倍率で写した座標で計算し（shader の中、CPU も同じ）、path の coverage は倍率を key に入れて作る（倍率は 1/64 に量子化）。毎 frame 倍率が変わる animation は §4 の予算の中で作り直す。
- 画素の位相: CPU と GPU は item の原点を scroll の後に整数の pixel に丸め（`floorf(px + 0.5)`、今の矩形と同じ）、その位相 0 で描く。だから coverage の tile は scroll で作り直さない（cache の key に位相が入らない）。角丸矩形と影も同じ丸めの原点から測る。
- 辺の丸め: 背景・border・角丸の clip の rrect は、矩形の 4 辺を PAINT_RECT と同じく `floorf(v + 0.5)` で整数の pixel に丸めてから item に入れる（半径は端数のまま）。だから角丸 0 の箱は今の PAINT_RECT と画素が一致し、隣の矩形との間に継ぎ目が出ない（`vec_rrect_coverage` の comment）。影の矩形は丸めない（ぼかすので）。
- clip: PAINT_CLIP の item に角丸（`rounded`・`radius`、今は BACKDROP が使う field）を持たせ、`paint_clips` の top に「矩形 ∩ 一番内側の角丸矩形 1 つ」を持つ。外側にもう一つ角丸の clip がある時は、その矩形だけを使う（既知の差。§14）。vector の item・矩形・glyph・画像は角丸の clip を shader の式（`vec_rrect_coverage`）で掛ける。
- SVG の clip は「部品の和（部品ごとに rule）と親との積の鎖」（`struct svg_draw_clip`）なので、item は 1 本の折れ線でなく clip の表の番号を持つ。list の arena に clip の表（`struct paint_vector_clip`: 部品の配列 {折れ線, rule}、親の番号、key）を置き、path の item の `clip` はその番号（-1 で無し）。coverage を作る時、部品ごとの coverage を `vec_coverage_unite` で和にし、親の鎖を `vec_coverage_intersect` で掛ける（clip の coverage も cache に置き、同じ clip を共有する item で使い回す）。coverage の key は path の key と clip の鎖の key の組。
- 深い写し: view transition の snapshot（`paint_transition_capture` が item を写して list の arena を捨てる所、snapshot を list に戻す所）は、`item->vector`（path の頂点・contour、stop、clip の表と部品の折れ線）を snapshot の memory に深く写す（今は glyph だけを写している。gradient の IMAGE の item が arena の bitmap を指したままなのも同じ所で直す）。p018 の範囲。
- 透明度の group: SVG の opacity と CSS の opacity は、今と同じく list が item の alpha に掛ける（重なりでは近似。fill と stroke を持つ図形では線の内側の半分が濃く見える: U3）。`svg_picture_render`（CPU の bitmap）だけは本物の layer で描く。
- `paint_dump`（試験の text の dump）: 新しい種類ごとに 1 行（種類・原点・bbox・倍率・paint の色か gradient の種類と stop の数・key・clip の番号）。
- CPU の参照の描画（software.c、`--no-gpu` と headless）も描画の thread の coverage の cache（§4 と同じ module）を使い、scroll ごとに path を raster し直さない。rrect・影は pixel ごとの式のまま（GPU と同じ）。
- `paint/paint.h` の file の comment にある開発の管理情報（設計の文書への参照）は、p018 が paint.h を変える時に消す（guardrail）。

## 8. Vulkan の描画（p020・p026・p027、Opus）

### 8.1 pipeline

今の display の pipeline（矩形・glyph・画像、instance 5 個の vec4）はそのまま残し、ベクトル用の pipeline を 1 本足す。同じ render pass の中で list の順に描き、同じ pipeline の続く item を 1 回の draw にまとめる（pipeline の切り替えで順序は保たれる）。理由: glyph の多い頁の instance の data を増やさない（今の 96 byte/頂点 × 6 を保つ）。文字とベクトルの item が交互に並ぶ頁では切り替えが増えるので、p020 が切り替えの数と時間を測り、dump の統計に出す（多ければ、角丸の clip の下の文字・画像もベクトルの pipeline で描く形に寄せる: p027）。

ベクトルの pipeline の instance（頂点ごとに繰り返す、V3D の制約）: 12 個の vec4 ＋ corner の vec4 = 52 語（GHCI の attribute の 60 語の上限の内、[ws290 の調べ](../ws290/vk10-fixed-gaps.md)。他の driver の上限は p020 が確かめる）。fragment の flat の入力は 12 vec4 ＋ pixel の位置 vec2 = 50 成分（Vulkan 1.0 の maxFragmentInputComponents の最小 64 の内）。1 item は 1 instance（6 頂点）で、path も tile ごとでなく item の bbox の quad 1 枚にする（§8.2 の tile の表）。

| vec4 | 中身 |
| --- | --- |
| 0 rect | 描く範囲（pixel、左上右下） |
| 1 color | 単色（straight）、gradient の時は alpha の係数 |
| 2 kind | 種類（rrect の塗り・ring・影・path・角丸の clip の下の矩形・glyph・画像）、辺、inset、倍率 |
| 3 radii_a | 角丸の左上・右上の半径（影では箱の半径） |
| 4 radii_b | 右下・左下の半径 |
| 5 extra | ring の幅（上右下左）。影は sigma・spread・offset の x と y。path は tile の表の左上（表の texture の x・y）と横の tile の数・tile の原点 |
| 6 box | ring の内側の矩形。影は箱（border box か padding box）の矩形 |
| 7 clip | 矩形の clip（pixel） |
| 8 clip_radii_a | 角丸の clip の半径 |
| 9 clip_radii_b | 同 |
| 10 paint_a | gradient の行列（to_gradient の a b c d） |
| 11 paint_b | gradient の行列の e f、ramp の行、ramp の幅、spread・種類、CSS の tile の繰り返しの周期 ＋ 幾何（線形の start・end、放射の焦点と中心と半径、円錐の中心と角度）は item の種類で詰め方を決める |

影は箱の rrect（矩形と半径）・spread・offset を送り、shader が `vec_rrect_spread` の式で影の形を作り、箱そのもの（半径付き）で切る（外の影は箱の外だけ、inset は箱の中だけ）。だから箱の角丸は失われない。詳しい詰め方は p020 が決め、vulkan.c の comment に書く（module の header ではない）。CSS の背景の gradient の繰り返し（background-repeat）は、tile の周期を paint_b に入れ shader が `fract` で 1 周期に戻すので、tile ごとに item を作らない（1 item）。

### 8.2 texture

- binding 0: 今の atlas（glyph・画像、2048²、BGRA8）。
- binding 1: coverage の atlas。2048² の BGRA8 を 16×16 の slot 128×128 個に分け、各 slot の 4 channel に別々の tile を置く（65536 tile）。tile は `vec_coverage` の partial tile。cache は path の key と tile の位置で引き、frame ごとの使用の印で LRU に捨てる。1 frame で足りない時は、足りない path を描かず数を数える（frame は欠けるが止まらない。cache の大きさを試験で測り、要れば 4096² に増やす）。
- binding 2: ramp の texture。2048 × 256 の BGRA8、1 行が 1 つの ramp。gradient の key（衝突は stop の列で確かめる）で引き、LRU。1 frame に 256 を超える ramp は、超えた item を平均の色（`vec_gradient_average`）で描いて数える。
- binding 3: tile の表。2048² の BGRA8、path の item ごとに bbox の tile の数（横 × 縦）の texel を確保し、各 texel に「全被覆・空・atlas の slot と channel」を符号にして置く。fragment は自分の pixel の tile の texel を引き（`texture()`、texel の中心）、slot を読んで coverage の atlas を引く。頂点の data は item ごとに 1 instance で、tile の数に比例しない（全画面の SVG でも頂点は item の数だけ）。
- 4 つとも今の atlas と同じく linear・host が書く・nearest の sampler・texel の中心で `texture()`（texelFetch を使わない）。frame の間に host が書き、fence の後に使う（今の約束のまま）。

### 8.3 shader の式

fragment shader は §4 の表の式を vector/ の関数と同じ順・同じ定数で書く（`vec_rrect_coverage`・`vec_rrect_ring_coverage`・`vec_rrect_side_share`・`vec_rrect_shadow`（erf は A&S 7.1.26、角の曲線は 45° で二つに分け各 8 点の中点則、急な半分は y で、緩い半分は部分積分で x で）・`vec_gradient_parameter`（円錐の角は `vec_atan2` の多項式）・`vec_gradient_spread`）。指数は全て `vec_exp` の形（`exp2(x * 1.44269504)`）で書き、GLSL の `exp`・`atan` は使わない。CPU の参照の描画（software.c）は vector/ の関数そのものを呼ぶ。許容は WS074 の gpu-compare.py の基準（channel の差 ≤ 2、超える pixel が 0.1% まで）。

### 8.4 driver の制約

新しい shader も、今の display.vert・display.frag と同じ制約で書く: gl_VertexIndex を使わない、instance の頂点入力を使わない、texelFetch を使わない、fragment で gl_FragCoord を使わない。加えて、zedBSD の自前の shader compiler が受ける命令だけを使う: 共通の compiler（`src/drivers/gpu/compiler/spirv.c`、i915・V3D・RDNA・GHCI・Mali・NVIDIA が使う）は GLSL.std.450 の列挙した物以外（Atan・Atan2 など）を断り、PowerVR の compiler（`src/drivers/gpu/powervr/compiler/`）は Exp も持たず Exp2 だけ。1 つでも断られる命令があると pipeline 全体が作れないので、`atan` は使わず `vec_atan2` の多項式、`exp` は使わず `exp2`、`sqrt`・`floor`・`fract`・`min`・`max`・`clamp`・`mix` だけにする。loop は回数が定数の物だけ（影の角 4 つ × 2 半分 × 8 点、ring の 4 辺）。影は角ごとに「その半分の行が cy ± 4σ の外なら飛ばす」早い抜け（header の式）で、大きな影でも殆どの pixel は直線部分の閉じた式だけになる。

p020 の確かめ: host の lavapipe と QEMU の Venus での画素の比べ、加えて host で build できる zedBSD の shader compiler（共通の compiler と PowerVR の compiler）に新しい SPIR-V を通して断られない事（host で build できない物は実機の一覧へ）。i915・V3D の実機、Pi 4（V3D 4.2）での全画面の影 1 枚と全画面の SVG の frame の時間の計測は、Q1 の割り当ての時に試験の一覧に載せる（未実施を明記）。

## 9. CSS の装飾（paint/list.c、p019・p024・p025、Opus）

| CSS | 今 | 新しい形 |
| --- | --- | --- |
| 角丸の背景（border-radius） | 行ごとの矩形 | PAINT_RRECT（塗り）。半径は `vec_rrect_normalize`（§4.5）。background-clip が padding・content の時は `vec_rrect_inset` |
| border（solid、全辺同じ色） | 行ごとの矩形 | PAINT_RRECT の ring 1 つ |
| border（辺ごとに色が違う） | 行ごとに辺の色 | ring 4 つ、各々 `side` の分け前（§4.4） |
| border-style dashed・dotted | solid で描く | 辺の中心線を path にして `vec_stroke`（dashed は幅 × 3 の dash と幅の gap、dotted は長さ 0 の dash と round cap、角の丸みに沿って）→ PAINT_PATH。長さの選び方は CSS Backgrounds 3 §3.2 が UA に任せる所で、p019 が決め dump に出す |
| double・groove・ridge・inset・outset | solid | double は ring 2 つ（幅の 1/3 ずつ）、groove・ridge は ring 2 つで明暗を逆に、inset・outset は辺の色を明暗に（色の明暗の式は p019 が決める） |
| box-shadow（外） | 10 枚の重ね、border box の下にも描く | PAINT_SHADOW（outer、p024）。箱は border box（半径付き）、shape は `vec_rrect_spread(box, spread)` を offset だけ動かした物で、border box の外だけに描く（§6.1.1 の knockout。箱の角丸で切る） |
| box-shadow（inset） | 描かない | PAINT_SHADOW（inset、p024）。padding box の中だけ、影の shape は padding box を spread で縮めた物を offset だけ動かした物の外側 |
| gradient の背景 | CPU の bitmap を IMAGE で | PAINT_RRECT（角丸が無ければ半径 0）か矩形を paint の gradient で（p025）。tile の繰り返し（background-repeat・size）は tile の周期を paint に入れ、shader と CPU が周期で戻す（item は 1 つ） |
| gradient の stop の位置 | — | 繰り返さない gradient で 0% より前・100% より後の stop は `vec_gradient_extend_span` で線を伸ばす（CSS Images 3 §3.1・§3.2）。repeating は `vec_gradient_repeat_span`、退化（first = last）は `vec_gradient_average`（等間隔に置き直す）。hint は stop と同じ並びの entry として `vec_gradient_fixup_css` に渡す |
| 退化した radial | — | CSS Images 3 §3.2.3: 半径 0 の円は「とても小さな半径」の円として（list が半径を 1/1024 px にする）、幅か高さ 0 の楕円は §3.2.3 の手順どおり（幅 0 は高さを保ち幅を極小に、など）。p025 が §3.2.3 の文を読み、場合ごとに試験を足す |
| radial・conic・repeating-radial・repeating-conic・hint | 解析しない | css/ の解析を足す（p017）。`struct css_gradient` に種類・形・大きさ・位置・始まりの角・hint を足す（§9.1） |
| overflow の clip ＋ border-radius | 矩形の clip | 角丸の clip（§7）。padding box の角丸（inner radius） |
| 置換要素（img など）の角丸 | 角丸は背景だけ | 画像の item に角丸の clip（content box の角丸、§4.3 の corner clipping） |

`list_round_*` の近似は p019、`list_shadow` は p024、`paint/gradient.c` は p025 で消す（削除は Q1 の手順: 担当は path と理由を Q1 へ。git の file の削除は `git rm` の一覧を Q1 に送り OK の後）。

p019・p024・p025 は GPU の描画（p020・p026・p027）が main に入った後に始める（§11）。新しい item を GPU が描けない間に CSS をそれへ切り替えると、main の GPU の描画で装飾が消えるため。

### 9.1 css.h の案（p017 が当てる）

```c
enum css_gradient_kind { CSS_GRADIENT_LINEAR, CSS_GRADIENT_RADIAL, CSS_GRADIENT_CONIC };
struct css_gradient_stop { uint32_t color; float position; int unit; float position2; int unit2; int hint; };
/* (hint 1: this entry is a transition hint between the stops around it; position2 for the two-position stop syntax.
 * CSS_GRADIENT_STOPS counts stops and hints together and goes from 16 to 32.) */
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

## 10. SVG の統合（p021・p028、Opus）

- **木の adapter**（p021）: `struct svg_tree` を DOM の上に作る（`page/` の新しい file、例 `page/svg-tree.c`）。名前は SVG の名前空間の element の local name、`parent` は DOM の親（SVG の名前空間の外の親でも返し、`name` が NULL を返すので svg/ の側で根が分かる）、属性は UTF-16 の vm_string を UTF-8 にして build の間だけ arena に置く、`find_id` は document の id の表、`parse_color` は css の tokenizer と `css_parse_color`（currentcolor の印 0x00000001 は来ない約束）。
- **継承の口**: inline の `<svg>` は HTML の祖先の computed style から `struct svg_style` の外側（`outer`: color、fill・stroke などの継承される性質、font-size）を作って `svg_picture_build` に渡す。`fill="currentColor"` のアイコンが CSS の color で描かれる。`<img>`・背景の SVG は外側無し（NULL、文書として独立）。
- **inline の `<svg>`**（p021）: 今は `images_svg` が bitmap を作り IMAGE の item。新しくは、layout が今と同じ大きさを決め、list.c が `svg_picture_build`（その大きさで）→ `svg_picture_draw`（置き場所の行列、tolerance 0.1 px）→ sink が PAINT_PATH の item を arena に作る。picture の item の列は svg の content box の PAINT_CLIP で囲む（`overflow` が visible の時を除く。UA の `svg:not(:root) { overflow: hidden }` が既定なので普通は切る。slice の preserveAspectRatio・viewBox の外の図形が箱の外に出ない）。
- **picture の持ち主と作り直し**: picture の表（今の `images_svg` の「要素の番地で引く表」を置き換える）は style の thread の物で、list を作る間だけ読む（描画の thread は scene の arena の写しだけを見る）。作り直す条件: svg の subtree の DOM の変化、外側の style の変化、大きさ（% の長さ・viewport）の変化、picture が参照した subtree の外の要素（`find_id` で引いた use・gradient・clipPath の先）の変化。最後の物は build が引いた id の一覧を picture の表に持ち、DOM の変化の id と照らす。
- **`<img src=*.svg>`**（p028）: decode の時に markup を保持する（今もしている）。表示の大きさで picture を作り、inline と同じく PAINT_PATH を出す。画像の cache は picture を持つ（cache の持ち主の thread は今の画像の cache と同じ）。
- **CSS の背景・list-style などの画像としての SVG**（p028）: tile が多いので bitmap の道（`svg_picture_render`、一度だけ、大きさが変わった時に作り直す）。今の `page_svg_decode` の口を保つ。
- **group の opacity**: list の sink は group の opacity を中の item の alpha に掛ける（近似、§7）。bitmap の道は本物の layer。
- `page/svg.c` の rasterizer（svg_fill・svg_stroke など）は p028 で消し、`page_svg_render`・`page_svg_decode` は svg/ の上の薄い口にする。`SVG_SIDE_MAX` 512 の制限は vector の道には無い（bitmap の道は `SVG_BITMAP_SIDE_MAX`）。
- p021・p028 も GPU の path の描画（p026）が main に入った後に始める。

## 11. 実装の Phase

基点（header の凍結）: design-reviewer の反映の commit（SHA は ws.md の Resume point と各 phase.md に Q1 が書く）。

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
| ws328-p013 | svg-picture.c の核（root の svg・g・図形・style・paint・opacity・容器の bbox・上限・pathLength） | Sonnet | test-picture | p011・p012 |
| ws328-p029 | svg-picture.c の use・symbol・入れ子の svg・switch | Sonnet | test-picture-use | p013 |
| ws328-p014 | svg-clip.c | Sonnet | test-clip | p013 |
| ws328-p015 | svg-draw.c | Sonnet | test-draw | p004・p005・p014 |
| ws328-p016 | svg-bitmap.c | Sonnet | test-bitmap | p006・p015 |
| ws328-p017 | CSS の gradient の解析（radial・conic・repeating・hint、css.h の §9.1） | Sonnet | `plan/ws328/tests/` に置く host の試験（host-css-gradient） | p001 |
| ws328-p018 | paint の統合（paint.h の §7、software.c が新しい item を描く、coverage の cache、dump、scene、snapshot の深い写し、倍率）。始めに WPT の基準の数を取る | Opus | host の試験（paint の dump と CPU の画素、合成の item） | p005・p006・p007 |
| ws328-p020 | Vulkan のベクトルの pipeline: rrect・ring・影・gradient の shader、ramp の texture | Opus | gpu-compare（lavapipe、合成の item の頁）、shader compiler に通す | p018 |
| ws328-p026 | Vulkan の path: coverage の atlas・tile の表・cache と予算 | Opus | gpu-compare（合成の path の item） | p020 |
| ws328-p027 | 角丸の clip の下の矩形・glyph・画像、pipeline の切り替えの計測、QEMU の Venus | Opus | gpu-compare、QEMU | p026 |
| ws328-p019 | CSS の角丸の背景・border（各 style）・角丸の clip・置換要素の角丸（list.c） | Opus | host の試験・WPT の該当の reftest（CPU） | p004・p027 |
| ws328-p024 | CSS の box-shadow（外・inset） | Opus | host の試験・WPT | p019 |
| ws328-p025 | CSS の gradient の背景（linear・radial・conic・repeating、tile、退化）、`paint/gradient.c` を消す | Opus | host の試験・WPT | p017・p019 |
| ws328-p021 | SVG の統合: 木の adapter・継承の口・inline の `<svg>`・viewport の clip・picture の表 | Opus | host の試験・WPT の SVG の reftest | p016・p026・p029 |
| ws328-p028 | `<img>` の SVG・背景の SVG、page/svg.c の rasterizer を消す | Opus | host の試験・WPT | p021 |
| ws328-p022 | 統合の試験: WPT の該当の試験の集計（p018 の基準との比べ）、gpu-compare の頁の集合、QEMU の画面、実機の試験の一覧 | Opus | — | p024・p025・p028 |
| ws328-p023 | 規約の全文の見直し（書き方だけ） | Sonnet（conformance-reviewer） | 全部 | p022 |

並べられる組: p002 の後に p003・p006・p007・p008。p017 はいつでも。p003 の後に p004・p005。p008 の後に p009・p010。p013 の後に p029・p014。GPU の側（p020 → p026 → p027）を先に main に入れ、それから CSS（p019 → p024・p025）と SVG（p021 → p028）を切り替える（新しい item を GPU が描けない期間を main に作らない。ユーザーは main で試す）。

Phase の番号は表の順でなく、レビューで分けた物に新しい番号（p024〜p029）を付けた。各 phase.md は `plan/ws328/phaseNNN/phase.md`（spec-module-delegation の §4.6 の 7 節）。

**header の誤りの扱い（再凍結）**: 実装か統合の Phase が header の契約の誤り（仕様と違う・矛盾・未定義）を見つけたら、その Phase は header を変えずに止めて Q1 へ返す。Q1 は（設計の担当に頼んで）header と design.md を直し、その commit を新しい基点として ws.md の Resume point に書き、まだ始めていない Phase の基点を差し替える。済んだ Phase の .c が直しの影響を受ければ、直しの Phase を立てる。p023 は書き方だけで、契約の直しを持ち越さない。

## 12. WS074 との関係（再開の時に合わせる）

- WS328 は WS074 の file を変える: `paint/paint.h`・`list.c`・`scene.c`・`software.c`・`vulkan.c`・`shaders/`・`gradient.c`（消す）、`page/svg.c`・`page/images.c`、`css/css.h`・`css/gradient.c`・`css/values.c`、`layout/shape.c`（shape-outside が gradient を読む）、computed value の直列化（`bind/` の gradient を文字列にする所）、`sources.mk`。
- WS074 の再開の時の前提: display list の装飾は §7 の item、CSS の gradient は §9.1 の形、SVG は svg/ の module。WS074 の design.md §8.1・§8.2 の「SDF の角丸を shader で」「offscreen の layer で opacity・filter」のうち、前者は WS328 で実現し、後者（GPU の layer）は WS074 の側に残る（§14）。
- WS074 の host の試験（`plan/ws074/tests/`）の期待の値（paint の dump、画素）で WS328 の変更が変える物は、WS328 の統合の Phase が直し、各 phase.md の結果に file を全部書く（WS345 と同じ扱い。2026-11-14 Q1 が許した: ws.md の Resume point）。WS328 の新しい試験（p017 の host-css-gradient など）は `plan/ws328/tests/` に置く。

## 13. make の変更の案（承認が要る: [user-questions.md](user-questions.md) の U5）

`sources.mk` は make の断片なので Makefile と同じく Claude は変えず、差分の案をユーザーに出す（2026-11-14 Q1 の判断、ws.md）。承認まで vector/・svg/ は試験の script だけが compile する（libbrowser の build には入らない）。当てる時期は p018 の始め（paint が vector/ を呼び始める時）。

案 1（p018、vector/・svg/ を足す）: `$(LIBBROWSER_DIR)/layout/bounds.c \` の行（今の 228 行目）の次、`$(LIBBROWSER_DIR)/paint/list.c \` の前に、次の 16 行を挿す（各行は tab で始まる）:

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

（git の diff の形は前後の行の tab と空白が `git diff --check` に掛かるので、挿す行と場所で示す。当てる時は Q1 が `git diff` を作ってユーザーに見せる。scratchpad で context 付きの diff を作り `git apply --check` が通る事を確かめた。）

案 2（p025、`paint/gradient.c` を消す）: `$(LIBBROWSER_DIR)/paint/gradient.c \` の 1 行を消す。

案 3（p021、木の adapter）: `$(LIBBROWSER_DIR)/page/svg.c \` の次に `$(LIBBROWSER_DIR)/page/svg-tree.c \` を足す（page/svg.c は p028 で薄い口として残す）。

各案は、その Phase の始めにユーザーの承認を Q1 経由で得てから当てる（承認の後、当てるのは Q1 か Q1 が許した担当）。browser-win32 の Makefile は sources.mk を読むので、そちらは変えずに済む見込み（p018 が確かめる）。WS074 の `plan/ws074/tests/host-build.sh` は libbrowser の source の一覧を読むので変えずに済む見込み。module の .c は include だけで engine に依らないので、足すだけで build は壊れない（骨格のままでも compile と link が通る: check-skeleton の 2 と 6）。

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
| clipPath の子の clip-path | 適用しない | svg-clip の拡張 |
| SVG の pathLength = 0 で最初の dash が 0 の時 | 何も描かない（zero-length の dash の cap も省く） | — |
| path の coverage の窓（§4）が `VEC_RASTER_TILES_MAX` を超える物 | その path を描かない（数える） | 窓を分けて作る |
| 1 frame の coverage の予算を超えた path | 次の frame に回す（それまで前の coverage か無し） | — |
| 256 を超える ramp が 1 frame に要る時 | 超えた item を平均の色で | ramp の texture を広げる |
| SVG の言語の条件（systemLanguage） | "en" とみなす | 頁・OS の言語を渡す |
| `<color-interpolation-method>`、CSS の色空間（oklab など） | sRGB の補間だけ | CSS Color 4 の実装の後 |
| Canvas 2D | 無い | vector/ を土台にできる |
| devicePixelRatio ≠ 1 | 1 のまま | DPI の倍率の導入の時（guardrail の論理 px） |

## 15. 試験の道具（p001 が作り、実装の担当は変えない）

| file | 役目 |
| --- | --- |
| [tests/run-unit.sh](tests/run-unit.sh) | `test-<名前>.c` を全 module（未実装は骨格）と ASan・UBSan・`-std=c11 -Wall -Wextra -Werror` で build して流す。最後の行 `run-unit: PASS`／`FAIL`。`CC=clang` も |
| [tests/check-skeleton.sh](tests/check-skeleton.sh) | header の単独の compile（cc・clang、二重の include）、全 .c の compile、engine の header を include しない事、開発の管理情報の不在（WS・Phase・Queue・Bug の番号、Q1、日付、plan/）、`BASE=<SHA>` で header の不変と追加の無さ（git に入れていない header も数える）、zedBSD の clang と x86_64 の sysroot での compile（既定は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot`、`WS328_ZEDBSD_CC`・`WS328_ZEDBSD_SYSROOT` で変えられる。package.mk の x86_64 の flag）。toolchain か sysroot が無ければ最後の行は `check-skeleton: INCOMPLETE`（PASS にならない） |
| [tests/unit/check.h](tests/unit/check.h) | 判定の macro（整数・float の近さ・色の近さ）、決定的な乱数（xorshift32） |
| [tests/unit/raster-reference.h](tests/unit/raster-reference.h) | coverage の定義の独立の参照（各副走査線で 256 点の winding を数える力ずく）。vec-raster の試験の照合に使う。幅 1024 を超える矩形と 4096 を超える交差は -1 で断る（切り詰めた参照で試験が通らないように） |
| [tests/unit/svg-test-tree.h](tests/unit/svg-test-tree.h) | 試験用の小さな markup の reader と `struct svg_tree` の関数（parent を含む）、試験用の色の parser |
| [tests/unit/test-harness.c](tests/unit/test-harness.c) | 道具の自己試験（手の計算の coverage、markup、乱数）。骨格のままで PASS |

統合の試験（p018〜p028 が作り、p022 がまとめる）: WS074 の `gpu-compare.py`（CPU と GPU の画素の比べ、host の lavapipe）に頁の集合 `plan/ws328/tests/pages/*.html` を足す（角丸・border の各 style・影・各 gradient・SVG の図形・線・dash・gradient・clip、倍率の transform、view transition の頁に角丸と SVG）。WPT は WS074 の `fetch-suites.sh wpt` と `run-wpt-reftests.py` で、`css/css-backgrounds/`（border-radius・box-shadow）、`css/css-images/`（gradient）、`svg/shapes/`・`svg/painting/`・`svg/pservers/`・`svg/coordinate-systems/`・`svg/paths/`・`css/css-masking/clip-path/`（SVG の clipPath）の reftest を流して数える。基準（変える前の数）は p018 の始めに取って p018 の結果に書き、p022 が後の数と比べる。

## 16. 資料

仕様は git に入れない。p001 が取得した物は担当の worktree の `build/sources/specs/ws328/`（`SHA256SUMS` 付き、text にした物は `txt/`）。main の checkout の build/ には写さない（2026-11-14 Q1 の判断）。各 Phase の担当は、前の担当の worktree の `build/sources/specs/ws328/` を自分の worktree の build/ に cp して `sha256sum -c SHA256SUMS` で照合するか（Q1 が場所を依頼文で示す）、同じ URL からパラメータの無い GET で取って照合する（W3C の TR の版は日付で固定される。照合が合わない時は Q1 に報告）。

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
| D7 | 影は右と左の辺ごとの積分に分け、直線部分は erf で厳密、角の曲線は 45° で二つに分け、急な半分は y の 8 点の中点則、緩い半分は部分積分で x の 8 点の中点則 | 角の帯を y だけで積分すると、曲線が水平に近い所で被積分関数が階段になり σ が小さく半径が大きい時に §6.1.2 の 5% を超える（レビューの数値実験で 5.9%）。この形は倍精度の数値実験で 0.009 以内（§18） |
| D8 | 新しい pipeline を足し、今の display の pipeline を残す | glyph の多い頁の頂点の data を増やさない |
| D9 | SVG は DOM を関数の表越しに読む | module を host で試験でき、DOM の変更から切り離せる |
| D10 | SVG の色は CSS の色の parser を callback で使う | 名前付きの色の表を二重に持たない |
| D11 | CSS の dashed・dotted は stroker の dash で | 角の丸みに沿う。長さは UA の任意（§3.2）なので p019 が決める |
| D12 | inline・`<img>` の SVG はベクトルの item（大きさに依らない）、背景の SVG は bitmap | tile の多い背景で item が増え過ぎない |
| D13 | 新しい module は make の承認まで試験の script だけが compile | Makefile・mk の変更は承認が要る（AGENTS.md §7） |
| D14 | 円錐の角は A&S 4.4.47 の多項式（`vec_atan2`）、指数は exp2 の形（`vec_exp`）で CPU と shader が同じ式 | 自前の shader compiler が Atan を断り、PowerVR は Exp も持たない。atan が 1 つあると pipeline 全体が作れない |
| D15 | ベクトルの item は CSS の倍率を幾何に焼かず `scale_x`・`scale_y` で持つ | list の scale と view transition の写像が item の箱を倍にする今の形に、arena の幾何を書き換えずに合わせる |
| D16 | SVG の clip は list の clip の表（部品の和と親の鎖）を item が番号で指す | 1 本の折れ線と rule 1 つでは和も入れ子も表せない |
| D17 | path は item ごとに quad 1 枚と tile の表（texture の間接参照） | tile ごとの instance は全画面の SVG で frame ごとに約 10 MB の頂点を書く |
| D18 | GPU の Phase（p020・p026・p027）を CSS・SVG の切り替え（p019・p024・p025・p021・p028）より先に main に入れる | 新しい item を GPU が描けない期間を main に作らない |
| D19 | SVG の referenced element（clipPath・gradient・stop）の style は DOM の祖先から（`svg_style_of`、`svg_tree.parent`） | CSS Masking 1 §6.1 と SVG 2 §14.2.4 の継承。referencing 側の style を渡す誤りを header で塞ぐ |

## 18. design-reviewer の指摘と反映

design-reviewer は 2 回走った。1 回目は前の担当（q2091）が素案 7cc9eca796 に対して起動し、結果が遅れて Q1 経由で届いた（High 7・Medium 15・Low 11、Q1 の要約で受けた。表の「1 回目」の欄の H・M・L の番号）。2 回目は q2098 の P4 が起動した（重大 6・中 21・軽 23、R の番号）。同じ指摘は 1 行にまとめた。数値の確かめ: 影の積分は scratchpad の Python（倍精度、密な数値積分との最大の差）で行った。

| # | 1 回目 | 重さ | 指摘（要点） | 採否 | 反映と理由 |
| --- | --- | --- | --- | --- | --- |
| R1 | M8 | 重大 | CSS の scale と view transition の写像は item の箱を倍にするが、ベクトルの幾何は倍にならない | 採る | §7 の倍率（`scale_x`・`scale_y`）、D15。p018 の範囲 |
| R2 | M8 | 重大 | view transition の snapshot が item を写して arena を捨て、`item->vector` が宙に浮く（use-after-free） | 採る | §7「深い写し」、p018 の範囲と ASan の試験 |
| R3 | — | 重大 | 自前の shader compiler は Atan を断り、PowerVR は Exp も無い。atan 1 つで pipeline 全体が作れない | 採る | `vec_atan2`・`vec_exp`（vec-base.h）、vec-gradient・vec-rrect の式、§8.3・§8.4、D14、p020 の compiler の確かめ。確かめ: `src/drivers/gpu/compiler/spirv.c` の GLSL.std.450 の列挙と PowerVR の `shader-spirv.h` を読んだ |
| R4 | — | 重大 | p019・p021 が p020 より先に main に入ると GPU で装飾と SVG が消える | 採る | §11 の依存（GPU の p020・p026・p027 を先に）、§9・§10、D18 |
| R5 | H6 | 重大 | item の clip（折れ線 1 つと rule 1 つ）は SVG の clip（部品の和と親の積）を表せない | 採る | §7 の clip の表（`struct paint_vector_clip`）、D16 |
| R6 | M9 | 重大 | `svg_picture_build` に HTML から継承する color の口が無く、currentColor のアイコンが黒になる | 採る | `svg_picture_build` の `outer`（svg-picture.h）、§10「継承の口」 |
| R7 | H1 | 中 | pathLength の dash の倍率が逆、全体の長さ、pathLength = 0 | 採る | vec-stroke.h の dash_scale（§9.6.1 に従い、§13.5.7 の文の向きの誤記を注記）、svg-picture.h の pathLength、p004・p013 の試験 |
| R8 | M1 | 中 | moveto だけの subpath を stroke しない（§13.5.1） | 採る | vec-flatten.h・vec-stroke.h、p004 の試験 |
| R9 | M2 | 中 | CSS の hint は位置で fixup の step 2 に入る、H = 0・1 は硬い遷移 | 採る | `struct vec_stop` の is_hint・has_hint、`vec_gradient_fixup_css(stops, &count)`、C の式と端、p006 の試験（red 50%, 20%, blue） |
| R10 | M4 | 中 | CSS の 0〜100% の外の stop、退化した radial | 採る | `vec_gradient_extend_span`、§9 の表（退化は §3.2.3 に従い p025） |
| R11 | M3 | 中 | 退化した repeating の平均は等間隔に置き直す（§3.3） | 採る | `vec_gradient_average` の comment、p006 の試験に仕様の例 |
| R12 | H2 | 中 | 影の角の帯の 8 点の中点則では σ が小さく半径が大きいと 5% を超える | 採る（形を変えて） | 1 回目の案（32 点か間隔 ≤ σ/4）は点が多く GPU が重い。辺ごとに分け 45° で二つに分ける形（vec-rrect.h、D7）にした。倍精度で密な積分との差: σ 0.5〜3・角 8〜100 px・楕円の角で最大 0.0085（旧の形は 0.059）。σ 10・30 も 0.006 以内。p007 の許容は 0.015 |
| R13 | M10 | 中 | `vec_rrect_side_share` の遷移の領域・組み合わせが未定義、和が 1 にならない | 採る | 中央線で 4 象限に分け、象限の角の線で 2 辺が分ける閉じた式（和は構成で 1）。細い箱の中央線での不連続は既知と header に書いた |
| R14 | M7 | 中 | 今の PAINT_RECT は辺を整数に丸める。rrect は端数のままで継ぎ目と縁が出る | 採る | §7「辺の丸め」、`vec_rrect_coverage` の comment |
| R15 | M11 | 中 | coverage の cache の粒度、大きな path、animation で毎 frame CPU が作る | 採る | §4「cache の粒度と予算」（窓を tile に揃えて 1 回で作る、予算、超えたら次の frame）、`vec_rasterize` の窓の注記、U1 に例外を書いた |
| R16 | L8 | 中 | tile ごとの instance で frame ごとの頂点の data が大きい、gradient の tile の繰り返し | 採る | path は quad 1 枚と tile の表（§8.1・§8.2、D17）、gradient の繰り返しは shader の周期（§8.1・§9） |
| R17 | — | 中 | 影の loop の GPU の負荷と早い抜けが無い | 採る | header に早い抜け（半分の行が cy ± 4σ の外なら飛ばす）を式で書いた。Pi 4 の計測を p020 の試験の一覧に |
| R18 | M9 | 中 | clipPath・gradient の stop は DOM の祖先から継承する。tree に parent が無い | 採る | `svg_tree.parent`、`svg_style_of`（svg-style.h）、svg-clip.h・svg-pserver.h の引数を外側の style に、D19 |
| R19 | H3 | 中 | command が realloc で伸びる stops・dashes を指す | 採る | command に dash_first・stop_first、pointer は build の終わりに付ける（svg-picture.h） |
| R20 | M6 | 中 | scale > 100 で user 空間の tolerance が下限を割り線が消える、EINVAL の扱い | 採る | `VEC_TOLERANCE_MIN` を 1e-6 に（作業量は `VEC_FLATTEN_SEGMENTS_MAX` で抑える）、svg-draw.h に max(…, MIN) と EINVAL は命令を飛ばすと書いた |
| R21 | M12 | 中 | svg-bitmap の float の layer は 256 MiB／枚 | 採る | layer は 8 bit の premultiplied、`SVG_BITMAP_LAYER_BYTES_MAX` 64 MiB、超えたら alpha に掛ける近似（svg-bitmap.h） |
| R22 | H4・H5 | 中 | SVG の walk と raster に作業量の上限が無い | 採る | `SVG_VISITS_MAX`・`SVG_POINTS_MAX`・容器の bbox を 1 回だけ（svg-picture.h）、`VEC_RASTER_CROSSINGS_MAX`・`VEC_RASTER_TILES_MAX`（vec-raster.h） |
| R23 | M13 | 中 | inline の SVG を viewport で切っていない | 採る | §10、p021 |
| R24 | — | 中 | header の契約の誤りを p023（書き方だけ）に持ち越せない | 採る | §11「header の誤りの扱い（再凍結）」、p018〜p028 と p023 の phase.md |
| R25 | M15 | 中 | p019・p020・p021・p013 が 1 回の起動に大き過ぎる | 採る | p019 → p019・p024・p025、p020 → p020・p026・p027、p021 → p021・p028、p013 → p013・p029（§11） |
| R26 | L6 | 中 | check 6 の skip が PASS になる、sysroot の指定、toolchain の install の規則 | 採る | check-skeleton.sh: 既定を自分の worktree の build/ に、skip は `INCOMPLETE`（PASS でない）、flag を package.mk の x86_64 に合わせた（`-DHAL_ARCH_AMD64` は古い名前だった）。phase.md §6 に toolchain の入れ方を書き、各 Phase の起動の時に Q1 が許す形にした |
| R27 | — | 中 | U1 の前提「GPU で覆いを出すには buffer か loop が要る」は誤り（stencil-then-cover がある）、A の例外 | 採る | U1 に案 C と A の例外を書いた。推しは A のまま（stencil と MSAA の各 driver の対応が未確認で、今の render pass に stencil が無い） |
| R28 | — | 軽 | 正の shoelace は y 下向きの画面で時計回り | 採る | vec-stroke.h |
| R29 | — | 軽 | 長さ 0 の線分を挟む join の向き | 採る | vec-stroke.h（前後の長さのある線分） |
| R30 | — | 軽 | `VEC_DASH_PIECES_MAX` の残りの扱いが曖昧 | 採る | 最後の dash から contour の終わりまで実線（vec-stroke.h） |
| R31 | — | 軽 | `vec_gradient_sample` の点が pixel の角か中心か | 採る | 点そのもの、renderer は中心を渡す（vec-gradient.h の file の comment） |
| R32 | — | 軽 | radial・conic の ramp の幅の長さが未定義 | 採る | `vec_gradient_line_length`（vec-gradient.h）、svg-bitmap.h がそれを使う |
| R33 | — | 軽 | start = end の linear で REPEAT だと最初の stop の色 | 採る | spread を通さない（vec-gradient.h） |
| R34 | L4 | 軽 | 円錐の中心の atan2(0, 0) | 採る | 中心は t = 0（vec-gradient.h）、`vec_atan2` は (0, 0) で 0 |
| R35 | — | 軽 | 極小の半径で溢れる | 採る | `VEC_RADIUS_MIN`（1/256 px）未満は 0 |
| R36 | — | 軽 | 負の stroke-width を picture が「stroke 無し」とするのは style の無効の扱いと違う | 採る | svg-style.h で無効な宣言として捨てる、svg-picture.h を合わせた |
| R37 | — | 軽 | `const float radii[4][2]` に非 const の配列を渡すと gcc の C11 -Wpedantic が warning | 採る | `vec_path_append_rounded_rect(path, const struct vec_rrect *)`。gcc 14 の -std=c11 -Wpedantic で warning を確かめた |
| R38 | — | 軽 | transform の属性は単位付きの角度・px を受ける | 採る | svg-values.h |
| R39 | — | 軽 | use の translate(x, y) の向きと clip-path の座標系 | 採る | svg-picture.h（transform × translate、§5.5.2 を読んで確かめた） |
| R40 | — | 軽 | picture の作り直しの条件と持ち主の thread | 採る | §10 |
| R41 | — | 軽 | 資料の置き場所の記録の食い違い | 採る | §16 を Q1 の判断（worktree の build/）に、user-questions を直した |
| R42 | — | 軽 | p017 の依存の書き方、§12 の file の一覧の欠け | 採る | §11・§12 |
| R43 | — | 軽 | 実機の一覧に Pi 4 が無い | 採る | §8.4、p020・p022 |
| R44 | H7 | 軽 | 角丸の clip の下の影・影の knockout が箱の角丸を失う | 採る | 影は箱の rrect と spread・offset を送り shader が `vec_rrect_spread` で形を作る（§7・§8.1）。12 vec4 に収まる |
| R45 | — | 軽 | software.c に coverage の cache が無い | 採る | §7（同じ cache の module） |
| R46 | — | 軽 | `paint/paint.h` の comment に開発の管理情報 | 採る | p018 で消す（§7） |
| R47 | L5 | 軽 | raster-reference が交差の上限を超えると黙って捨てる、幅 1024 | 採る | -1 で断る形にし harness に試験を足した |
| R48 | — | 軽 | check 5 が git の外の header を見ない、check 4 が Q1・p 番号・日付を拾わない | 採る | check-skeleton.sh |
| R49 | — | 軽 | U3 の背景に SVG の fill と stroke の重なりの差を書く | 採る | user-questions.md U3 |
| R50 | L11 | 軽 | hint を entry に数えると CSS_GRADIENT_STOPS 16 では足りない、ramp の行 | 採る | §9.1（32 に）、§8.2（256 行を超えたら平均の色） |
| — | M5 | 中 | radial の相対の閾値・安定な根・r(ω) > 0・焦点が円上の repeat | 採る | vec-gradient.h の radial の式 |
| — | M14 | 中 | sources.mk はユーザーの問いへ、p017 の試験は plan/ws328/tests/ に | 採る | §13（U5）、§12、p017 |
| — | M15 | 中 | p022 の前の WPT の基準を p018 の初めに | 採る | §11・§15、p018 |
| — | L1 | 軽 | CSS Color 4 の節番号は §13.3・§13.5 | 採る | vec-gradient.h（txt で節を確かめた） |
| — | L2 | 軽 | path data の誤りは「最後の完全な引数の組」まで | 採る | svg-pathdata.h（§9.5.4 の例を引いた） |
| — | L3 | 軽 | switch の定義 | 採る | svg-picture.h（requiredExtensions・systemLanguage・requiredFeatures） |
| — | L7 | 軽 | pipeline の切り替えの費用 | 採る | §8.1（p020・p027 が測る） |
| — | L9 | 軽 | cache の key の衝突 | 採る | §4（当たったら幾何の写しと比べる）、§8.2 |

採らなかった物: 無し。形を変えて採った物: R12（1 回目の 32 点の案でなく分け方を変えた）、R27（案 C を足したが推しは A）。1 回目の L10 は Q1 の要約に無かった（未確認）。

確かめられていない事（p001 では）: 新しい shader の式が各 driver の compiler を実際に通る事、flat の入力 50 成分・頂点の入力 13 個が i915・V3D・PowerVR で通る事、§4 の予算の値、§8 の atlas の大きさで足りる事。p020・p026・p027 が確かめる。
