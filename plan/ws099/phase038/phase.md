# ws099-p038: 最大化（dock）の時に compositor が中身の rect を四方に 4 px ずつ空ける

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-264 の PNG（8 px）、T1-277 で p134・p076・p132・p137 PASS）（旧: in-progress（q815、P1。実装と host の確認まで済み、T1 の撮影待ち））
WS: [WS099](../ws.md)
Related: [ws090-p021](../../ws090/phase021/phase.md)（app の padding を 0）

## 出典

2026-10-06 ユーザー:「最大化したとき、コンポジタがウィンドウのコンテントRectを四方向で4pxずつパディングするようにしてほしいです。バランスを取りたいのでスクショを見せてください。あと、Pixel単位で話していますが、あとでDPIスケーリングを導入したいので、デフォルトDPIでの論理Pixelだと理解してください。」

## 範囲

- DOCKED（最大化）の時、compositor が窓の中身の rect を上下左右に 4 論理 px ずつ内側に置く（app に渡す大きさもその分小さく）。app の padding は 0 のまま（ws090-p021）。
- 値は compositor の定数（論理 px、既定の DPI。後で DPI scaling を入れる時に倍率を掛ける所を 1 か所に）。
- 全画面（fullscreen・game mode）は対象外（端まで）。
- 試験: host か QEMU で最大化した窓（Files・Settings・Text Editor・Terminal）の撮影をユーザーに見せる（`build/review/` に複写して相対 path で）。AAT・guest の試験の窓の大きさの期待（1256x680 など）が変わる物を直す。
- WS131 p021（compositor の一括の改名）の merge の後に着手（衝突を避ける）。

## q815（P1、2026-10-06）

### 変えた所

- `kwl.h`: `KWL_GLASS_DOCK_PAD 4`（論理 px、既定の DPI。DPI の倍率を掛ける所はここ 1 か所）、`KWL_GLASS_DOCK_TOP` = bar + `KWL_GLASS_DOCK_PAD`（上の 4 px は前から bar の下にあった値を定数にした）。
- `shell.c` の `docked_rect`: x = 4、y = 48、幅 = 画面 − 右の keyboard − 8、高さ = 画面 − 48 − 下の keyboard − 4。docked の大きさ（configure で app に渡す大きさ、`kwl_glass_unfullscreen_docks`・`open-docked`・dock の animation・pull・press の判定・大きさが固定の窓の中央）は全てここから。1280x800 では 4,48 1272x748（前は 0,48 1280x752）。
- `keyboard.c`: 画面の keyboard が開いた時の docked の大きさも同じ余白（定数で）。
- `draw_body` の `docked`（下の角を画面の外に出して四角に見せる）を除いた。四方に余白があるので、docked の窓も角が丸い（Terminal のように角を四角に保つ窓は `window_square` のまま四角）。
- fullscreen（game mode を含む）は変えない（`docked_rect` を使わない）。

### 試験の期待の直し

`ws035/tests/zdesktop-p059.sh`・`p062.sh`（1272x748）、`p134.sh`（docked の body 4,48 1272x748、Terminal の角は四角のまま）、`ws090/tests/sheet-guest.sh`（中央の y = 48+(748−高さ)/2、x は 260 のまま）、`ws099/tests/bug194-guest.sh`（1272x748、x=4）、`ws142/tests/p010-guest.sh`（x=4）、`ws102/tests/osk-guest.sh`（1272x412・1272x748・954x748）。

### 確認

- build: zedBSD amd64 の wayland（exit 0、warning 0）、`make keiland-linux` gcc（0、warning 0）。host: ws142 host-layout ok (57)、run-host-role PASS。
- 未実施: 撮影（T1）: 最大化した Files・Settings・Text Editor・Terminal（1280x800、glass）をユーザーに（`build/review/`）。上の guest の試験。

## 2026-10-06 ユーザーの変更

撮影（4 px）を見て:「4pxだったところを8pxのパディングに変更して、スクショを1枚でいいのでください。Filesのスクショがいいです。」→ `KWL_GLASS_DOCK_PAD` を 8（論理 px）に。1280x800 で最大化の領域は x=8・y=52・1264x740 の見込み。Files の最大化の撮影 1 枚をユーザーへ。

## q819（P1、2026-10-06）: 4 px → 8 px

ユーザー（原文）:「4pxだったところを8pxのパディングに変更して、スクショを1枚でいいのでください。Filesのスクショがいいです。」
- `KWL_GLASS_DOCK_PAD` を 8 に。1280x800 で docked の body は 8,52 1264x740（画面の keyboard の QWERTY の時 1264x404、flick の時 946x740）。
- 試験の期待を新しい値に: zdesktop-p059・p062・p134、sheet-guest（y = 52+(740−高さ)/2、x は 260 のまま）、bug194-guest、ws142 p010-guest、osk-guest。
- 確認: zedBSD の wayland の build（warning 0）、keiland-linux の gcc、ws142 host-layout。T1 に Files の最大化の撮影 1 枚。

## q822（P1、2026-10-06）: T1-264 の FAIL（試験の側）

- zdesktop-p134: P1 の誤り。p021 で comment の `zwl_` を `kwl_` にした sed が shell の補助関数の呼び出し `zwl_app_clients` まで `kwl_app_clients` にしていた（`not found` → `zc1` が無く、窓の場所が 0,0 になり、角・dock・undock・probe の行が全て崩れた）。`zwl_app_clients` に戻した（他の script に同じ壊れは無い、grep）。
- zdesktop-p062: docked.png の点 1260,50 が 8 px の余白（body は y=52 から）に入った。1260,60 に。
