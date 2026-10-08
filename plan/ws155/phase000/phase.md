<!-- awesome-plan project=zedbsd record=ws155-p000 -->

# ws155-p000: Calendar の UI の mock

Status: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: mock は T1-179・T1-179c で撮影、ユーザーの再指示は p001 の設計に反映、本物の app は p002〜p004 で cleared（T1-297）。cleared か置き換えの canceled かは Q1）（旧: in-progress（実装・host の PNG と animation の GIF・build は済み。QEMU は T1 待ち。ユーザーが mock を見て再指示する））
Disposition: normal
Parent: [WS155](../ws.md)
Queue: q745（2026-10-05、P2）
依存: なし

## 範囲（Q1、ユーザーのデザイン案と方針）

ws.md の「デザイン案」の要素（このままでなく要素として）と Q1 の方針で、月の表示を中心に sidebar・Add Event の panel・3D の日めくりの animation。data は固定の試験 data、保存は作らない。Windows 11 のように実用的でミニマルな中に知性、格好だけの効果は入れない。動きは少しだけ、落ち着いた 3D。

## 実装（2026-10-05、P2）

- `userland/desktop/calendar/`（新規、package `calendar`、既定では image に入らない `n`）:
  - `render3d.h/.c`: app の中の小さな software の 3D。三角形を 1/z の depth と重心座標で塗る（透視補正の texture、bilinear）、光は 1 つ（環境光＋拡散＋弱い highlight）、2 倍で描いて平均で縮める（輪郭の smoothing）。
  - `scene.h/.c`: box・quad・押し出し・楕円体・輪の mesh。日めくり（頁・下の頁の束・板・脚・綴じの bar と青い 2 つの輪、頁は上の綴じ目の線で回る）、周りを流れる半透明のリボン、種類の icon（Work の鞄・Personal のハート・Study の本・Family の 2 人）。
  - `date.c`（月の長さ・曜日・月と日の移動）、`data.c`（Work 青・Personal 赤・Family 緑・Study 黄の 4 つの calendar と、今日の月からの相対の日付の試験の予定）。
  - `view.c`: 3 つの card（不透明では淡い青の地に白い card と柔らかい影、glass では desktop が間に見える card）。左の sidebar（mark と「Calendar」、Month View・Today・Search・Settings、My Calendars の色の checkbox で表示・非表示、Add Calendar、下に「A more organized you」の card）。中央の上の帯（‹ ›・Today・Month / Week / Day の segmented・検索・…）、曜日の行（日曜は赤・土曜は青）、月を縦に続けた scroll（前後 6 か月、月の見出し、日曜の列は淡い赤・土曜は淡い青、前後の月の日は灰、今日は accent の地と丸、選んだ日は accent の輪、予定は calendar の色の pill、入り切らない分は「+N more」、検索に合わない予定は薄く）。右の panel（「Add Event」と説明、種類の card 2×2 に 3D の icon、Custom、下に 3D の日めくりと「Small plans make big days.」）。
  - 動き: 日めくりがゆっくり呼吸する（7 s と 9 s の周期で小さく傾く、リボンが 14 s で流れる。約 20 fps）。日付を選ぶ・‹ ›・Today・キーで日が変わると頁が上へめくれて薄れて消え（560 ms）、下の頁が新しい日。種類の card を日付へ drag して落とすと、その日に予定が足され（動いている間だけ）cell が 300 ms 沈む。「動きを減らす」（View の Reduce Motion、`--reduce-motion`）では呼吸を止め、頁と cell の変化はその場で。
  - キー: 矢印で日・週、Page Up/Down で月、T で今日。Week・Day・Settings・Add Calendar・Custom・… は「not in the mock yet」の chip。
  - `main.c`: kl_app の loop（Phone と同じ形、glass は Phone と同じに first frame で判定し panel を送る）。今日は system の local の日。
- `platform/amd64/vmunix.mk`: `bin/calendar` の link の規則。`userland/desktop/wayland/apps.conf`: App Home に「Calendar」。
- 試験の image の config: `plan/ws155/tests/config-amd64-calendar.mk`（CI の image ＋ calendar）。

## 確かめ

- host: `sh plan/ws155/tests/run-host-calendar.sh` → PASS 7（start・select（FLIP 10-05→10-14）・drop（DROP Work 10-22）・next（11-22）・hide（Work を隠す）・motion・reduced）。今日は 2026-10-05 に固定。
- PNG（`build/ws155/`）: `host-calendar-start.png`、呼吸の `breath-0..3`（1.75 s 毎）、頁めくりの `flip-0..5`（110 ms 毎）、`drag`（drag の途中）、沈む `sink-0..2`、`november`、`hidden`、`reduced`、`glass`（壁紙をぼかした近似の地）。日めくりの部分の animation は `host-calendar-desk.gif`。
- build: `make ZEDBSD_CONFIG=plan/ws155/tests/config-amd64-calendar.mk BUILD=build/ws155-zed build/ws155-zed/bin/calendar` が warning 0。`sinf` などは zedBSD の libc.so にあることを確かめた。host の gcc `-Werror`・clang `-Wshadow` も 0。
- style-check: 新しい file は違反 0。
- QEMU（T1 に依頼する）: App Home から Calendar を起動、全画面の PNG、日付の click と drag の PNG。
- 未実施: QEMU、実機、Week・Day の表示、保存、3D の CPU の負荷の測定（呼吸の間は約 20 fps で 3D の部分を描き直す）。

