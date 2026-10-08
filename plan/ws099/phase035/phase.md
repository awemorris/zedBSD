<!-- awesome-plan project=zedbsd record=ws099-p035 -->
# ws099-p035: 設計 — App Home の stage と 2 層の animation

Parent: [WS099](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 案 A、T1-229 pass・T1-231 は needs-person 2 と回帰 pass。遅れの体感は 5330 の UAT）（旧: in-progress（2026-10-06 q783-i01 P2: p035a の montage を作り Q1 経由でユーザーに提示、選択待ち。p035b 以降は選択の後。以前: planned（Q1 の設計の第 1 版）））
Disposition: normal
Related: [BUG-236](../../bugs/BUG-236.md)・[BUG-225](../../bugs/BUG-225.md)・[BUG-232](../../bugs/BUG-232.md)・[BUG-237](../../bugs/BUG-237.md)・[ws128-p012](../../ws128/phase012/phase.md)

## 由来（ユーザー、2026-10-06 UAT）

- BUG-236「アプリ一覧は、暗い背景のステージに、アプリアイコンが置かれ、ややスポットライトがそれぞれのアイコンに当たって、光沢のある床にアイコンが反射しているエフェクトがいいです。」
- BUG-225「アプリ一覧の表示がワンテンポ遅れる感じがします。0.7sくらい…最初に直ちに描画できるテクスチャで画面を覆うアニメーションを開始して…アイコンは別なアニメーションで後追いで浮かび上がる、2層性のアニメーションにすれば…」
- BUG-232 起動中の app は起動せず切り替える。

## 1. 見た目（stage）

- **地**: 暗い stage。画面全体を、今の desktop の blur（wallpaper と窓）の上に暗い glass（黒、不透明度 0.82 の目安）で覆い、中央の上が少し明るい放射状の濃淡（上部の bar の「中央が明るい」と揃える）。
- **床**: icon の行ごとに、icon の下端の少し下に**光沢の床の線**（横の細い明るい帯、端に向かって消える）。床より下に **icon の反射**（上下を反転した tile、不透明度 0.25 から下へ 0 に fade、高さは icon の 0.35）。
- **spotlight**: 各 icon の上から、icon の幅の 1.6 倍ほどの楕円の柔らかい光（白、不透明度 0.10〜0.14）を icon の後ろの床に。hover・選択の icon は光を強める（0.22）。
- **icon**: montage-4 の tile。地が暗いので穴は `GLASS_HOLE_GROUND`（暗い stage が記号に透ける、BUG-237 の穴の扱い）。名前の文字は白（不透明度 0.9）。
- dark・light の外観で同じ（stage はいつも暗い。上部の bar と揃える）。

## 2. 2 層の animation（BUG-225）

| 層 | 始まり | 中身 | 時間 |
| --- | --- | --- | --- |
| 第 1 層（覆い） | 入力（Super・App Home の button・gesture）の**次の frame** | 暗い stage の地と濃淡だけ（準備の要らない単色＋既にある blur の texture） | 120 ms の fade in |
| 第 2 層（中身） | 準備が終わった frame（遅くとも 400 ms で第 1 層だけでも見える） | tile・spotlight・床・反射・名前 | icon ごとに 30 ms ずらして下から 12 px 浮き上がり＋fade（180 ms） |

- 準備: tile は起動時の atlas（ws128-p012、20/26/48/72 px）にあるので、App Home の開きで要るのは名前の文字と配置と反射の texture。**起動時と app の一覧の変わった時に先に作っておく**（反射は tile の atlas を上下に反転して描くだけなので texture は要らない、shader の uv の反転）。それでも 0.7 秒の遅れが残るなら、その内訳（文字の rasterize・blur の再計算）を測り、文字は起動時に cache、blur は今の frame の物を使い回す。
- 閉じる: 第 2 層を 100 ms で fade out、第 1 層を 120 ms で fade out。
- 遅れの目標: 入力から第 1 層の最初の frame まで 1 frame（16 ms）以内、第 2 層の始まりまで 150 ms 以内。

## 3. 起動中の app（BUG-232）

App Home で app を選んだ時、その app id の窓が既にあれば起動せず `shell_switch_to(最後に使った窓)`（[ws142-p007](../../ws142/phase007/phase.md) の関数、最大化の session の状態に合わせる）。起動中の app の tile の下に小さな点（bar の今の app の下線と同じ色）。

## 4. 試験

- host: stage の描画の host render（hole-host と同じ形、`plan/ws099/tests/`）。第 1 層の始まりの frame の数の計算。
- QEMU（T1・AAT）: `desktop.home.open-latency`（入力から第 1 層の log の行までの時間、`ZWL HOME layer=cover` と `layer=content`）、`desktop.home.switch-running`（起動中の app を選んで新しい窓が増えない）。
- 実機（ユーザー）: 見た目と速さの感触。

## 5. Phase の分け方（ベータ2）

| Phase | 内容 | 見積もり |
| --- | --- | --- |
| p035a | montage（spotlight・反射の強さ 2〜3 案）をユーザーに見せて選ぶ | 0.3 LW |
| p035b | 実装: stage の地・床・反射・spotlight の描画（glass の shader の mode の追加は避け、既存の image・glass の mode の組み合わせで） | 1 LW |
| p035c | 2 層の animation と準備の先回り、遅れの測定、BUG-232 | 0.7 LW |
| p035d | AAT のシナリオ・T1・規約の見直し | 0.3 LW |

## 6. 未決（ユーザー）

- spotlight・反射の強さ（p035a の montage で選ぶ）。
- light の外観でも暗い stage でよいか（案: よい、bar と揃える）。

## p035a の montage（2026-10-06 q783-i01 P2）

- 道具: [p035-stage-mock.sh](../tests/p035-stage-mock.sh)・[p035-stage-mock.py](../tests/p035-stage-mock.py)（tile は compositor の icons.c の zwl_icon_tile の 72 px を tile-dump で。地・床・spotlight・反射・名前は shape の組み合わせを host で近似。実物の compositor ではない）。
- 出力（P2 の worktree）: `build/ws099-p035a/montage.png`（A・B・C を半分の大きさで横に）、`stage-a.png`・`stage-b.png`・`stage-c.png`（1280x800）。
- 案: A 弱い（spotlight 0.08、hover 0.16、反射 0.15、床 0.10）、B 中（設計の値: 0.12・0.22・0.25・0.18）、C 強い（0.18・0.30・0.38・0.28 と上からの光の筋 0.07）。Settings を hover の icon として明るく。
- 未決: ユーザーの選択（A・B・C か調整）と、light の外観でも暗い stage でよいか。選択を待つ間は p037 に進む（Q1 の指示）。

2026-10-06 ユーザー（クリックの回答）: stage は **案 A（弱い: spotlight 0.08・hover 0.16、反射 0.15、床の線 0.10）**。light の外観でも暗い stage のまま。→ p035b 以降は A の値で実装する。

## p035b の実装（2026-10-06 P2、案 A の値）

Status（p035b）: test-wait（T1 依頼中。実装・build まで）

| 所 | 内容 |
| --- | --- |
| `home.c` の `zwl_home_draw` | 前の白い glass 0.48 と青み 0.22 を、暗い stage に: 黒い glass 0.82（`light` の印で dark の外観でも写さない）＋上の中央から青白い柔らかい光 0.10（MODE_SHADOW）。Home を描く間は `keep_colours`（stage は両方の外観で暗い） |
| 床 | `home_draw_floors`・`home_draw_floor`: 行ごとに画面の上の icon の左右より 36 広い線を tile の下 6 px に、中央 0.10 から両端へ 0 に（24 の piece）、上下の 1・2 px にも弱い線（0.55・0.35・0.2 の重み） |
| spotlight | `home_draw_spotlight`: tile ごとに床の後ろに幅 1.6 tile・高さ 36・柔らかさ 8 の白い楕円、0.08（pointer の下は 0.16） |
| 反射 | `glass_draw_app_tile_reflection`（glass.c）: tile の下の 35 % を上下に反転して床の下に、0.15 から下へ 0 に（8 帯）。中抜きの記号は反射では床の色 |
| 名前・点 | 名前は白 0.9 で反射の下（`HOME_LABEL` 26 → 49、click の範囲も）、page の点は白 0.32 |
| 中抜き | tile の中抜きから壁紙（`GLASS_HOLE_WALLPAPER`、p034b と同じ） |

確認: zedBSD・Linux の build warning 0、style-check（home.c・glass.c・glass.h）指摘 0。QEMU は T1 に依頼。p035c（2 層の animation・準備の先回り・遅れの測定・BUG-232）は次。

## p035c の実装（2026-10-06 P2）

Status（p035c）: test-wait（T1 依頼中。実装・build・AAT の host 試験まで）

| 所 | 内容 |
| --- | --- |
| 2 層（BUG-225） | stage（第 1 層）は開いた最初の frame から全部（desktop の層が横へ滑って現れる今の動きのまま）。icon（第 2 層）は `home_content`: 開いた時から 1 つ 30 ms ずつ遅れて、12 px 下から 180 ms で浮かび上がり fade（ease-out）。床は最初の icon と一緒に。閉じる時・drag の間・drag で開いた時は今のまま Home の進みと一緒 |
| 測定 | log `ZWL HOME layer=cover after_ms=`（開く要求から stage の最初の frame）・`ZWL HOME layer=content after_ms=`（最初の icon が見え始めた frame） |
| 先回り | `home_prepare`（tick、最初の frame の後に 1 回）: 名前の glyph を測って atlas に（ASCII の外の文字、日本語の名前は初めての時に描かれるので）。log `ZWL HOME prepared names=N`。tile は起動時の atlas、反射は tile の反転なので texture は要らない |
| BUG-232 | `home_running_window`: 一覧の app の窓（picture のある app は app_id の picture が同じ、無い app は program の名前、shell は除く、dialog は除く）で map の順の一番新しい窓。あれば起動せず `ZWL HOME switch name= surface= client=` と `zwl_glass_activate`（desktop を出し、最小化から戻し、layout mode に合わせる）。起動中の app の名前の下に短い線 |
| `home_launch` | Home を閉じたかを返す（切り替えの時は activate が閉じる）、呼ぶ側は 2 度閉じない |
| AAT | `tests/scenarios/desktop/home/switch-running.md`・`open-latency.md`（新）と helpers_desktop.py の helper |

確認: zedBSD・Linux の build warning 0、style-check（home.c）0、check-scenarios PASS（87）、aat run-host PASS。QEMU は T1 に依頼。遅れの目標（cover 16 ms・content 150 ms）は QEMU と実機で測る。

## p035d（2026-10-06 P2）

- AAT のシナリオ: p035c の `desktop.home.switch-running`・`desktop.home.open-latency`（active、helper つき）。stage の見た目は `desktop.home.super-key` の撮影で（T1-229）。
- 規約: p035b・p035c で変えた C（home.c・glass.c・glass.h・zwl.h）を coding-style.md と照らし style-check 指摘 0。手での見直し: 宣言は関数の頭、条件の中の呼び出しなし（`home_running_window` の結果は変数に）、Boolean は if で、return の前の comment。
- 残り: T1 の結果（T1-229 と p035c の依頼）と実機の感触（遅れの値）。Status（p035d）: test-wait。
