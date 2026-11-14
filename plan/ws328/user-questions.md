<!-- awesome-plan project=zedbsd record=ws328-user-questions -->

# WS328 のユーザーへの問い（p001、2026-11-14 P4）

WS328（libbrowser のベクトル描画: path の塗りと線、CSS の角丸・gradient・影、それを使った SVG の描画）の設計（[design.md](design.md)）で、製品の判断が要る点。
設計は各問いの推しの案で書き切ってある。答えが推しと違えば、該当の Phase の前に設計を直す。

## U1. path の「覆い」の計算を CPU で一度だけ行い、GPU が合成する形でよいか

- 背景: 2026-10-30 の決定で WS328 は Vulkan で描く。図形の描画は「どの pixel をどれだけ覆うか（覆い、coverage）」と「その pixel の色（塗り・gradient・合成）」に分かれる。
- 推し（A）: 角丸の箱・border・影・gradient は、覆いも色も GPU の shader が式で出す（毎 frame CPU は画素を触らない）。SVG の図形や線のような一般の path は、覆いを CPU で内容が変わった時に一度だけ作って GPU の texture に置き（文字の glyph と同じ形）、色と合成は GPU が毎 frame 行う。scroll では作り直さない。
- 案 B: 一般の path の覆いも GPU で毎 frame 計算する（辺の data を GPU に渡し shader で数える）。
- 案 C: GPU の stencil（画素ごとの数え）を使う。図形を三角形の扇にして stencil に「何回覆ったか」を書き、その後で図形の箱を塗る時に stencil が 0 でない所だけ色を付ける（stencil-then-cover）。縁のなめらかさは MSAA（画素を複数の点で標本にする機能）で出す。shader の loop も buffer も要らず、Vulkan 1.0 の範囲で書ける。
- 推しの理由: zedBSD の自前の GPU driver（Intel の i915、Raspberry Pi の V3D、VisionFive 2 の PowerVR、Amaterrace の GHCI）は shader の機能がまちまちで、今の browser の shader もそれに合わせて機能を絞っている。B はどの driver でも動く保証が無い。C は shader の機能は少なくて済むが、今の browser の描画の手順に stencil が無く、各 driver が stencil と MSAA をどこまで正しく扱えるかを確かめていない（確かめと描画の手順の作り直しが要る）。A は CPU の参照の描画と GPU の結果が一致し、「frame ごとの経路で CPU が画素を触らない」という方針（2026-10-12）にも普段は合う。後で driver が揃えば、item の形を変えずに B か C に替えられる。
- A の例外: SVG の animation（JS や CSS で形が毎 frame 変わる物）や、拡大・縮小の animation では、形が変わるたびに CPU が覆いを作り直す。frame ごとの CPU の仕事に上限（予算）を設け、超えた分は次の frame に回す（その間は少し古い形か何も描かない）。

## U2. SVG の対象の範囲

- 推し: WS328 で描くのは、図形（rect・circle・ellipse・line・polyline・polygon・path）、塗りと線（dash・join・cap を含む）、transform、viewBox、g・use・symbol・defs、linearGradient・radialGradient、clipPath、opacity。text・image・marker・mask・pattern・filter は描かない（後の別の WS か Phase）。
- 案: text（SVG の中の文字）も WS328 に入れる。
- 推しの理由: アイコンやロゴの多くは図形と gradient で足りる。SVG の text は文字の配置（textPath など）の仕様が大きく、ベクトル描画と別の主題。

## U3. 半透明の group を GPU の重ね（layer）で正しく描くのは WS328 の外でよいか

- 背景: `opacity: 0.5` の箱や SVG の `<g opacity=0.5>` は、本来は中身を別の画像に描いてから半透明で重ねる。今の browser は中の部品それぞれの透明度に掛けて近似している（部品が重なる所の色が少し違う）。CSS の filter も GPU では描いていない。
- SVG では差が目立ちやすい: 塗りと線の両方を持つ図形に `opacity` を付けると（アイコンでよくある）、線の内側の半分が塗りと重なって濃く見える。
- 推し: WS328 では今の近似のまま（CPU で画像にする SVG だけは正しく描く）。GPU の layer は WS074 か新しい WS で、filter と一緒に作る。
- 案: WS328 に GPU の layer を入れる（Vulkan の描画の作りが大きく変わる）。

## U4. SVG に style sheet（`<style>` や頁の CSS の class の指定）を効かせるのは後でよいか

- 背景: SVG の色や線は、属性（`fill="red"`）、style 属性（`style="fill:red"`）、style sheet（`.icon path { fill: red }`）のどれでも指定できる。今の libbrowser の CSS の engine は SVG の性質（fill・stroke など）を知らない。
- 推し: WS328 では属性と style 属性だけを読む。style sheet の cascade を SVG に効かせるのは、CSS の engine に SVG の性質を足す別の Phase（WS074 の再開の時か新しい WS）。
- 案: WS328 に入れる（CSS の engine の変更が要り、範囲が広がる）。
- 補足: Web の inline の SVG のアイコンは class で色を付ける物が少なくないので、後回しにすると一部のサイトのアイコンの色が黒になる。一方、`fill="currentColor"` のように属性で文字の色を使うアイコン（とても多い）は、頁の CSS の `color` を svg に継承させる口を設計に足したので（レビューの反映）、style sheet の cascade が無くても正しい色で描かれる。

## U5. libbrowser の build の一覧（`sources.mk`）に新しい file を足してよいか

- 背景: `userland/desktop/libbrowser/sources.mk` は make の一覧で、Makefile と同じく Claude は変えずに差分の案をユーザーに見せる決まり。新しいベクトル描画と SVG の file（vector/ の 7 個、svg/ の 9 個）を libbrowser の build に入れるには、この一覧に足す必要がある。
- 案: [design.md](design.md) §13 の案 1（16 行を足す。paint の統合の Phase p018 の始め）、案 2（古い gradient の CPU の描画の file を消す。p025）、案 3（SVG を頁の木に繋ぐ file を足す。p021）。各 Phase の始めに、Q1 がこの差分を見せて承認を得る。
- 推し: 案のとおり、各 Phase の始めに当てる。足すだけでは今の動きは変わらない（新しい file はまだどこからも呼ばれない）。

## Q1 との確かめ（済み）

- WS074 の host の試験の期待の値で WS328 の統合が変える物を WS328 の Phase が直す事: 2026-11-14 Q1 が許した（ws.md の Resume point）。
- 仕様の資料の置き場所: main の checkout の build/ に写さず、各担当の worktree の build/ に置く（2026-11-14 Q1）。
