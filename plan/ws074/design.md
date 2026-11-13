<!-- awesome-plan project=zedbsd record=ws074-design -->

# WS074 設計: zedBSD の Web ブラウザ（`browser`）

Parent: [WS074](ws.md) / Phase: [ws074-p001](phase001/phase.md)
Status: 設計（2026-09-27、ws074-p001）。ユーザーの指示の原文は [ws.md](ws.md) の「目標」。人間の判断が要る点は §17「判断が要る点
（既定で進めた）」に置き、戻せる既定を選んで先へ進める。段階の目標値は §15、Phase の分割は §18。

## 0. 前提（再確認しない決定）

- **自前の engine**。HTML・CSS・layout・描画・network・画像・JavaScript・Wasm を base の中に自前の C で書く。外部の browser engine・
  JS engine・regex・Unicode の library を取り込まない（base の方針、[master-design-policy.md](../master-design-policy.md) §2.1）。
- 新しいコードは [coding-style.md](../coding-style.md) の全文（ANSI C の書き方、段落の comment、条件の中で関数を呼ばない…）。
  `python3 plan/tools/style-check.py` で新しい file は 0 件。**設定に環境変数を使わない**（§12）。設定は command line と file。
- 外部の試験 suite（html5lib-tests、WPT、test262、WebAssembly の spec test）と参照用の font は **source tree に入れない**。取得して
  commit の SHA で検証し、`build/ws074-suites/` に置く（§14）。ライセンスは取得の script が確かめる。
- zdesktop の非標準の拡張（titlebar・System Menu・glass）は **libkeiland の API だけ**で使う（WS069 design.md §0 の規則）。
  zdesktop・libkeiland・libwayland は desktop のサブエージェントの持ち物で、この WS からは変えない（要るときは main に伝える）。
- PNG は `libpng-compat`（WS035 D2〜D4、作るのは ws071-p010）、zlib の inflate は `libz-compat`（ws035-p040）。この WS は
  第二の実装を書かない。JPEG の `libjpeg-compat` はこの WS が新しく作る（§10）。
- 試験は amd64 だけ。Phase の途中は build が通れば進み、試験（host・guest・boot test）は Phase の最後に 1 回（2026-09-27 ユーザー）。

## 1. 全体の形

```
                 ┌──────────────── browser（1 process、main thread）────────────────┐
 Wayland ◀──────▶│ shell/   窓・入力・titlebar（CONTROLS→TABS）・toolbar・タブ・System Menu        │
 (zdesktop)      │   │                                                                        │
                 │ page/    タブごとの browsing context: event loop の task・microtask・描画の機会  │
                 │   ├── html/  tokenizer・tree builder・serializer・encoding                   │
                 │   ├── dom/   Node・Element・Document・Event（GC の cell）                    │
                 │   ├── css/   tokenizer・parser・selector・cascade・computed style             │
                 │   ├── layout/ box tree・block・inline・float・position・flex・grid・table     │
                 │   ├── text/  font の一覧・libtruetype・shaping（軽い）・改行                   │
                 │   ├── paint/ display list・stacking・rasterizer（CPU）                       │
                 │   ├── image/ JPEG（libjpeg-compat）・PNG（libpng-compat）・GIF               │
                 │   ├── bind/  WebIDL から生成する DOM の binding                               │
                 │   ├── js/    lexer・parser・compiler・組み込み・RegExp        ┐               │
                 │   ├── wasm/  decoder・validator・compiler・JS API             ├─ vm/ の上      │
                 │   └── vm/    共通の実行 engine: 値・GC heap・object・bytecode・interpreter    │
                 │ net/     URL・HTTP/1.1・TLS（OpenSSL を dlopen）・cookie・cache・loader       │
                 │   └── resolver thread（getaddrinfo が止まるため、これだけ別の thread）       │
                 └─────────────────────────────────────────────────────────────────────────────┘
```

- **process**: 最初は 1 process（UI と engine と network が同じ process）。1 つの窓に複数のタブ。New Window は自分を新しい process で
  起動する（files・terminal と同じ）。process の分離（タブ・site ごとの renderer process と sandbox）は後の WS
  （§16 の Future）。理由: 最初に process の境界を作ると IPC・共有 memory・描画の受け渡しが engine より先に大きくなる。境界になる所
  （shell ↔ page、page ↔ net）は関数の interface として分けておき、後で IPC に置き換えられる形にする。
- **thread**: main thread が Wayland・入力・engine・JS・network の非同期 I/O（`poll`）をすべて回す。別の thread は DNS の resolver
  だけ（`getaddrinfo` が block するため。結果は pipe で main へ）。画像の decode は最初は main thread で同期（後で decoder thread）。
  JS は仕様どおり単一の thread（Worker は後）。
  main thread の stack は kernel の上限の 1 MiB（`EXEC_STACK_HARD_MAX`、ELF の `PT_GNU_STACK` はそれ以上を拒む）なので、再帰の深い処理
  （parser、layout、JS）が要るようになったら、main は大きな stack（例 16 MiB）の thread を 1 つ作ってそこで program の全体を回す。
  再帰にはそれぞれ深さの上限を置く。
- **タブ = 1 つの agent**: タブごとに VM の heap・realm・document を持つ（タブの間で GC の object を共有しない）。1 つのタブの GC は
  他のタブを止めない。`window.open` の opener の関係は後（同じ agent cluster が要る）。
- **headless の mode**: 同じ program に command line の mode を持たせる（試験と guest での計測のため。別の command 名を増やさない）。
  - `browser --render=PAGE --output=OUT.ppm [--width=W --height=H]`: 窓を開かずに描いて PPM（P6、alpha は無し）に書く。
  - `browser --dump=layout|dom|style PAGE`: 試験用の text の dump（html5lib の形の DOM、box の位置）。
  - `browser --js=FILE`（test262 の shell: `print`・`$262`）、`--wasm-spec=JSON`（spec test の runner）。
  - host の試験は同じ source を host の C compiler で build した同じ entry（Wayland と Vulkan を除く）を使う（§14）。

## 2. source の置き場所と module

`userland/desktop/browser/`（program `/bin/browser`、窓の題名は page の title、app_id `browser`）。
package の Makefile は directory の直下に 1 つだけ（build は `userland/*/*/Makefile` を拾うため、下の directory に Makefile を置かない）。

