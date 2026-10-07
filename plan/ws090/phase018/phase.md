<!-- awesome-plan project=zedbsd record=ws090-p018 -->
# ws090-p018: 設計 — pointer の追従の再描画を一定の frame rate に（hover・drag の範囲選択）

Parent: [WS090](../ws.md)
Status: test-wait（T1 依頼中。q791-i01、P1、2026-10-06 実装済み。Files の host での一致の確かめは安全の判定で保留、下の「残り」）
Disposition: normal
Related: [BUG-221](../../bugs/BUG-221.md)・[BUG-226](../../bugs/BUG-226.md)

## 由来（ユーザー、2026-10-06 UAT）

BUG-221「マウス移動イベントすべてで再描画していることがインタラクションの遅れの原因だと思いますが、これは概ね一定のフレームレートに丸めてアニメーションするのが…30fpか15fpsでいいと思います。」BUG-226「Mailアプリの左ペインで…描画が遅れます…何かGPU描画でない合成とかをやっている気がしました…左ペインのあるFiles,Mail, Calendar,Settings、すべて修正対象です。」

## 設計

1. **まず測る**: 左の pane の hover の 1 回の再描画で、CPU の時間（行の描画・icon の rasterize・文字の rasterize・blur）と GPU の時間を分けて log（`KL PERF redraw ms=... cpu=... upload=...`）。選択の色の地に icon を CPU で描き直している（毎回の rasterize や texture の作り直し）なら、それを cache する（icon と文字は 1 回だけ texture にし、選択の色は地の矩形で描く）。
2. **frame rate の丸め**: libkeiland の window の再描画の要求を「dirty にするだけ」にし、実際の描画は wl_surface の frame の callback が来た時に 1 回（最新の pointer の位置で）。pointer の移動の event では描かない。callback が来ない間に要求が続いても描画は 1 回。上限は output の更新の速さ、重い app は 30 fps（33 ms）に間引く option。
3. **部分の再描画**: hover の行が変わった時は、前の行と新しい行だけを damage にする（全体を描き直さない）。
4. 対象: Files（範囲選択の drag・左の pane）・Mail・Calendar・Settings（左の pane）・desktop の範囲選択（compositor、同じ考えで compositor の frame に合わせる）。

## 試験と Phase

- host: 要求の集約（10 回の移動で描画 1 回）。
- AAT・実機: `apps.files.drag-select-follow`（注入の移動と描画の log の遅れ）、ユーザーの感触。
- 実装: p018a（測定と CPU の合成の除去、1 LW）、p018b（frame の callback での集約と部分の再描画、各 app、0.7 LW）。

## q791-i01（P1、2026-10-06）: 測定と実装

### 測定（host、Settings の描画を perf で）

Settings の host の描画（`plan/ws089/tests/host-build.sh` の settings-render、Wi-Fi の頁、1180x800、host の -O2）: 1 frame の全体の再描画は約 7 ms
（host の CPU）。時間の大半は **CPU の canvas の半透明の角丸の矩形の塗り**（`fm_canvas_round_gradient` → `canvas_run` → `canvas_blend_premultiplied`、
全体の 55〜60%）。glass の card・行の地が半透明なので、画素ごとに 4 channel の割り算付きの合成をしていた。icon の rasterize や blur ではない。
target の build は -Os で CPU も遅いので、1 frame は数十 ms の見込み。**全ての app で、pointer の hover が変わるたびに窓の全体を CPU で描き直していた**
（kl_ui の app は pointer の移動の event ごとに）。

### 直し

| 変更 | commit | 効果（host） |
| --- | --- | --- |
| `files/canvas.c`・`libkeiland/ui/canvas.c` の `canvas_blend_premultiplied` を 2 channel ずつ 16 bit の lane で計算（÷255 は正確な `(t + (t >> 8) + 1) >> 8`、飽和も同じ） | 82bb75de | 旧と新の結果は 0 から 255 の全ての組と 5000 万の乱数の組で完全一致（scratch の試験）。full frame は約 25% 速い |
| Settings: hover の変化は前と今の行（hit の矩形 ＋ 6 px）だけを clip して描き直す（`hover_pending`・`hover_damage`、glass の時はその矩形だけ透明に） | 82bb75de | hover の 1 frame は約 0.2 ms（full は約 7 ms）。**画素は full と完全一致**（wifi・ethernet・home × light・dark × 3 点、settings-render の新しい `peek=` で確認） |
| Files: hover の変化と rubber band の drag は、前と今の hover の矩形、前と今の band、選択の変わった item の矩形だけを描き直す（`damage_pending`・`fm_ui_damage`） | 82bb75de | build は通る。host の描画での一致は**未確認**（下） |
| Mail・Calendar・Phone: pointer の移動は widget の hover が変わる時か button を押している時だけ frame を描く（今までは移動の event ごとに全体を再描画） | 7897390b | — |

