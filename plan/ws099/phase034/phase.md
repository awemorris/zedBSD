<!-- awesome-plan project=zedbsd record=ws099-p034 -->
# ws099-p034: 上部の system bar のデザインの調整（グループの pill と黒い地）

Parent: [WS099](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 第 1 版は p034b に置き換わり、p034b は T1-228 で cleared）（旧: planned（2026-10-06 mock-1 にユーザーが回答、実装してよい））
Disposition: normal

## 由来

ユーザー（2026-10-06）「Keilandコンポジタにコメントです。上部バーのデザインを調整したいです。添付が、1つめは上部バー左端でのアイコンのまとめかたと、黒いテクスチャをベースにするというアイディアです。2つめは、仮想デスクトップ切り替えの表示です。バーの中央部に置きます。3つめは、通知アイコン領域のアイコンのグルーピングです。4つめは時刻表示のグルーピングです。」

## 参考の画像（ユーザーの添付、Q1 が読んだ要点）

| 画像 | 部分 | 要点 |
| --- | --- | --- |
| [bar-1](images/bar-1.png) | 左端 | 左端に Kei の logo（青い葉の形）、細い区切りの線、その右に **起動中の app の icon を 1 つの暗い（黒い半透明の質感の）pill の中にまとめる**。icon は角の丸い四角の色の地に白い記号（P・歯車・画像・計算機・メモ・M）。bar の地は**黒い texture を base** にするアイディア |
| [bar-2](images/bar-2.png) | 中央 | **仮想 desktop の切り替えを bar の中央**に。小さな丸い横長の点（pill）が 4 つ並び、今の desktop だけが白い輪郭の大きめの pill。縮小の絵（今の thumbnail）ではない |
| [bar-3](images/bar-3.png) | 右 | 通知の icon の領域: 検索（虫眼鏡）・入力の方式（丸の中の A）・Wi-Fi・音量・電池を **1 つの pill にまとめる**。入力の方式の A は丸い地 |
| [bar-4](images/bar-4.png) | 右端 | 通知の icon の pill の右に、**時刻（Mon Oct 5 13:43）を別の pill** にまとめる。間に細い区切り |

## 範囲（案）

- bar の地を黒い texture（暗い glass）に。左・中央・右の要素を pill（角の丸い暗い半透明の帯）でまとめる。
- 左: logo ＋ 区切り ＋ app の icon の pill。中央: 仮想 desktop の点の pill（今の thumbnail の代わり、クリックで切り替え）。右: 状態の icon の pill ＋ 時刻の pill。
- light・dark（WS089 p017）、bar の高さ（ws099-p031）、Alt+Tab の preview（WS142・BUG-209）、USB の icon（ws132）、通知（WS156 の H7: bar の媒体の icon を通知に置き換え）との整合。
- app の icon は ws128-p012（デザインした icon への差し替え）と同じ絵を使う。

## ユーザーの決定（2026-10-06）

「plain dark glassだけど中央部が少し明るいような濃淡がいいですね。でもそれをベースにあなたが考えてくれてもいいです。ドットの明るさはウィンドウの有無で変えなくていいです。アイコンはラウンドでカラーをベースにする意見を反映してほしいです。」

- bar の地: **無地の暗い glass、中央が少し明るい濃淡**（横方向の緩い gradient）。これを base に担当が詰めてよい。light の外観でもこの暗い地を保つかは mock で示して確かめる。
- 中央の点: **窓の有無で明るさを変えない**（今の desktop だけが白い輪郭の大きめの pill）。
- app の icon: **角の丸い色の地に白い記号**（ws128-p012 にも反映）。

## mock（2026-10-06 P2、ユーザーの確認待ち、実装はまだ）

[mock-1.png](images/mock-1.png)（A〜D と拡大）、[mock-1-screen.png](images/mock-1-screen.png)（画面全体、Birch Lake）。QEMU を使わず host で描いた:
app の絵と Kei の mark は compositor の rasterizer（icons.c・mark.c）の出力、他は compositor の shape（glass・solid・ring・text）で
描ける物だけで描いた。道具 `plan/ws099/tests/p034-bar-mock.sh [OUT.png]`。

- 地: 壁紙の blur に暗い青みの tint（64%）、明るすぎる壁紙では暗い glass の上限（luma 0.22）まで暗くする（白い文字の contrast）。
  中央が少し明るい（横の緩い gaussian、+8.5%）、上から薄い sheen、下端に細い明るい線、bar の下に薄い影。
- 左: Kei の mark、区切りの線、起動中の app の icon（26 px、間 8）を 1 つの pill に。今の app は icon の下の短い白い線。
- 中央: 4 つの点の pill（点 18×7、今の desktop は 30×12 の白い輪郭）。窓の有無で変えない。画面の中央に固定。
- 右: 状態の pill（入力の方式の A は丸い地、USB は出ている時だけ、Wi-Fi は扇形、音量、電池）、その右に時計の pill。
- dock した時（C）: app の icon の代わりに、窓の icon と title の pill・窓の操作（戻る・進む・home・場所）、窓の button（最小化・
  元に戻す・閉じる）の小さな pill を中央の点の左に。
- light の外観（D）: 参考に明るい glass の版も描いた。提案は light の外観でも A の暗い bar（light・dark で bar を変えない）。

確かめたいこと（ユーザー）: (1) light の外観でも暗い bar（A）か、明るい bar（D）か、(2) 参考の bar-3 にある検索の虫眼鏡は今の bar に
機能が無いので mock では省いた（足すなら App Home の検索を開く）、(3) 今の app の印（icon の下の線）、(4) dock の時の配置（C）、
(5) Wi-Fi を今の棒から扇形に、(6) 中央の点の pill の大きさ。高さは ws099-p031 の 44 px のまま。

## 2026-10-06 ユーザーの回答（mock-1 への）

- light の外観でも**暗い bar**（案 A）。
- 「サーチボタンはバーにはいらないです。」→ 検索の button は bar に置かない。
- 今の app の下線: OK。
- dock の時の配置（案 C）: OK。
- Wi-Fi の扇形: OK。ただ「下部のマルが1pxほど左にズレているように見えます。気のせいかもしれないですが。」→ 扇の下の点の中心の位置を確かめ、ずれていれば直す。
- 点と pill の大きさ: OK。bar の高さは 44 のまま。
→ 実装してよい（app の icon は ws128-p012 の円・単色・白抜きの新しい形に合わせる）。

## 実装の途中（2026-10-06 P2、ラップアップ、b095413c）

- 実装済み（agent/p2 b095413c、build warning 0、QEMU 未実施、merge は保留）: 暗い glass の bar（light・dark の両方、`glass_shape.dark_glass`・
  `server->keep_colours`）と中央の明るい帯、app の pill（26 px の icon、間 8、今の app の下線）、中央の desktop の点の pill（窓の有無で変えない、
  log `ZWL GLASS desktops x= step=34 width=30`）、状態の pill（IME の丸い chip・USB・Wi-Fi の扇（`GLASS_ICON_WIFI_1..4`、点と弧は同じ中心で
  ユーザーの「点が 1 px 左」を直す）・音量・電池）と時計の pill、dock の時の題の pill と button の pill（間 34）。検索の button は無し。
- **待ち**: app の icon の形。ユーザーは ws128-p012 の montage-4（3B の帯の地に 3W の中抜きの記号）を選んだ。b095413c の mark は円（montage-2 の時の案）
  のままなので、ws128-p012 の統合の後に bar の pill もその形にする。それまで merge しない。
- 残り: 形の統合、bar の試験の座標の確かめ（`zdesktop-p065.sh`・`p072.sh` は 2026-10-07 に削除: 4 つの desktop とメニューの desktop の切り替えが前提で、WS181 の 3 つの desktop で意味が無くなった。bar の座標は plan/ws181/tests/ws181-guest.sh が見る）、T1 の QEMU で light・dark・
  dock・Wiseview・App Home の PNG、全文の規約。

## 2026-10-06 保留の実装の置き場所

P2 の bar の実装（b095413c、旧 agent/p2、ユーザーの指示の履歴の書き換えで branch は消す）は [held/p034-bar-b095413c.patch](held/p034-bar-b095413c.patch) に patch として保つ。montage-4 の icon の実装（main acbb7e4a）の後なので、当てる時は shell.c・glass.c・icons.c で衝突する見込み。再開の時はこの patch を今の main に合わせて当て直す（bar の app の pill は zwl_icon_tile の形を使う）。

## 2026-10-06 当て直し（P2、ラップアップ）

- held の patch（b095413c）を main 1225dfd6＋BUG-237（dec0fd39）の上に `git apply -3` で当て直した。衝突（glass.c・home.c・shell.c）は montage-4 の側を取り、
  patch の円の mark（`APP_MARK_PICTURE`、home.c の円の tile、atlas の app の記号 48・18 px、頭文字の円）は捨てた。残したもの: 暗い glass の bar
  （`dark_glass`・`keep_colours`）、中央の明るい帯、app の pill、desktop の点の pill、状態の pill と時計の pill、Wi-Fi の扇（`GLASS_ICON_WIFI_1..4`）、
  IME の丸い chip、dock の時の題と button の pill。
- montage-4 に合わせた変更: bar の app の tile は 26 px（glass.c の保つ大きさ 28 → 26、1 対 1 で描く）、hover の光と「+N」は tile と同じ角の丸い四角
  （半径は辺の `GLASS_ICON_TILE_RADIUS`）。bar の上の tile の記号は暗い bar が透ける（`mark_hole` が `keep_colours` の間は `GLASS_HOLE_GROUND`）。
- 確認: zedBSD・Linux の compositor の build exit 0、warning 0。style-check: glass・home・shell・icons・apps-bar・zwl.h で 0。network.c・input-method.c は
  main の時点からある 50 件だけ（増えていない）。QEMU・実機は未実施。
- host の montage: `plan/ws099/tests/p034-bar-host.sh [OUT.png]`（この branch の描く code の位置・色を panel.frag の式で描く。tile は icons.c、blur は近似、
  文字は PIL、dock の窓の menu の語は代わり）→ [bar-montage-4.png](images/bar-montage-4.png)（Birch Lake・Lakeside × light・dark × floating・dock、2 倍の拡大）。
- 再開の地点: ユーザーに bar-montage-4.png を見せる。T1 の QEMU で light・dark・dock・Wiseview・App Home の PNG と bar の試験の座標（`zdesktop-p065.sh`・
  `p072.sh`）。全文の規約の見直し。held の patch は当て直したので、Q1 の merge 後に消してよい。

## 2026-10-06 ユーザーの bar-montage-4 への意見と第 2 版の設計（title bar の design の変更）

ユーザー「ウィンドウ最大化ドック時のウィンドウボタン（最小化、最大化、閉じる）は、画面右上に置いて、時計などは左にずらすのがいいです。でもこれはアニメーションでやらないと見た目が悪いですね。アニメも実装です。」

- **dock の時の配置**: 窓の button の pill（最小化・restore・閉じる）を**画面の右上の端**（今の時計の pill の位置）に置き、状態の pill と時計の pill を**その左へずらす**。中央の desktop の点は動かさない。窓の icon・題・操作の pill は左のまま。
- **animation**: dock に入る時、button の pill が右端に fade と scale（0.9 → 1）で現れ、時計と状態の pill が左へ slide（180 ms、ease-out）。dock から出る時は逆。浮いた窓だけの時は今の配置（button の pill は無し）。
- [ws142-p007](../../ws142/phase007/phase.md) の `layout_mode` の切り替え（app の切り替えで dock・窓が変わる）でも同じ animation。切り替え先も dock なら button の pill は動かさず中身だけ替える。
- 実装の Phase: p034b（配置と animation、0.5 LW）。ユーザーの順の指示「Cの設計の見直しをまずやりましょう。そのあと、タイトルバーのデザイン変更」で、C の後に行う。

## 2026-10-06 ユーザーの変更（light の外観の bar）

「モンタージュの画面上部のバーですが、ダークモードではこの黒い色でOKです。ライトモードでは、ウィンドウタイトルバーと同じ色味にしてほしいです。」
→ 同日の前の回答「light の外観でも暗い bar（案 A）」を置き換える。dark の外観は今の黒い glass の bar のまま。light の外観では、bar の地を窓の title bar と同じ色味（light の title bar の glass の色・透明度）にし、icon・文字・pill の色も light の title bar に合わせて読めるようにする。App Home の stage は light でも暗いまま（別の決定、ws099-p035）。p034b で実装する。

## 2026-10-06 p034b の montage（q794、P2）

- 道具: [p034b-bar-host.sh](../tests/p034b-bar-host.sh)・[p034b-bar-host.py](../tests/p034b-bar-host.py)（p034-bar-host.py の描き方を流用）→ [bar-montage-5-light.png](images/bar-montage-5-light.png)。
- light の外観: bar の地は窓の浮いた title bar と同じ白い glass（0.64）、文字・記号・desktop の点は title bar の暗い ink（0.12, 0.16, 0.24）、pill は ink の 0.06 の地と 0.10 の縁、Kei の mark は bar の色（light の bar 向けの色）。app の tile の中抜きの記号は title bar の ink の地が透ける（淡い tile の上でも記号が読めるように。dark の bar は今の GLASS_HOLE_GROUND）。
- dark の外観: p034 の暗い glass の bar のまま。
- dock の時: 窓の button の pill（最小化・restore・閉じる）を右上の端（今の時計の位置）に、状態と時計の pill をその左へ。中央の desktop の点は動かさない。（animation は実装の時。）
- 待ち: ユーザーの確認（light の bar の色味、tile の記号の地）。

## 2026-10-06 ユーザーの回答（bar-montage-5-light）

- 「このまま実装」: light の bar を montage-5 の形で実装してよい。
- app の icon の中抜きの記号: 「デスクトップ背景が透けて見えるとうれしいです。ライトもダークも、Apps一覧も。」→ bar（light・dark）と App Home の Apps の一覧の icon は、中抜きの記号の部分を本当に透明にし、そこから desktop の背景（壁紙）が透けて見えるようにする（ink の地や白で塗らない）。title bar の小さな tile の扱いは別（変えるならユーザーに確かめる）。

## 2026-10-06 p034b の実装（q794、P2）

Status（p034b）: cleared（2026-10-06 Q1 判定: T1-228 は fail 無し（pass・needs-person）。PNG を Q1 が目視: light の bar は title bar と同じ白い glass、dark は黒、dock の時は button の pill が右上の端（buttons=1252,1218,1184）で時計がその左。実機は UAT）

| 所 | 内容 |
| --- | --- |
| `glass.h`・`shell.c` | `struct glass_bar_colours` と `zwl_glass_bar_colours`: dark は p034 の白い ink・暗い pill（0.25）・白い縁（0.12）・lit 0.20・点 0.42、light は title bar の ink（0.12, 0.16, 0.24, 0.94）・線 0.16・pill 0.06・縁 0.10・lit 0.10・点 0.36。`draw_system_bar`・`draw_bar_strip`（light は白い glass 0.64 と暗い hairline 0.10、dock しかけの drag の間は lit で少し暗く。dark は今のまま）・`draw_bar_group`（`draw_bar_group_faded` に）・`draw_desktops`（点と今の desktop の輪を ink から）を両方の外観に |
| `apps-bar.c` | app の pill の地・縁・今の app の下線・hover の光・「+N」を `zwl_glass_bar_colours` から |
| `shell.c` の配置 | dock の窓の button の pill を右上の端（`buttons_x`・`buttons_width`、閉じるが一番右）、状態と時計の pill はその左へ `bar_dock` の分だけ寄る。題と menu の場所は desktop の点の手前まで |
| animation | `bar_dock_follow`（tick）: 前の窓が dock（全画面・App Home・WiseView でない）なら 1、それ以外は 0 へ、180 ms の ease-out（今の位置から始める）。button の pill は `bar_dock` で fade と 0.9 → 1 の scale（`draw_bar_buttons`）、時計と状態は slide。layout_mode の切り替えで dock のまま app が替わる時は 1 のまま動かない |
| 中抜きの記号（ユーザーの決定） | `GLASS_HOLE_WALLPAPER`（glass.c）: tile の内側に壁紙のその場所を鮮明に描いてから tile を重ね、中抜きから壁紙が透ける。bar の tile（light・dark、`mark_hole` が `keep_colours` の間）と App Home の一覧の tile（home.c）に。title bar の小さな tile は今のまま（SCENE・GROUND） |
| `zwl.h` | `bar_dock`・`bar_dock_from`・`bar_dock_to`・`bar_dock_ms` |

確認: zedBSD の compositor・Linux の Keiland の build warning 0。style-check（shell.c・glass.c・glass.h・home.c・apps-bar.c・zwl.h）指摘 0。QEMU は T1 に依頼（下）。host の montage（p034b-bar-host.py）は ink の地の版のまま（実装は壁紙の透け）。