| directory | 接頭辞 | 中身 |
| --- | --- | --- |
| `base/` | `wb_` | arena、可変長配列、文字列 buffer、UTF-8/UTF-16、hash、log。OS に依存しない |
| `vm/` | `vm_` | 値、GC heap、string・atom、object・shape、bytecode、interpreter、realm、embedding の API（§11） |
| `js/` | `js_` | lexer、parser（AST）、scope、compiler、組み込み object、RegExp（§12） |
| `wasm/` | `wasm_` | binary の decoder、validator、共通 bytecode への compiler、`WebAssembly` の JS API（§13） |
| `html/` | `html_` | encoding の判定と decoder、tokenizer、tree builder、fragment、serializer（§3） |
| `dom/` | `dom_` | Node の木、属性、namespace、Event、Range（後）。GC の cell（§4） |
| `css/` | `css_` | tokenizer、parser、selector、cascade、property の表、値、media query（§5） |
| `text/` | `text_` | font の一覧と選択、libtruetype、shaping（kerning まで）、改行、Unicode の表（§7） |
| `layout/` | `layout_` | box tree、formatting context ごとの file、fragment（§6） |
| `paint/` | `paint_` | display list、stacking context、rasterizer、layer（§8） |
| `image/` | `img_` | 画像の種類の判定、JPEG・PNG・GIF、decode した画像の cache（§10） |
| `net/` | `net_` | URL、fetch の簡略版、HTTP/1.1、TLS、resolver、cookie、cache、MIME（§9） |
| `bind/` | `bind_` | 自前の WebIDL の file と、build のときにそこから生成する binding（§12.4） |
| `page/` | `page_` | browsing context、event loop、script の読み込み、navigation、history |
| `shell/` | `shell_` | Wayland の窓、Vulkan の swapchain、titlebar、toolbar、タブ、menu（§8.4） |

- 各 directory は公開の header を 1 つ（`vm/vm.h`、`html/html.h` …）と内部の header を持つ。依存は下向きだけ:
  `base` ← `vm` ← `js`・`wasm` / `base`・`vm` ← `dom` ← `html`・`css` ← `layout` ← `paint` ← `page` ← `shell`。`net`・`image`・
  `text` は `base` だけに依存する。
- `vm/`・`js/`・`wasm/` は後で base の library（例 `libzvm`）へ切り出せるように、browser の他の部分を include しない。
- 生成する表（文字参照、Unicode の性質、日本語の encoding の表）は `tools/` の生成 script と生成物の `.c` を置く（§17 の D4）。

## 3. HTML

### 3.1 入力と encoding

- byte の列 → encoding の判定（BOM → HTTP の `charset` → `<meta>` の prescan（最初の 1024 byte）→ 既定 windows-1252、日本語の
  locale の既定は後で選べる）→ decoder → **UTF-16 の code unit** の入力 stream。`document.write` の文字列（JS の UTF-16）をそのまま
  入力の位置へ差し込める。改行の正規化（CR LF → LF）は stream の段で行う。
- decoder: UTF-8、UTF-16LE/BE、windows-1252（と Latin-1 の別名）を最初に（p018）。Shift_JIS・EUC-JP・ISO-2022-JP と他の single-byte
  の legacy encoding を同じ Phase で（表は §17 の D4）。

### 3.2 tokenizer

- WHATWG の tokenizer の状態機械（全部の状態、character reference の名前 2231 件の表、数値参照の置き換え表）。token は start tag
  （名前は atom、属性の配列）、end tag、comment、DOCTYPE、文字の run（連続する文字をまとめる）、EOF。parse error は数えるだけ
  （試験の比較用に種類も残す）。
- 入力の位置（insertion point）を持ち、`document.write` の再入と script の待ちで止まれる形（tree builder が tokenizer を駆動する
  pull 型）。

### 3.3 tree builder と DOM の構築

- WHATWG の tree construction（23 の insertion mode、open element の stack、active formatting element の list と adoption agency、
  foster parenting、template の insertion mode の stack、foreign content（SVG・MathML の名前の補正））。
- scripting の flag を持つ（JS の前は off で試験し、JS の後は on）。parser の入れ子の深さは 512 で打ち切る（C の再帰と layout の
  深さを抑える。Blink と同じ値）。
- fragment parsing（`innerHTML`・`insertAdjacentHTML`）と serializer（`innerHTML` の getter、試験の dump）。

### 3.4 試験

html5lib-tests の tokenizer（JSON）と tree-construction（`.dat`、`#script-off`・`#script-on`・fragment）。runner は python が
test の file を読み、headless の engine の batch mode（長さを前置きした入力の列を読み、token・木を決まった text で出す）と比べる。

## 4. DOM と GC

- DOM の Node は **VM の GC heap の cell**（§11.2）。Node の寿命は到達可能性で決まる（document の木、JS の変数、event の listener
  から）。JS の wrapper は別の object にせず、Node の cell 自身が JS の object を兼ねる（最初の field が VM の object header）。
  理由: wrapper と Node の二重の寿命の管理（参照の循環、finalizer の順序）を無くす。
- 文字列（text の data、属性の値）は VM の string（Latin-1 か UTF-16、§11.3）。要素の名前・属性の名前・namespace は atom
  （intern した string、永続）で、名前の比較は pointer の比較。
- 属性は要素の中の配列（`id`・`class` は取り出して別に持つ: selector の照合が速い）。class の list は atom の配列。
- computed style と layout の box は GC の外（`malloc`）。document が持ち、Node の削除のときに同期して外す（Node が GC で消える前に
  必ず box が無い）。
- 変更の通知: 子の変更・属性の変更・text の変更で style と layout の dirty の bit を立てる（最初は document 全体を作り直し、後で
  部分の再計算、§6.5）。MutationObserver は bind の Phase で。

## 5. CSS

### 5.1 構文と stylesheet

- CSS Syntax 3 の tokenizer と parser（stylesheet → rule の列、宣言の列、at-rule）。未知の at-rule・宣言は仕様どおり読み飛ばす。
- at-rule: `@media`、`@import`、`@charset`、`@supports`、`@font-face`（TTF/OTF。WOFF は libz-compat の後、WOFF2 は Brotli が要るので
  後）、`@keyframes`、`@layer`、`@container`（後）。
- stylesheet の元: UA stylesheet（HTML の rendering 節の内容を自前で書いた `css/ua.css` を C の文字列として埋める）、`<style>`、
  `<link rel=stylesheet>`、`style` 属性、user stylesheet（後）。

### 5.2 selector と cascade

- Selectors 4 の parse と照合（右から左）: type、universal、class、id、属性（`= ~= |= ^= $= *=`、`i`・`s`）、結合子（空白 `>` `+` `~`）、
  構造（`:first-child`・`:nth-child(An+B of S)`…）、`:not`・`:is`・`:where`・`:has`（後）、状態（`:hover`・`:active`・`:focus`・
  `:focus-visible`・`:link`・`:visited`（常に未訪問として扱う: privacy）・`:checked`・`:disabled`…）、pseudo-element（`::before`・
  `::after`・`::marker`・`::first-line`・`::first-letter`・`::placeholder`・`::selection`）。
