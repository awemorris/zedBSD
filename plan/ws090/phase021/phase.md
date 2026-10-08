# ws090-p021: 全ての app の窓の中身の padding を 0 に（title bar と同じ幅、title bar との間は compositor の定数）

Status: test-wait（2026-10-08 q902 P1 の照合: T1-256 の (a)〜(c) PASS。(d)(e) の 13 app の PNG はユーザーが見る、未）（旧: planned（2026-10-06 Q1 が作成））
WS: [WS090](../ws.md)
Related: [WS099](../../ws099/ws.md)（compositor の title bar）・[BUG-218](../../bugs/BUG-218.md)（Phone の padding）

## 出典

2026-10-06 ユーザー（Mahora の外観の画面を見て）:「Text Editorのウィンドウの中身が、ウィンドウタイトルバーより幅が小さいので、パディングを入れているように見えます。他のアプリもすべてそうですが、パディング0にして、ウィンドウタイトルバーの幅と、ウィンドウの中身の幅は、同一でお願いします。同様に、ウィンドウタイトルバーとウィンドウコンテントの間のスペースの高さは、すべてのアプリで同一にしたいので、パディングを入れているアプリは修正して0にしてください。アプリ側で0にすれば、コンポジタが定数でその高さを入れることになりますよね。アプリすべてチェックしてください。」

## 範囲

- 全ての app（Text Editor・Terminal・Files・Settings・PDF Viewer・Image Viewer・Notes・Phone・Mailer・Calendar・System Monitor・Video Player・Browser・Music など、libkeiland の glass・panel を使う物と自前で描く物の全て）の窓の中身の左右・上の padding（margin・inset・glass の panel の外側の余白）を 0 にし、中身の幅を title bar と同じにする。
- title bar と中身の間の高さは、app 側を 0 にして compositor の定数だけにする（ユーザーの理解「アプリ側で0にすれば、コンポジタが定数でその高さを入れる」が今の compositor で正しいかを最初に確かめ、違えば compositor の側を定数にする）。全ての app で同じ高さになること。
- libkeiland の共通の部品（glass・panel・cards・list の外側の余白）にある既定の padding も 0 にして、app ごとに残らないようにする。
- 試験: host の描画、QEMU は T1 で全ての app の窓の撮影（title bar と中身の左右の端が揃う、間の高さが同じ）をユーザーが見る。

## q813（P1、2026-10-06）

Status: test-wait（T1 依頼中）。

### compositor の確かめ（最初の報告）

- `shell.c` の `floating_title()` が title bar を body の上に置く: x・幅は body と同じ、y は `body->y - ZWL_GLASS_GAP(8) - ZWL_GLASS_TITLE(44)`。title bar と中身の間は compositor の定数 8 px だけで、app が上の余白を 0 にすれば全ての app で同じ高さになる（ユーザーの理解のとおり。compositor は変えない）。

### 変えた所（app の外側の余白を 0 に）

| app | 前 | 後 |
| --- | --- | --- |
| Text Editor | `TE_CARD_INSET` 8（glass でも不透明でも、card が窓の端から 8 内側） | 0（`te_app_card` は窓の全体） |
| Image Viewer | `IV_CARD_INSET` 8（glass の card） | 0 |
| Mailer | glass で `ML_VIEW_MARGIN` 12 | 定数を除き margin 0（pane の間の gap 10 は残す） |
| Files | glass でない時 `UI_MARGIN` 12、dock した時 `UI_GLASS_GAP` 8 | どの時も 0（`docked` の field と main.c の代入を除いた。`UI_MARGIN` は tasks の位置にだけ残す） |
| Settings | 同上 | 同上（`UI_MARGIN`・`docked` を除いた） |
| Calendar | glass でない時 `CAL_MARGIN` 12 | 0（定数を除いた） |

- 変えなかった物: Phone（`PH_VIEW_MARGIN` 0、q790 で済み）。Terminal・PDF Viewer・System Monitor・Video Player・Browser は glass を使わず、不透明な地が窓の全体を塗るので、中身の幅は title bar と同じ（`TERMINAL_PADDING`・`PV_MARGIN` などは地の中の文字・頁の余白）。Notes は窓の全体が頁で、上の toolbar の card（`UI_CARD_PADDING` は card の中の余白）は頁の上に浮かぶ物なので変えない（P2 の WS175 と重ならない）。
- libkeiland: 外側の余白の既定は無い（`cards.c` の `CARDS_CARD_PAD`・`CARDS_DIALOG_PAD` は card の中の余白、`list.c` の `LIST_BAR_MARGIN` は scroll bar の分の行の右の余白、どれも app が渡した矩形の内側）。変えない。
- dock した窓（title が system bar に入る）も余白 0 になる（Files・Settings は以前 8 を保っていた。Calendar・Phone・Text Editor などと揃えた）。
- 試験の期待: `plan/ws090/tests/sheet-guest.sh` の Text Editor の card を `card:8,8,` → `card:0,0,` に。

### 確認

- build: zedBSD amd64 の textedit・imageview・files・settings・calendar・mailer（exit 0、warning 0）。
- host: `plan/tools/files/host-build.sh` の files-render で glass の窓を描き、sidebar と中身の card が窓の左・上・右の端に付くことを見た（`build/ws090-p021/files-glass.png`）。
- 未実施: QEMU（T1）での全 app の窓の撮影。