## T1-179 の指摘（2026-10-05、Q1 が QEMU の PNG で気づいた 3 点）と直し

- 指摘: Work を 20 日に drop した直後の PNG（C-drop.png）で、chip は出ているのに 20 日に pill が無く、14 日と 20 日に枠が 2 つ、日めくりは 14 のまま。
- 原因（2 つが重なった）:
  1. drop は panel で受けていたが、panel を月の表の後に描いていた。drop の frame の月の表は drop の前の状態（14 の枠、pill なし、20 は drag の的の枠）で、chip だけが新しかった。次の frame で直るはずだが、
  2. 1 frame が重い（host の -Os で全体 56〜75 ms、QEMU ではもっと）ので、撮影が drop の frame と次の frame の間に入った。session.log には DROP・SELECT 20・FLIP 14→20 が出ており、data は正しく変わっていた。
- 直し:
  - panel を月の表の前に描く（drop は表示中の frame の cell で探す）。drop の frame で pill・選んだ枠・頁めくりの始まりが揃う（host の `sink-0` で確かめた）。
  - 日めくりだけが動く frame（呼吸・頁めくり）は、前の全体の frame が残した日めくりの下の絵を戻して日めくりだけを描き直す（`cal_view_desk_only`・`cal_view_draw_desk`、widget は描かないので kl_ui の frame は始めない）。全体の frame と画素が同じことを host の試験 `desk-only` で確かめた。
  - texture の bilinear を軽くし、呼吸の frame を 10 fps に。host の -Os で、日めくりだけの frame は 26 ms（全体の frame は 49〜70 ms）。
- host: PASS 8（`desk-only` を足した）。zedBSD の build は warning 0、style-check 0。

## ユーザーの再指示（2026-10-05、q745-i02）と直し

ユーザー「カレンダーのスクショ、いいですね。気に入りました。カレンダーはglass透過にして、ペインは分離して背景はなしの、Filesと同じスタイルにしましょう。右下のカレンダーの画像部分のスペースがちょっと無駄っぽく見えるので、メモ・ノートの領域にしましょう。日付ごととかでなくて、アプリの唯一のメモ領域でOKです。メモはドラッグして日付にドロップすると、メモ属性の予定として追加して、でもこれは予定追加ではなくて、予定追加の機能を応用した、メモの保存にしましょう。」

- Files と同じ style: glass では card が窓の端まで届き（余白 0）、間は 8、card の地は Files と同じ薄い veil（sidebar は `glass_sidebar`、他は `glass_content`）、card の間は地なしで desktop が透ける。種類の card とメモの地も薄い veil に。不透明の窓では今までどおり。
- 右下の大きな 3D の日めくり・リボン・呼吸の動きを外した（常時の動きは無くなり、何もしない時は再描画しない）。
- メモ: app に 1 つの複数行のメモ（mock は memory だけ、初期の例文あり）。click で keyboard を受け、Enter で改行、Backspace、Esc で離す。見出し（メモの印・「Memo」・「Drag to a date」・grip）を drag して日付に落とすと、その日にメモの写しを保存（`CALENDAR MEMO date=… length=…`、予定の追加の drag と drop の仕組みを使う）。cell では予定の pill と分けて、白地に枠線とメモの印の pill（題は 1 行目）。
- 選んだ日: メモの下に card。左に小さな 3D の日めくり（日が変わると頁がめくれる、静かな動きとして残した）、日付、件数、その日の予定（時刻と題）とメモ（全文）を並べる。日付を押すとその日のメモが読める。
- 3D は Add Event の 4 つの icon と、選んだ日の小さな日めくりに残した。
- host: PASS 9（`memo` を足した、呼吸の frame は外した）。PNG は `start`・`flip-0..5`・`drag`・`sink-0..2`・`memo-drag`・`memo`・`november`・`hidden`・`reduced`・`glass`、頁めくりの GIF は `host-calendar-desk.gif`。zedBSD の build は warning 0、style-check 0。
- T1-179c の指摘（窓の高さ 800 では選んだ日の card が見えない）: メモの欄の高さを、選んだ日の card（小さな日めくり＋数行）が残るように縮める（最大 150、最小 60）。host の `host-calendar-short.png`（1280×680、desktop の 800 から title bar を引いた高さ）で card が見えることを確かめた。PASS 9、warning 0、style-check 0。