- rule の索引: 最右の compound の id・class・tag の hash と、それ以外の list（照合する rule を絞る）。
- cascade: origin と重要度、`@layer`、specificity、出現順。継承、`initial`・`inherit`・`unset`・`revert`・`revert-layer`。
- custom property と `var()`（最初から。今の site の多くが使う）、`calc()`・`min()`・`max()`・`clamp()`。

### 5.3 property と値

- property の表（名前、継承するか、初期値、parse の関数、computed の型、animatable、shorthand の展開）を 1 つの C の表にする。
  shorthand（`margin`・`border`・`font`・`background`・`flex`・`grid`…）は parse のときに longhand へ展開する。
- computed style は element ごとの struct（まず全 property を 1 つの struct に持つ。memory が問題になったら継承の group ごとに共有の
  struct へ分ける）。長さは computed の段で px の float に、layout は固定小数（§6.1）に直す。
- `-webkit-` の接頭辞: 標準の property の別名の表（`-webkit-box-sizing` → `box-sizing` など）と、Chrome が今も持つ非標準の property
  （`-webkit-line-clamp`、`-webkit-text-fill-color`、`-webkit-text-stroke`、`-webkit-box` の古い flexbox、`-webkit-appearance`…）。
  Chrome との比較の Phase で表を増やす（§15.3）。

## 6. layout

### 6.1 単位と木

- 長さは **1/64 px の固定小数の int32**（Blink の LayoutUnit と同じ）。float の誤差の積み重ねを避け、Chrome の丸めに近づける。
- DOM + computed style → **box tree**（block container、inline box、text run、replaced、anonymous の block・inline、`display: contents`
  の飛ばし、`::before`・`::after`・`::marker` の box）→ layout → **fragment**（line box、各 box の位置と大きさ）→ display list。
- formatting context ごとに file を分ける: `block.c`（BFC、margin の相殺、幅の計算、clearance）、`inline.c`（IFC、line box、
  改行、`vertical-align`、`text-align`、`white-space`）、`float.c`、`position.c`（relative・absolute・fixed、後で sticky）、
  `flex.c`、`grid.c`、`table.c`、`replaced.c`（img・video の placeholder・canvas・form control の固有の大きさ）、`list.c`（marker）。
- viewport と scroll: document の scroller。`overflow: auto|scroll|hidden` の box は scroll の位置を持つ（描画は clip と移動）。

### 6.2 順序

block と inline（p011）→ float・position・overflow・list・単位（p013）→ flex（p035）→ table（p037）→ grid（p039）→ 多段組・
sticky・writing-mode（縦書き）・ruby は後（§16）。

### 6.3 text の配置

inline の中の text は、white-space の処理 → 改行の機会（UAX #14 の簡略版: 空白、CJK の文字の間、禁則の一部）→ font の選択
（文字ごとの fallback）→ shaping（§7）→ line box に詰める。bidi（UAX #9）と複雑な script（アラビア文字・インドの文字）の shaping は
後（§16）。

### 6.4 試験の dump

`--dump=layout` は box の種類・位置・大きさを 1 行ずつ出す。Chrome の比較（§14.5）はこれと Chrome の `getBoundingClientRect` を比べる。

### 6.5 再計算

最初は変更のたびに style と layout を document 全体で作り直す（正しさが先）。dirty の bit による部分の再計算は、JS から DOM を
動かす page で遅さが見えた Phase（p031 以降）で入れる。

## 7. text と font

- **font の一覧**: `/usr/share/fonts/` の TTF を起動のときに走査し、`name`・`OS/2` の表から family・weight・italic を読む。CSS の
  generic family（`serif`・`sans-serif`・`monospace`・`system-ui`・`cursive`・`fantasy`）は設定の file
  （`/etc/browser/fonts.conf`、無ければ組み込みの既定）で具体的な font の file へ対応させる。fallback は文字の cmap で選ぶ
  （日本語は keiland-fallback.ttf）。`@font-face` の font も同じ一覧に入る。
- **libtruetype**: 今の API は整数の pixel の大きさと整数の advance だけで、kerning・名前の表・`unitsPerEm` を返さない。browser には
  小数の大きさ（13.333px など）、1/64 px の advance、`kern`・`GPOS` の kerning、名前と `OS/2` の値、design unit の outline が要る。
  **libtruetype に関数を足す**（既存の関数は変えない追加だけ。p010。§17 の D8: main に伝えてから）。
- **shaping**: 最初は cmap → glyph、advance、kerning（`kern` の表と GPOS の pair adjustment）、合字は無し。HarfBuzz 相当の GSUB/GPOS
  の全体は後（§16）。
- **Unicode の表**: 一般 category、East Asian Width、Line_Break、大文字小文字の変換、JS の識別子の文字（ID_Start/ID_Continue）、
  正規化（`String.prototype.normalize`、後）、Grapheme の境界。UCD（Unicode Character Database）から生成した C の表（§17 の D4）。
- glyph の cache: (face, 大きさ, glyph, 小数の位置 1/4 px) → coverage の bitmap。

## 8. 描画と窓

### 8.1 display list

layout の fragment を CSS 2 の付録 E の順（stacking context、z-index、positioned、float、inline）に歩いて display list を作る:
矩形の塗り、border（solid・dashed・dotted・double・groove・ridge・inset・outset、角丸）、背景（色、画像の repeat・position・size、
gradient）、text の run、画像、clip（矩形と角丸）、opacity・transform・filter の group の始まりと終わり、box-shadow。

### 8.2 GPU（Vulkan）の描画と CPU の参照の描画

**2026-09-27 ユーザーの決定「ブラウザはWaylandとVulkanで実装してください。」**（D6 の既定の wl_shm を取り消し）。

- **窓の描画は Vulkan**: display list を Vulkan で描く（`paint/vulkan.c`）。primitive は 1 本の pipeline の instance の四角
  （四角の位置、色、4 隅の半径、border の幅、texture の座標、種類）で、fragment shader が角丸の距離関数（SDF）で AA の coverage を
  出す。text は glyph を libtruetype で CPU で描いて R8 の atlas の texture に置き、画像は texture。clip は矩形なら scissor、角丸は
  SDF の mask、opacity・filter の group は offscreen の image（layer）に描いて合成。scroll する box と固定の要素は layer に分け、
  scroll は layer の移動（再描画なし）で行えるようにする。shader は GLSL で書き、host の `glslc` と `spirv-val` で SPIR-V にして
  生成した header を commit する（files の `shaders/regenerate.py` と同じ方式。build に shader の compiler は要らない）。
- **CPU の参照の描画**（`paint/software.c`）: 同じ display list を CPU で描き、headless の `--render=PAGE --output=PPM` と host の
  試験（WPT の reftest、Chrome との比較）に使う。GPU の描画と同じ数式（pixel の中心での SDF の coverage、同じ glyph の bitmap、
  同じ画像の sampling）で書き、primitive の意味を 1 つの定義（`paint/paint.h` の display list の型）で共有する。
