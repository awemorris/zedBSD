<!-- awesome-plan project=zedbsd record=ws328-user-questions -->

# WS328 のユーザーへの問い（p001、2026-11-14 P4）

WS328（libbrowser のベクトル描画: path の塗りと線、CSS の角丸・gradient・影、それを使った SVG の描画）の設計（[design.md](design.md)）で、製品の判断が要る点。
設計は各問いの推しの案で書き切ってある。答えが推しと違えば、該当の Phase の前に設計を直す。

## U1. path の「覆い」の計算を CPU で一度だけ行い、GPU が合成する形でよいか

- 背景: 2026-10-30 の決定で WS328 は Vulkan で描く。図形の描画は「どの pixel をどれだけ覆うか（覆い、coverage）」と「その pixel の色（塗り・gradient・合成）」に分かれる。
- 推し（A）: 角丸の箱・border・影・gradient は、覆いも色も GPU の shader が式で出す（毎 frame CPU は画素を触らない）。SVG の図形や線のような一般の path は、覆いを CPU で内容が変わった時に一度だけ作って GPU の texture に置き（文字の glyph と同じ形）、色と合成は GPU が毎 frame 行う。scroll では作り直さない。
- 案 B: 一般の path の覆いも GPU で毎 frame 計算する（辺の data を GPU に渡し shader で数える）。
- 推しの理由: zedBSD の自前の GPU driver（Intel の i915、Raspberry Pi の V3D、VisionFive 2 の PowerVR、Amaterrace の GHCI）は shader の機能がまちまちで、今の browser の shader もそれに合わせて機能を絞っている。B はどの driver でも動く保証が無い。A は CPU の参照の描画と GPU の結果が一致し、「frame ごとの経路で CPU が画素を触らない」という方針（2026-10-12）にも合う。後で driver が揃えば、item の形を変えずに B に替えられる。

## U2. SVG の対象の範囲

- 推し: WS328 で描くのは、図形（rect・circle・ellipse・line・polyline・polygon・path）、塗りと線（dash・join・cap を含む）、transform、viewBox、g・use・symbol・defs、linearGradient・radialGradient、clipPath、opacity。text・image・marker・mask・pattern・filter は描かない（後の別の WS か Phase）。
- 案: text（SVG の中の文字）も WS328 に入れる。
- 推しの理由: アイコンやロゴの多くは図形と gradient で足りる。SVG の text は文字の配置（textPath など）の仕様が大きく、ベクトル描画と別の主題。

## U3. 半透明の group を GPU の重ね（layer）で正しく描くのは WS328 の外でよいか

- 背景: `opacity: 0.5` の箱や SVG の `<g opacity=0.5>` は、本来は中身を別の画像に描いてから半透明で重ねる。今の browser は中の部品それぞれの透明度に掛けて近似している（部品が重なる所の色が少し違う）。CSS の filter も GPU では描いていない。
- 推し: WS328 では今の近似のまま（CPU で画像にする SVG だけは正しく描く）。GPU の layer は WS074 か新しい WS で、filter と一緒に作る。
- 案: WS328 に GPU の layer を入れる（Vulkan の描画の作りが大きく変わる）。

## U4. SVG に style sheet（`<style>` や頁の CSS の class の指定）を効かせるのは後でよいか

- 背景: SVG の色や線は、属性（`fill="red"`）、style 属性（`style="fill:red"`）、style sheet（`.icon path { fill: red }`）のどれでも指定できる。今の libbrowser の CSS の engine は SVG の性質（fill・stroke など）を知らない。
- 推し: WS328 では属性と style 属性だけを読む。style sheet の cascade を SVG に効かせるのは、CSS の engine に SVG の性質を足す別の Phase（WS074 の再開の時か新しい WS）。
- 案: WS328 に入れる（CSS の engine の変更が要り、範囲が広がる）。
- 補足: Web の inline の SVG のアイコンは class で色を付ける物が少なくないので、後回しにすると一部のサイトのアイコンの色が黒になる。

## Q1 への確かめ（製品の判断でない物）

- WS074 の host の試験（`plan/ws074/tests/`）の期待の値で WS328 の統合が変える物を、WS328 の Phase が直してよいか（WS345 と同じ扱い。design.md §12）。
- `userland/desktop/libbrowser/sources.mk` に新しい .c を足す差分（design.md §13）の承認。
- 仕様の資料を main の checkout の `build/sources/specs/ws328/` に写す事（design.md §16）。
