<!-- awesome-plan project=zedbsd record=ws035p059 -->

# ws035-p059: look and feel の疎通確認（浮いたタイトルバー、すりガラス、上部のバー）

Phase ID: `ws035-p059`
Parent: [WS035](../ws.md)
Status: cleared（q460-i01、2026-09-26）
Phase disposition: normal
Queue: q460-i01
承認: 2026-09-26 ユーザー「すりガラス風のシェーダーは作りましょう。ハリボテでもいいので、一回このlook and feelが実現できることを、疎通確認的に実装してみませんか？」「スクリーン上部のバーはハリボテでいいです。ウィンドウタイトルバーは実装しましょう。画像を参考にしてください。続けてください。フォントはあとで置き換えるので、Google Fontsからアルファベットを持ってきましょう。コミットしなければOKです。」（参考画像: 窓の本体から離れて浮いたタイトルバー、すりガラスの面、淡い青の壁紙、上部の全幅のバー）

## 目的

ユーザーが示した look and feel（窓の本体と分離して浮いたタイトルバー、すりガラスの面、上部のシステムバー）を zdesktop（zwl）で実際に描けることを、疎通確認として示す。設計 D7（server-side decoration）の見た目の方向を、浮いたタイトルバーに変える試作でもある。

上部のバーの配置（ユーザー 2026-09-26）: 左上はアプリケーションの起動・システムメニュー（Mac のアプリのメニューバーではない）、右上は通知領域。

## 範囲

- `zwl --glass`（既定の見た目と p052〜p054 の試験は変えない）。
- 壁紙: CPU で一度だけ描く淡い青の風景（空の gradient、重なる山、湖）。
- すりガラス: 壁紙を縮小してぼかした画像を一度だけ作り、面はその画面の位置を sampling して白で色を足し、縁に明るい線を引く（Windows の Mica と同じく壁紙だけを透かす。窓の上に重なった面の背後の窓は透かさない。背後の実際のぼかしは p057）。角丸・影は shader の距離関数。
- 窓のタイトルバー（実装）: 本体の上に隙間を空けて浮く、角丸のすりガラスの面。題名（`xdg_toplevel.set_title`）、左にアプリの印、右に最小化・最大化・閉じるの button（hover で背景）。本体は角丸と影。題名の帯を掴んで移動、窓を押すと最前面、閉じるは `xdg_toplevel.close`、最大化は作業領域の大きさで configure（戻すと元の位置と大きさ）。最小化は見た目だけ。
- 上部のバー（ハリボテ）: 全幅のすりガラスの帯。左に起動の印と「zedBSD」、右に通知領域（電波・電池の印、日付と時刻）。操作は無い。
- 文字: Google Fonts の Inter（OFL 1.1）の ASCII を libtruetype で glyph の atlas にする。font は `build/ws035-fonts/`（git に入れない。後で置き換える）、image へは試験の追加 file として `/usr/share/fonts/zdesktop.ttf` に置く。

範囲外: 背後の窓のぼかし（p057）、damage（p055）、最小化の実体・タスクバー・起動の menu（p011・p013）、日本語の文字（font は後で置き換える）、実機。

## 受け入れ

1. Venus の guest で `zwl --glass` の画面を撮り、参考画像の要素（浮いたタイトルバー、すりガラス、角丸と影、上部のバー、壁紙）が見えること（画面をユーザーに見せる）。
2. 題名の帯の drag で窓が動き、閉じるで client が閉じ、最大化で作業領域の大きさになり戻せる（QMP の pointer で操作し、画面と log で確かめる）。
3. `--glass` 無しの p052・p053・p054 の試験が通る。build は warning 0、新しい C は style-check の指摘 0、boot test。

## 結果（q460-i01、2026-09-26、QEMU の Venus guest（host の Lavapipe）。実機は未実施）

実装: `userland/base/zwl/glass.c`（新規）、`shaders/panel.vert`・`panel.frag`（すりガラス・影・角丸の画像・単色・輪郭・文字を 1 つの shader の mode で描く。押し込み定数 96 byte）、`compose.c`（panel の pipeline・layout、線形の sampler、`--glass` のときの描画）、`shm.c`（host の画像を sampler 付きで作る関数を公開）、`protocol.c`（`set_title` を保持、最大化の configure に state `maximized`）、`display.c`（`--glass` の配置、時計の分の更新）、`seat.c`（title bar と desktop の button、移動中の motion を zdesktop が取る）、`objects.c`（移動中の窓の破棄）、`main.c`（`--glass`、`--font=`）。zwl は libtruetype を link する。

試験: [zdesktop-p059.sh](../tests/zdesktop-p059.sh)（Venus の guest、QMP の pointer）。

| 受け入れ | 結果 |
| --- | --- |
| 1. look and feel が見える | 浮いたタイトルバー（本体の上 8 px の隙間、角丸 14、すりガラス、題名・印・3 つの button）、角丸と影の本体、上部のバー（起動の印・「zedBSD」・電波・電池・日付と時刻）、壁紙が描けた（`build/ws035-p059/desktop.png`・`hover.png`・`maximized.png`、ユーザーに提示） |
| 2. 操作 | 題名の帯の drag で窓が (-300, -100) 動いた（log と画面）。閉じるで `xdg_toplevel.close` が届き wlshm が終了。最大化で configure 1256x690（state maximized）、wltest が resize して作業領域を埋め、もう一度で 420x300・元の位置に戻る。hover で button の背景（閉じるは赤） |
| 3. 回帰・規約 | `--glass` 無しの p052・p053・p054 PASS。amd64 の build は warning 0。style-check は `glass.c` と変えた行で 0。boot test PASS（`build/ws035-p059-boot/login.png`） |

制限（後の Phase へ）:
- すりガラスが透かすのは壁紙だけ（Mica と同じ）。題名の帯の後ろの別の窓はぼかさない → p057。
- 最小化は見た目だけ（taskbar が無い）→ p011・p013。上部のバーは操作の無いハリボテ → p013。
- font は Google Fonts の Inter（OFL 1.1）を `build/ws035-fonts/` に置き、image へ試験の追加 file（`/usr/share/fonts/zdesktop.ttf`、`zdesktop-OFL.txt`）で入れた。git には入れていない（ユーザー指示、後で置き換える）。font が無ければ文字無しで描く。日本語は無い。
- 窓の body の左上の角の外（隙間）を押しても下の窓には届かず、desktop の扱い（何もしない）。focus を得た client への `pointer.enter` の座標は title bar 上なら負になる（wlshm の log で y=-30）。
- 時計は guest の時間帯（UTC）。
- Lavapipe の上なので速さは測っていない。