- **2 つの一致の確かめ方**: guest の Venus（と i915 の実機）で同じ page を Vulkan の offscreen の image に描いて読み戻し、CPU の参照の
  描画と画素ごとに比べる試験（`--render-gpu=PAGE --output=PPM` と比較の script）。許容は channel の差 ≤ 2（GPU の補間の差）で、
  それを超える画素が 0.1% を超えれば失敗。primitive を足す Phase はこの試験に page を 1 つ足す。

### 8.3 描画の機会

page の変更 → 次の frame callback（`wl_surface.frame`）で rAF → style → layout → paint → 表示。scroll は display list をそのまま
ずらして描き直す（最初）。damage の矩形を付ける。

### 8.4 窓（shell）

- **提示は Wayland の上の Vulkan**（libvulkan の Wayland WSI の `VkSurfaceKHR` と swapchain）。files の `window.c`・
  `present.c` の方式（premultiplied alpha、glass を使うなら keiland_glass_v1）に倣うが、canvas を 1 枚貼るのではなく display list を
  §8.2 の GPU の描画で直接 swapchain の image へ描く。
- **titlebar**: WS070 の titlebar の拡張（libkeiland の `keiland_titlebar_*`）。
  - ws070-p011（TABS の presentation）の前: **CONTROLS** mode（戻る・進む・再読み込み／中止・URL の text field・menu）。タブは
    無し（1 窓 1 タブ、Ctrl+T は新しい窓）。
  - ws070-p011 の後: **TABS** mode（タブの strip と `+`）と、窓の中の toolbar（戻る・進む・再読み込み・URL の欄）を browser が描く
    （titlebar-spec §4.3 の用途: browser が TABS の最初の本格的な使い手）。CONTROLS と TABS は排他（仕様 §5）なので、URL の欄は
    窓の中へ移る（§17 の D3）。
- **menu**: System Menu（WS070）で File（New Tab・New Window・Open File…・Close Tab）、Edit（Copy・Paste・Find）、View（Reload・
  Zoom・View Source）、History（Back・Forward）、Window、Help。link の上の右 click は `keiland_menu_popup`。
- 入力: pointer（hover・click・wheel・drag の選択）、keyboard（focus の移動、scroll、form の入力）。日本語の IME は zdesktop の
  text-input の対応の後（§16）。

## 9. network

- **URL**: WHATWG URL Standard の parser と serializer（special scheme、percent-encoding、IPv4・IPv6 の host、IDNA は punycode と
  簡単な mapping まで。UTS #46 の全表は後）。試験は WPT の `url/resources/urltestdata.json`。
- **fetch の簡略版**: request（method、URL、header、body、mode、credentials、redirect）と response（status、header、body の stream、
  type）。scheme ごとに `http`・`https`・`file`・`data`・`about`（`about:blank`、`about:version`）。same-origin policy と CORS（XHR・
  fetch、JS の後）、混在 content（https の page の http の script）の遮断。
- **HTTP/1.1**: 非同期（`poll`）の socket、host ごとに 6 本までの持続接続、chunked、Content-Length、redirect（301・302・303・307・
  308、20 回まで）、`Accept-Encoding: identity`（gzip・deflate は libz-compat の後）。HTTP/2・HTTP/3 は後（§16）。
- **DNS**: resolver の thread で `getaddrinfo`（IPv6 と IPv4）。
- **TLS**: 最初は **OpenSSL の package の library（`/usr/lib/libssl.so`・`libcrypto.so`）を `dlopen` して使う**（§17 の D2）。
  使う関数（20 ほど: `TLS_client_method`、`SSL_CTX_new`、`SSL_CTX_set_default_verify_paths`、`SSL_new`、`SSL_set_fd`、`SSL_ctrl`
  （SNI）、`SSL_set1_host`、`SSL_connect`、`SSL_read`、`SSL_write`、`SSL_get_error`、`SSL_shutdown`、`SSL_free`…）を自前の header で
  不透明な型と関数 pointer として宣言し、`net/tls.c` の中に閉じる。証明書は ca-certificates の `/etc/ssl/cert.pem`（OpenSSL の既定）。
  TLS 1.2 以上、hostname の照合、失敗は error の page（例外の許可は無し）。将来の base の libssl・libcrypto の互換品は同じ API の部分
  集合を実装し、`net/tls.c` は読む library の名前の順（base の互換品 → package）を変えるだけにする。
  （注: 2026-11-13 WS335 p042 で、`net/tls.c` は `dlopen` をやめ libssl-compat・libcrypto-compat を直に link する形に置き換えた。[WS335 design.md](../ws335/design.md) §4.2・D29）
- **cookie**: RFC 6265bis の部分（Domain・Path・Expires・Max-Age・Secure・HttpOnly・SameSite（既定 Lax））。保存は
  `~/.local/share/browser/cookies`。
- **cache**: 最初は memory の cache（`Cache-Control: max-age`、`no-store`）。disk の cache と再検証（ETag・Last-Modified）は後。
- 試験: host の python の test server（HTTP と、自前の CA で署名した HTTPS）。guest は QEMU の user network で host（10.0.2.2）の
  test server と、外に出られれば実在の site（例 `https://example.com/`）。

## 10. 画像

- 種類は内容の先頭の byte で判定（Content-Type より優先、MIME Sniffing の画像の部分）。decode は premultiplied BGRA へ。
- **JPEG**: 新しい base の library **`libjpeg-compat`**（`userland/base/libjpeg-compat`、`/lib/libjpeg-compat.so`、header
  `include/libc/compat/jpeglib.h` → `/usr/include/compat/jpeglib.h`）。WS035 の D1〜D4 に揃える:
  - API は IJG の libjpeg の古典的な decompress の API の部分集合（関数名は本家と同じ: `jpeg_std_error`、`jpeg_CreateDecompress`
    （`jpeg_create_decompress` の macro）、`jpeg_mem_src`、`jpeg_stdio_src`、`jpeg_read_header`、`jpeg_start_decompress`、
    `jpeg_read_scanlines`、`jpeg_finish_decompress`、`jpeg_abort_decompress`、`jpeg_destroy_decompress`、`jpeg_save_markers`）。
    error は本家と同じく `err->error_exit`（呼ぶ側が `longjmp` する）。`JPEG_LIB_VERSION` は 62（libjpeg-turbo の既定の build、
    Debian の libjpeg62-turbo と同じ）。出力の色は `JCS_RGB`・`JCS_GRAYSCALE` と libjpeg-turbo の拡張 `JCS_EXT_BGRA`・`JCS_EXT_RGBA`。
  - header の struct は自前で書く（本家の header を写さない）。本家の struct の配置との binary 互換は求めない（base のプログラムだけが
    使い、SONAME も違う: D2）。
  - 範囲: baseline と extended の huffman（8 bit）、progressive、restart、任意の subsampling、grayscale・YCbCr・RGB・CMYK/YCCK
    （Adobe）。IDCT の 1/2・1/4・1/8 の縮小（`scale_num`/`scale_denom`）は後の Phase。算術符号・12 bit・lossless・encode は後（§16）。
  - 試験: host で Pillow（host の libjpeg-turbo）が作った JPEG の組（品質・subsampling・progressive・restart・grayscale・CMYK）を
    decode し、Pillow の decode と各 channel ±2 以内で比べる（IDCT の実装差）。試験の画像は script が作り、commit しない。