host の描画の道具: `plan/ws089/tests/host-render.c` と `plan/tools/files/host-render.c` に `peek=PATH`（前の action が描いた frame を描き直さずに書く）を追加、
Files の renderer は damage の frame も描く。

確認: build（files・settings・phone・mailer・calendar、zedBSD）warning 0、keiland-linux.mk exit 0、`plan/ws170/tests/run-host-phone.sh` PASS。
QEMU は T1 に依頼。

## q791-i02（P1、2026-10-06）: T1-233 (a) の settings-regress の FAIL

T1-233 (a)（証拠 `/home/awe/zedBSD-worktrees/t1/build/t1-233a/`）の 5 件の FAIL は、どれも試験が今の Settings の中身より古いことによる。
再描画の変更（82bb75de）でも ebb2780d でもない: 失敗はどれも「数」の食い違いで、画面と log の動き自体は正しい。

| 試験 | 失敗の行 | 原因 |
| --- | --- | --- |
| p002 | `PAGE about$` | 一覧は Home の次に 24 頁（Ethernet が 2 番目）。Network（5 番目）から About までは 19 回の Down だが、試験は 18 回で Updates で止まっていた |
| p006 | `PAGE about`・`PAGE about back` | 同じ理由で、23 回の Down は Updates まで。About が 24 回目 |
| p008 | `SEARCH query=dns results=2` | "dns" は Ethernet の頁の keyword（`pages.c`）にもあり、結果は 3（Ethernet・Network・"DNS servers"）。2 番目の結果をクリックすると Network を開くのは同じ（`RESULT index=1`・`open page=network` は ok） |
| p004・p009 | `LOOK pictures count=6`・`ready count=6`、p009 の Meadow・Twilight | ws099-p019（2026-10-06）で `Lakeside.png` が `wallpapers/` に入り、壁紙は 7 枚（Kei・Aurora・Dawn・Lagoon・Lakeside・Meadow・Twilight）。p009 は 4 番目を Meadow と思ってクリックし Lakeside を選んでいた（zdesktop の log に Lakeside） |

直した試験（`plan/ws089/tests/`）: p002 は Down を 19 回、p006 は 24 回で 25 頁以上、p008 は `results=3`、p004・p009 は `count=7`、p009 は
Lakeside を含めた 6 枚を順に選び、最後の tile を `index=106` で待つ。source の変更は無い。確認: `sh -n`（5 本）。QEMU での再実行は T1 に依頼する。

## 残り

- Files の host での画素の一致の確かめ（q801）はユーザーの判断で取りやめ（2026-10-06、canceled）。T1-233 (b) の Files の試験は PASS。
- Mail・Calendar の hover は q866 で部分の再描画にした（`kl_ui_take_damage`、KL_VERSION 62、[BUG-226](../../bugs/BUG-226.md) の q866）。Phone は未（同じ口で直せる）→ q875 で部分の再描画にした（下）。desktop の範囲選択（Files の desktop の mode）は q866 で部分の再描画にした（[BUG-221](../../bugs/BUG-221.md) の q866）。
- frame の callback での集約（設計の 2）は、描画が軽くなったので未実施。測った遅れ（注入から再描画の log まで）は未測定。

## q875（P1、2026-10-08）: Phone の hover の部分の再描画

- 直し（`userland/desktop/phone/main.c`）: Mail・Calendar（q866）と同じ形。hover だけが変わった frame（`lit_changed`、drag・動き・他の変化が無い時）は `kl_ui_take_damage` の part を clip にして描く。drag の間（button を押している）は全体。`ph_wait` は `lit_changed` でも 0 を返す。
- host 試験: `plan/ws170/tests/host-phone.c` に `hover-part`（60 回の移動、opaque と glass、部分の frame と全体の frame が画素で同じ）を足した。`sh plan/ws170/tests/run-host-phone.sh build/ws090-p018/host-phone`: PASS 19、FAIL 0（`hover-part glass=0 parts=22`・`glass=1 parts=22`）。
- build（warning 0）: `make -j16 BUILD=build/ws090-p018 ZEDBSD_CONFIG=plan/ws170/tests/config-amd64-phone.mk build/ws090-p018/bin/phone`、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws090-p018-linux all`（rc 0）。
- QEMU（T1、任意）: T1-389 と同じ形で Phone の左の一覧の行の上を pointer で動かし、行の光りが付いてくること・跡が残らないこと（PNG）。未実施。
- 残り: frame の callback での集約（設計の 2）と遅れの測定は未実施のまま（任意）。Music・Photos は対象外（同じ口で直せる）。