- **PNG**: `libpng-compat` の simplified API。browser は network の bytes から読むので `png_image_begin_read_from_memory` が要る
  （ws071-p010 の範囲に入れてもらう: §17 の D7）。
- **GIF**: browser の中（`image/gif.c`）の LZW の decoder（最初の frame、interlace、透明色。animation は後）。base の他の program に
  要るようになったら library へ（§17 の D5）。
- **WebP・AVIF・SVG の画像**は後（§16）。

## 11. 共通の実行 engine（`vm/`）

JavaScript と Wasm が **同じ値の表現、同じ GC heap、同じ bytecode、同じ interpreter、同じ呼び出し規約** の上で動く。後の JIT は
この bytecode を入力にし、JS と Wasm の両方に一度に効く。

### 11.1 値（64 bit、NaN-boxing）

JSC の方式の NaN-boxing（offset をずらした double）:

| 種類 | 表現（64 bit） |
| --- | --- |
| pointer（object・string・symbol・bigint の cell） | 上位 16 bit が 0（x86-64 と aarch64 の user の address は 48 bit）、下位 3 bit が 0 |
| int32 | `0xFFFE0000_xxxxxxxx` |
| double | 生の bit に `2^49` を足す（NaN は正規化） |
| undefined・null・true・false・empty（穴） | 0x0A・0x02・0x07・0x06・0x00（page 0 の小さい値。pointer と重ならない） |

Wasm の値は **型が静的に決まる**ので box しない: register の slot に生の 64 bit（i32 は zero-extend、f32・f64・i64 は bit のまま、
v128 は 2 slot）。`externref` は JS の値そのもの（上の表現）、`funcref` は function の cell の pointer か null。

### 11.2 GC heap

- **非移動の mark-sweep**。cell は大きさの class ごとの block（64 KiB に揃えた block、block の集合で「この word は cell を指すか」を
  引ける）から取り、大きい cell は個別の block。cell の header に型（trace と finalize の関数の表の番号）と mark の bit。
- **root は保守的な stack の走査 + 正確な heap の走査**: C の stack（`setjmp` で register を吐き出してから、記録した stack の底まで）と
  VM の register の stack（JS の box した値と Wasm の生の bit が混ざるので保守的に）を走査し、heap の中は型ごとの trace 関数で正確に
  辿る。理由: C のコードに handle・root の登録を強いない（DOM・parser・binding の C が素直に書ける）。Wasm の生の i64 が偶然 address に
  見えても、非移動なので寿命が延びるだけで壊れない。**代わりに移動・圧縮する GC は選べない**（世代別は非移動の sticky mark bit で後）。
- 明示の root（global の realm、document、task queue、timer）は登録の API で。weak な参照（WeakMap・WeakRef・FinalizationRegistry、
  Node の listener）は後の Phase で ephemeron として。
- GC は allocation の中でだけ起きる（閾値: 前回の生存量の 2 倍）。タブごとの heap の上限（既定 1 GiB）を超えると out of memory の例外。
- host の ASan の下では走査の関数に `__attribute__((no_sanitize_address))`。

### 11.3 string と atom

- cell の string は Latin-1（1 byte）か UTF-16（2 byte）。連結の rope と部分の slice は後（最初は平の copy）。hash は遅延で計算して保存。
- atom: intern した string（property の名前、要素の名前、CSS の識別子）。atom の表は永続（GC しない）。property の key は atom か
  symbol か整数の index。

### 11.4 object

- **shape（hidden class）** の遷移の木 + slot の配列（prototype の chain、property の属性）。配列は dense な elements（穴の印は
  empty）と、疎になったら辞書。関数・Node・Wasm の instance・memory・table も同じ object header を持つ。
- inline cache: 各 property の命令は function の IC の表の番号を持つ（最初は単態の cache: shape → offset）。bytecode の形を最初から
  決めておき、JIT が同じ表を使う。

### 11.5 bytecode

- **register machine**。1 命令 = 32 bit の opcode の word + 決まった数の 32 bit の operand の word（opcode の表が数と種類を持つ）。
  register は frame の slot の番号、定数は function の定数の表の番号、jump は word の相対の offset。可変長の符号化の工夫は後。
- 命令の group:
  - **共通**: `mov`、`load_const`、`jump`、`call`（引数は連続する register）、`return`、`throw`、`loop_hint`（回数を数える: JIT の
    tier-up の入口）、`nop`。
  - **JS（動的な型）**: `add`・`sub`…（ToPrimitive・ToNumeric を含む）、`strict_eq`・`loose_eq`、`typeof`、`get_prop`/`put_prop`
    （IC の番号）、`get_elem`/`put_elem`、`get_var`/`put_var`（環境）、`new_closure`、`new_object`、`new_array`、`construct`、
    `call_method`、`get_iterator`・`iterator_next`、`yield`・`await`（generator の frame を heap へ退避）、`jump_if_true`（ToBoolean）…
  - **Wasm（静的な型）**: `i32.add` … `f64.sqrt`、変換、`load`/`store`（memory の番号と offset、境界の検査）、`memory.grow`、
    `table.get/set`、`call_indirect`（signature の検査）、`br_table`、`select`、`ref.null`・`ref.is_null`・`ref.func`、`global.get/set`、
    `br_if`（i32 ≠ 0）。
- Wasm の binary は validation のときに **この bytecode へ変換する**（Wasm の stack machine を register に割り当てる）。Wasm の
  bytecode を直接解釈しない。理由: interpreter と JIT が 1 つで済む。

### 11.6 呼び出し規約と frame

- VM の stack は連続した 64 bit の slot の配列。frame の header（呼び出し元の frame、戻りの pc、function、引数の数、JS の
  `this`・`new.target`）の後に引数、局所の register。interpreter も後の JIT も **同じ frame の配置** を使う（OSR が容易）。
- JS → JS と Wasm → Wasm の呼び出しは interpreter の loop の中で frame を積む（C の再帰をしない）。native 関数
  （`int fn(struct vm_ctx *, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)`）の中から JS を呼ぶと
  C で再入する（深さの上限で RangeError）。
- JS ↔ Wasm の境界は変換の stub（ToWebAssemblyValue・ToJSValue）を持つ function の cell。function の cell は `entry`（最初は
  interpreter の入口。JIT は後でこれを差し替える）を持つ。
- 例外: function ごとの handler の表（pc の範囲 → handler の pc と例外の register）で unwind する。Wasm の trap は Wasm の中では
  捕まえられず（exception handling の提案は後）、境界で `WebAssembly.RuntimeError` になる。

### 11.7 embedding の API（`vm/vm.h`）

`vm_create`（heap の上限）、`vm_realm_create`（global object）、`vm_eval_script`、`vm_call`、`vm_define_native`、microtask の queue と
checkpoint、host の hook（promise の reject の追跡、module の読み込み（後）、`import.meta`）、GC の root の登録。page の event loop は
これだけを使う。

## 12. JavaScript（`js/`）

### 12.1 parser

手書きの再帰下降 → AST（arena、compile の後に捨てる）。構文は ES2024 の全体を目標（ASI、regex literal と除算の区別、template、
destructuring、class（field・private・static block）、async・generator、optional chaining、`??`、module は後）。最初は全部を
eager に compile し、遅延の compile（関数の本体の source の範囲を残して、最初の呼び出しで compile）は起動の遅さが見えたら入れる。

### 12.2 compiler

scope の解析（var・let・const・関数の巻き上げ、closure に捕まる変数は環境の object へ、そうでない変数は register へ）→ bytecode。
`with`・直接の `eval` は環境を動的に引く遅い道で対応。

### 12.3 組み込み

Object、Function、Array、String、Number（最短の往復の十進表記は自前の実装）、Boolean、Symbol、Math（libm）、Date（UTC と libc の
local time）、RegExp（自前の backtracking の engine: `u`・`v`（後）・`y`・`s`・`d` flag、lookbehind、名前付きの group、
Unicode の property は §7 の表）、JSON、Error の類、Map・Set・WeakMap・WeakSet、Promise、Proxy・Reflect、ArrayBuffer・TypedArray・
DataView、BigInt、Iterator の helper（後）、Intl は後（§16）。

### 12.4 DOM の binding（`bind/`）

- 自前で書いた WebIDL の file（仕様の IDL の断片を写さず、使う interface・attribute・operation を書く）を build のときに python3 の
  生成器（`userland/desktop/browser/tools/gen-bindings.py`、build は既に python3 を使う）で C に変換する。生成物は
  `build/` に出て commit しない。生成器の出力は coding-style の特別な範囲（generated）として扱い、生成器の側を規約に合わせる。
- ws074-p030 の最初の版は、生成器が出すのと同じ形の interface の表（`bind/internal.h` の `struct bind_interface`: 名前、親、
  attribute の getter・setter、operation、定数）を手で書き、`bind/window.c` が表から interface object と prototype を作る。
  interface が増えたとき（p031・p032）に生成器へ移すかを決める。DOM の node の object は node が持ち（`dom_node.wrapper`）、
  object の `internal` が node を指す（`VM_KIND_PLATFORM`）。event handler（onclick 等）は listener の列の中の listener。
- window（global object）、document、Node の類、Event・EventTarget、console、timer、location・history、CSSOM（inline style、
  `getComputedStyle`）、geometry（`getBoundingClientRect`・`offset*`・`scroll*`）、XHR・fetch、localStorage、Canvas 2D（後）。

## 13. Wasm（`wasm/`）

- binary の decoder と validator（型の stack、control の stack）を 1 回の走査で行い、同時に §11.5 の bytecode を出す。
- MVP → bulk memory、reference types、multi-value、sign-extension、非 trap の変換、SIMD（interpreter で v128）→ exception handling・
  tail call・GC の提案（Wasm GC の struct・array は §11.2 の同じ heap の cell）→ 後。
- `WebAssembly` の JS API（`Module`・`Instance`・`Memory`・`Table`・`Global`・`validate`・`compile`・`instantiate`、streaming は後）。
- linear memory は GC の外の大きな buffer（`mmap`、上限 4 GiB の予約と境界の検査。guard page による検査の省略は JIT の時）。

## 14. 試験の戦略

### 14.1 置き場所と原則

- `plan/ws074/tests/`: host の build（`host-build.sh`: 同じ source を host の cc で、`-fsanitize=address,undefined` の組も）、
  suite の取得（`fetch-suites.sh`）、runner（python）、guest の image と操作、Chrome の比較。
- 数（通過の件数・率）は `plan/ws074/results/<suite>.txt` に Phase ごとに 1 行を足す（commit する。suite の中身は commit しない）。
- 通過率の分母は **取得した suite の commit ごとに固定**。suite の commit を上げたら基準を取り直し、その旨を記録する。

### 14.2 取得する suite（すべて commit の SHA を固定して検証、`build/ws074-suites/`）

| suite | 取得 | ライセンス（取得のときに LICENSE を確かめる） | 使い方 |
| --- | --- | --- | --- |
| html5lib-tests | git の固定 commit | MIT | tokenizer・tree-construction |
| WPT（web-platform-tests） | sparse の git clone（`acid/`・`css/`・`url/`・`dom/`・`html/syntax/`・`encoding/`・`resources/`・`fonts/`）の固定 commit | BSD 3-Clause | reftest、Acidの履歴的診断、testharness、urltestdata |
| test262 | git の固定 commit（`test/`・`harness/`） | BSD 3-Clause（Ecma） | JS |
| WebAssembly/spec の test | git の固定 commit（`test/core/`） | Apache-2.0 | `.wast` を host の `wast2json`（Debian の wabt の package）で JSON と `.wasm` に |
| Ahem の font | WPT の `fonts/Ahem.ttf` | WPT の中の表示に従う | CSS の reftest |

### 14.3 host と guest

- host: 全件（速い）。ASan・UBSan の組でも同じ runner を回し、crash を 0 にする。
- guest（amd64、Venus の zdesktop の guest）: 各 suite の決まった部分集合（例 test262 の 1000 件、html5lib の全部、WPT の reftest の
  100 件）を headless の mode で SSH から走らせ、**host と同じ結果**であることを確かめる（libc・libm の差を見つける）。窓の試験は
  zdesktop の上で browser を起動し、QMP で入力して画面を撮る（`plan/tools/files` の方式を真似る）。
- fuzz: host の clang の libFuzzer（HTML tokenizer、CSS parser、JS parser、JPEG・GIF の decoder、Wasm の decoder）。最後の Phase で
  target ごとに時間を区切って回す。

### 14.4 WPT の reftest

test と reference を **両方とも自分の engine で描いて** 比べる（`<meta name=fuzzy>` の許容を守る）。font の rasterize の差が
Chrome と違っても、自分の中で一致すれば通る。JS を使う reftest（`reftest-wait` など）は JS の後に数に入れる。

`run-wpt-reftests.py`は同じlocal HTTP originからresourceとAhemを配り、scriptを使わないtestをdirectoryごとのround-robinで固定抽出する。
`run-acid-tests.py`はWPTに保存されたAcid2・Acid3を履歴的な診断として描画する。Acid2は対話用の説明を隠して顔をviewportへ出すharnessだけを足し、
test rule自体は変えない。Acid3は変更せずscoreと例外を記録する。これらは現在の標準への適合証明には使わない。

### 14.5 Chrome との比較

- host に Debian の `chromium` を入れた（2026-09-27、`sudo apt-get install chromium`、153.0.8010.52）。headless で
  `--screenshot`（`plan/ws074/tests/chrome-shot.sh`、device scale 1、scroll bar 無し）が撮れることを確かめた。
- font を揃える: Chrome には fontconfig の設定（`FONTCONFIG_FILE`、比較の font だけを並べ、serif・sans-serif・monospace を同じ file に
  対応させる）を渡し、自分の engine には同じ file を §7 の設定で渡す。
- 2 つの指標:
  1. **box の位置の一致**: 各 element の border box を Chrome は注入した script（`getBoundingClientRect` を JSON にして
     `--dump-dom` で取り出す）、自分は `--dump=layout` で出し、4 辺が ±1 px 以内の element の率。anti-aliasing の差に左右されない。
  2. **画素の一致**: screenshot の差（channel の差 ≤ 16 を一致とする）が 99% 以上の page の率。text は Ahem で比べる組と、実際の
     font で比べる組を分ける。
- corpus: 自前で書いた小さな page（`plan/ws074/tests/pages/`、property ごと）と、実在の site の保存（commit しない。取得の日付と
  URL を記録）。`-webkit-` の property の page は §5.3 の表に対応させる。

## 15. 段階の目標値

「100% は無理なので段階的に上げる」（ユーザー）に従い、段階ごとに目標を決め、達したら次の段階の数を決める。数は §14.1 の
results に残す。

| 段階 | 時期（Phase） | HTML | CSS（WPT） | JS・Wasm | Chrome との比較 |
| --- | --- | --- | --- | --- | --- |
| **M1 静的な page** | p014 の終わり | tokenizer ≥ 98%、tree-construction（script-off）≥ 90%、URL ≥ 85%（p015） | CSS2 の JS 無しの reftest ≥ 25% | — | 自前の corpus の box ±1 px ≥ 70% |
| **M2 JS の接続** | p034 の終わり | tree-construction（script-on を含む）≥ 92% | CSS2 reftest ≥ 35% | test262 全体（intl402・staging・未予定の提案を除く）≥ 35%、ES5 の範囲 ≥ 80%、WPT dom/nodes ≥ 40%、Wasm の MVP の spec ≥ 90% | box ≥ 80% |
| **M3 CSS の互換 1** | p040 の終わり | ≥ 95% | CSS2 ≥ 55%、css-flexbox ≥ 40%、css-backgrounds・css-values・selectors ≥ 40% | test262 ≥ 60%、Wasm（bulk・reference・multi-value・SIMD を含む）≥ 90% | box ≥ 85%、画素 ≥ 50% の page |
| **M4 CSS の互換 2** | p043 の終わり | ≥ 97% | CSS2 ≥ 75%、flexbox ≥ 70%、grid ≥ 50%、tables ≥ 60% | test262 ≥ 80%、Wasm ≥ 95% | box ≥ 92%、実在の site の保存 20 件で画素 ≥ 70% |
| M5 以降 | 後の Phase・WS | 段階ごとに 5〜10 point ずつ上げる（達した時に次の数を決め、ws.md に記録） |

使える page の目安（人が見て確かめる）: M1 は file の静的な page と文書の site、M2 は http・https の簡単な site と少しの JS、M3 は
Wikipedia の記事が画像と共に Chrome に近く読める、M4 は GitHub のような今の site が読める（書き込みは問わない）。

## 16. 後回し（Future の候補、この WS の完了の条件に入れない）

process の分離と sandbox、JIT（baseline → 最適化）、HTTP/2・HTTP/3、disk の cache、Service Worker、Worker、WebSocket（M2 の後の
候補）、WebP・AVIF・SVG（画像と inline）、動画（driver の後）、音声と `libvorbis-compat`、Canvas 2D と WebGL（WS068 の GLES の上）、
Intl、bidi と縦書きと複雑な script の shaping、IME、印刷、拡張機能、WOFF2（Brotli）、JPEG の encode と算術符号、base の libssl・
libcrypto の互換品（別の WS）、GPU の合成、sync・password の管理。

## 17. 判断が要る点（既定で進めた）

| # | 点 | 既定（戻せる） | 他の案 | 理由 |
| --- | --- | --- | --- | --- |
| D1 | process の構成 | 1 process（UI・engine・network）、窓ごとに process、DNS だけ別の thread | 最初から renderer の process を分ける | 境界を関数の interface で分けておけば後で IPC にできる。**最初の版は敵意のある site に対して安全ではない**（sandbox は後） |
| D2 | TLS の繋ぎ方 | OpenSSL の package の `libssl.so` を実行時に `dlopen`（base の build は OpenSSL に依存しない。package が無ければ https は error の page） | build のときに link する（base の build が package の OpenSSL の cross build に依存し、image に package が必須になる） | base と package の境界を保ち、将来の base の互換品へ読む名前を変えるだけで移れる |
| D3 | titlebar の使い方 | ws070-p011 の前は CONTROLS（URL の欄を titlebar に、1 窓 1 タブ）。後は TABS（タブを titlebar に）+ 窓の中の toolbar | CONTROLS のまま窓の中にタブ | 仕様（titlebar-spec §4.3）が browser を TABS の主な用途としている |
| D4 | 仕様や Unicode から作る表（文字参照 2231 件は WHATWG の HTML 標準の一覧（CC BY 4.0、source に取り込んだ部分は BSD 3-Clause）、日本語の encoding の表は WHATWG Encoding 標準の index、Unicode の性質は UCD（Unicode License v3）） | **決定（2026-09-28 ユーザー「文字の表は、生成した表をコミットしていいてす。」）**: 生成の script（`tools/`）と生成した `.c` を commit し、出典とライセンスの表示を file の先頭と `userland/base/licenses/browser/` の notice に置く。`tools/regenerate.sh` が固定の SHA-256 で一覧を取得して再生成する | build のたびに取得して生成（base の build が network に依存する） | どれも許容的なライセンスで表示を保てば再配布できる。base の build を offline に保つ |
| D5 | GIF の decoder の置き場 | browser の中（`image/gif.c`） | `libgif-compat` を base の library に | 今 GIF を要る base の program は browser だけ。**2026-09-28 ユーザー決定: `userland/base/libgif-compat`（共有）、libjpeg-compat も base の共有 library。header は `include/libc/compat/`（同日の訂正、GIF も同じ所）** |
| D6 | 窓の提示と描画 | **決定（2026-09-27 ユーザー「ブラウザはWaylandとVulkanで実装してください。」）**: Wayland の上の Vulkan。display list を GPU で描く。CPU の参照の描画は headless と試験だけ（§8.2） | （既定だった wl_shm は取り消し） | — |
| D7 | libpng-compat の範囲 | ws071-p010 の simplified API に `png_image_begin_read_from_memory` を含めてもらう（browser は memory から読む） | browser が一時 file に書いて `from_file` | 仕様の simplified API の一部で、実装はほぼ同じ |
| D8 | libtruetype の拡張 | 既存の関数を変えずに関数を足す（小数の大きさ、1/64 px の advance、kerning、`name`・`OS/2`、`unitsPerEm`）。足す前に main に伝える | browser の中に別の TrueType の読み手を持つ | TrueType の parser を base に 2 つ持たない |
| D9 | libjpeg-compat の API | IJG の古典的な API の decompress の部分集合、`JPEG_LIB_VERSION` 62、header は自前、encode は後 | TurboJPEG の API（`tj3*`）、version 80 | 「libjpeg-compat」の名前から古典的な API が自然。62 は libjpeg-turbo と Debian の既定 |
| D10 | 既定の font | `serif`・`sans-serif`・`monospace` を image の font（Inter・JetBrains Mono・Droid Sans Fallback）に対応させ、serif は serif の font が image に入るまで sans に | DejaVu・Noto 等を image の script で足す（font は commit しない） | 今の image にある font で始められる。serif・CJK の font の追加はユーザーが選ぶ |
| D11 | 段階の目標値 | §15 の数 | — | 最初の数はユーザーの見直しを受ける |

## 18. Phase の分割

**2026-09-27 ユーザー「ブラウザのサブエージェントにも、正常系でワンパス通すのを優先するように伝えてください。」**: まず正常系を端から端まで
（HTML → DOM → style → layout → 描画 → 窓に実際の page が出る → JS の接続）通し、準拠の率と端のケースは後の Phase に回す。各 Phase は
最小の範囲で通し、残りを phase.md の「後回し」に書く。段階の目標値（§15）はワンパスの後に上げる。実行の順は ws.md の「実行の順」。

[ws.md](ws.md) の Phase の表が正本。各 Phase は 1〜3 時間で終わる大きさを目標にし、着手の時に大きすぎれば分ける。依存の順:
基盤（p002〜p003）→ HTML（p004〜p006）→ CSS（p007〜p009）→ text・layout・描画・窓（p010〜p014、M1）→ network（p015〜p017）→
encoding（p018）→ 画像（p019〜p021）→ 共通の engine（p022〜p023）→ JS（p024〜p029）→ binding（p030〜p032）→ Wasm（p033〜p034、M2）→
CSS の互換（p035〜p039）→ Chrome との比較（p040、M3）→ shell の仕上げ（p041）→ M4（p042〜p043）→ 規約の照合（p044）。

## 19. 部品としての browser（libbrowser.so、2026-09-28 ユーザーの方針）

ユーザー:「ブラウザですが、システム環境設定などのウィンドウとか、ウィジェットとかで、HTML5コンポーネントを使えたらいいなと思っているので、
libbrowser.soというファイルに将来的に分割できるようにして、コンポーネントにしましょう。ブラウザの構造体へのポインタをハンドルにして、
Vulkanレンダリングターゲットを指定してイベントも送ってやるとと描画してくれて、Wayland依存はない、みたいなのがいいと思います！
これはいきなり実現しなくても、徐々にそういう設計に変えていきましょう。」

目標の形（徐々に移す。一度に作り直さない）:

- **engine と shell の分離**: HTML・CSS・layout・JS・DOM・net・image・paint（display list と Vulkan の描画）は engine、
  窓・Wayland・titlebar・location bar・入力の変換は shell（今の `shell/`・`main.c`）。engine は Wayland の header を include しない。
- **handle**: engine の実体の構造体へのポインタ（例 `struct browser_view *`、不透明な型）。作成・破棄・読み込み（URL・HTML の文字列）・
  大きさの変更・描画・入力の event（pointer・key・scroll・focus）・callback（題名・URL・読み込みの状態・再描画の要求・link の navigation の
  決定）を C の API にする。
- **描画の先**: 呼び出し側が Vulkan の device・queue と描画の先（VkImage と layout・大きさ、または command buffer）を渡し、engine は
  そこに描く。CPU の reference の描画（試験用）も同じ API から選べる。
- **library**: 将来 `libbrowser.so`（`userland/desktop/` の library。header は `include/libc/` の適所）に分け、`/bin/browser` はその
  上の shell になる。システム環境設定の窓や widget が同じ library を使う。
- 移し方: 新しいコードは engine と shell の境界を守って書く。境界を越える既存の依存は、触る Phase で少しずつ直す。
  分割（.so と公開の header、API の文書、2 つ目の使い手の試作）は専用の Phase で行う。
- 実施（2026-09-28、p054〜p057）: view の型（p054）、呼ぶ側の Vulkan の描画の先（p055）、DOM の形の入力と既定の動作（p056）の後、
  p057 で engine を `libbrowser.so` に分けた。公開の header は `include/libc/browser.h`（sysroot の `<browser.h>`、API の文書を兼ねる。
  `BROWSER_API_VERSION` と options の version）。package は `userland/desktop/libbrowser/`（Makefile と exports.map。source は
  `userland/desktop/browser/` の module の directory に残し、試験の path を変えない）。`/bin/browser` は `main.c` と `shell/` だけで、
  `<browser.h>` だけを使う（`--js`・`--dump=ast`・`--dump=code` は library の `browser_script_tool`）。2 つ目の使い手は
  `userland/base/tests/browser-probe`（`<browser.h>` と libc だけ、Wayland なしで page を CPU か engine の offscreen の GPU で PPM に描く）。


## 20. source 所有の改訂（2026-10-01 ユーザーレビュー）

§2 と §19 の engine source を browser に残す配置方針は [WS107](../ws107/ws.md) の新設により置換。
engine の全 module/private header/table/shader/生成器は desktop/libbrowser、main/shell/app data は desktop/browser。
public header の現所在は desktop/keiland/browser.h（WS104 の境界整理後）。
実 source はまだ移動前。移動する scope と reference の台帳は WS107 p001、source 移動は承認後 p002。
Wayland は library で利用不可、標準 Vulkan は利用可、shell が events を抽象 public API へ変換。
[全文の追加規則](../standards/browser-component.md)を今後の WS074 の設計/生成/検証にも適用する。
以前の p054〜p057 の結果は当時の成果として保存。WS074 の機能目標と p100→p101 は変更しない。
2026-10-01 / review-20261001-policy-ws074: 配置の設計改訂、理由と新規則/WS107 の引渡しを記録。未実装。
