<!-- awesome-plan project=zedbsd record=ws090-p017 -->
# ws090-p017: 設計 — libkeiland の慣性 scroll（全ての窓）と開始の遅れ

Parent: [WS090](../ws.md)
Status: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: T1-230 の FAIL は 32137e34 で直し、T1-230b kinetic-guest PASS、T1-242・T1-246 kinetic-apps-guest PASS（26 項目）。下の「残り」は他の app の慣性と遅れの測定）（旧: test-wait（T1 依頼中。q790-i01、P1、2026-10-06 実装済み。残りは下の「残り」））
Disposition: normal
Related: [BUG-211](../../bugs/BUG-211.md)・[BUG-218](../../bugs/BUG-218.md)

## 由来（ユーザー、2026-10-06 UAT）

BUG-211「慣性スクロールが実装されていないようです…この機能はlibkeilandのUI機能に入れて、どのウィンドウでもスクロールバーを使うときに有効にすれば利用できるようにしたいです。」BUG-218「Phoneアプリで慣性スクロールが効きました！でも、スワイプ動作から300msくらい遅れてスクロールが開始します。これは可能な限り小さく、50ms以内にしたいです。どんなに遅くても80msです。」

## 設計

- 場所: `userland/desktop/libkeiland/ui/scroll.c`（scroll の状態）に慣性を入れ、`list.c`・`scroll-bar.c` を使う全ての widget と、自前の scroll を持つ app（Phone・Files・Settings・Mail・Calendar）が同じ口を使う。Phone の独自の慣性は外して共通に寄せる。
- 速度の推定: 2 本指の scroll の event（wl_pointer の axis と axis_source=finger、axis_stop）の直近 100 ms の移動と時刻の最小二乗。指が離れた（axis_stop）時の速度で慣性を始める。
- 減衰: 指数（時定数 325 ms、iOS に近い）、速度が 20 px/s 未満で止める。端で止め、軽い弾み（overshoot 24 px まで、200 ms で戻る）は option（既定 on）。
- 新しい指の接触（axis の event）で慣性をすぐ止める。
- **開始の遅れ（BUG-218）**: 指の動きの最初の axis の event を受けた frame で scroll を動かす（gesture の判定や速度の窓を待たない）。今の 300 ms の内訳（compositor の gesture の判定の待ち、app の frame の待ち）を測り、compositor の 2 本指の scroll は「端の gesture でない」と決まるまで client に送らない今の待ちがあれば、端の band の外で始まった接触は即座に送る。目標: 指の動きから最初の再描画まで 50 ms 以内、最大 80 ms。
- 描画: 慣性の間は frame の callback ごとに 1 回だけ動かす（BUG-221 の frame rate の丸めと共通、[ws090-p018](../phase018/phase.md)）。
- API: `kl_scroll_set_kinetic(scroll, bool)`（既定 on）、`kl_scroll_tick(scroll, now)`（frame ごと）。KL_VERSION を上げる。

## 試験と Phase

- host: 速度の推定・減衰・端の止まり・新しい接触での停止。
- AAT: `apps.settings.kinetic-scroll`（wifi の一覧）、`apps.phone.scroll-latency`（注入の時刻から再描画の log まで）。実機はユーザー。
- 実装: p017a（libkeiland の慣性と API、host 試験、0.7 LW）、p017b（各 app への適用と Phone の独自の慣性の置き換え、開始の遅れの測定と短縮、0.7 LW）。

## q790-i01（P1、2026-10-06）: 実装（p017a と p017b の主な部分）

commit 58b9027a（試験は別の commit）。設計からの変更: 2 本指の scroll は compositor が**wheel の notch**（2.5 mm ごとに 15 単位）にしてから client
に送っていたので、client は指の始まり・終わり・速度を知り得なかった。慣性の前に compositor を直した。

| 層 | 変更 |
| --- | --- |
| compositor（`userland/desktop/wayland/touchpad.c`・`touchpad.h`・`seat.c`・`input.c`・`zwl.h`） | touch pad の 2 本指の移動を notch に加えて **wheel の単位**（`ZWL_TOUCHPAD_NOTCH_UNITS` = 15、約 0.17 mm に 1 単位）でも数え、client へは `wl_pointer.axis_source = finger`・単位ごとの `axis`（discrete 無し）で送る（`zwl_seat_axis_finger`）。指が離れる（SWIPE2 の END）と `axis_stop` を両方の軸に（`zwl_seat_axis_stop`、log `ZWL AXIS stop`）。App Home の page・音量・tab は従来どおり notch で（notch 0 の時も、その上なら受ける）。**開始の遅れ（BUG-218）**: 最初の scroll は 2.5 mm の後でなく約 0.17 mm の後に client に届く |
| libkeiland（`ui/window.c`・`window.h`・`ui/scroll.c`・新 `ui/axis-track.c`・`ui/ui.c`、`keiland-ui.h`・`keiland.h`、`exports.map`、Makefile 3 つ） | `kl_window_event` に `axis_source`、新しい kind `KL_WINDOW_AXIS_STOP`。axis の値の端数を捨てない（`wl_fixed_to_int` → `wl_fixed_to_double`）。`kl_scroll_axis`: 指は content を握り（飛んでいる物は止まる）、glide 無しで即座に動かす。`kl_scroll_axis_stop`: 指の速度（`kl_axis_track`: 直近 100 ms の移動 ÷ 時間、60 ms 以上止まってから離した時は 0）で既存の `kl_scroller` の慣性・端の弾みで飛ぶ。`kl_ui_axis`: 窓の axis event を pointer の下の scroll へ（握った scroll を指が離れるまで保ち、frame にもう無い scroll には触れない）。KL_VERSION 40 |
| app | Phone・Mailer・Calendar・file chooser: `KL_WINDOW_AXIS` と `KL_WINDOW_AXIS_STOP` を `kl_ui_axis` に。**Settings**（自前の窓と scroll）: axis の source・stop・端数、指が握った pane（page か list）を即座に動かし、離すと速度で飛ぶ（時定数 325 ms の指数の減衰、20 px/s 未満か端で止まる、log `ZSETTINGS KINETIC start pane=… velocity=…` と `KINETIC stop`）。飛ぶ間は 8 ms ごとに frame |
| Phone の padding（BUG-218） | glass の時の card の周りの margin 12 → 0、間の gap 10 → 8（Settings の glass と同じ） |

確認（host）: `plan/ws090/tests/host-input.sh` 72/72（新: track の速度・休んだ指・`kl_scroll_axis` の即座の移動と離した後の飛行・wheel の glide・`kl_ui_axis` の
window event からの通し）。`plan/ws159/tests/run-host-touchpad.sh` ok（25）、`plan/ws142/tests/run-host-gesture.sh` ok（62）、
`plan/ws170/tests/run-host-phone.sh` PASS、`plan/ws090/tests/host-widgets.sh` 94/94、`plan/ws089/tests/host-build.sh` built。build: wayland・settings・phone・
mailer・calendar・files（zedBSD）warning 0、keiland-linux.mk exit 0、`exports.py --check` ok。`ui/scroll.c` を使う他の host の script（keiui の chooser、
textedit の core、ws102 の inset、ws155 の calendar、ws170 の phone、ws090 の widgets）に `ui/axis-track.c` を足した。そのうち chooser・textedit・
inset・calendar・ws089 の dark は **この変更の前から** `kl_appearance_get`・`kl_theme_choose` の未定義で link できない（既存の壊れ、未修正）。

QEMU: `plan/ws090/tests/kinetic-guest.sh`（`config-amd64-kinetic.mk`: pen の guest に Settings。touchinject の pad で 2 本指を 20 mm 上へ約 100 ms で動かして離す）を
T1 に依頼。実機（5330 の touch pad、開始の遅れの体感、慣性の感じ）は UAT。

## 残り

- 自前の scroll を持つ他の app（Files・Text Editor・Terminal・PDF Viewer・Image Viewer・Browser・Monitor・Notes）の慣性は未対応（compositor の変更で
  scroll は細かく早く始まるようになるが、離した後は飛ばない）。`kl_axis_track` で同じ形に足せる。
- 開始の遅れの測定（注入の時刻から再描画の log まで、AAT の `apps.phone.scroll-latency`）は未実施。Phone の 300 ms が touch screen の swipe の話なら、
  別の経路（kl_ui の touch の drag の判定）を見る必要がある。
- 端の軽い弾み（overshoot）は kl_scroll の既存の rubber band に任せた（Settings は端で止まるだけ）。

## q790-i02（P1、2026-10-06）: T1-230 の FAIL の直し

T1-230: `ZWL AXIS stop` は出て list は下へ scroll したが、Settings の log に `ZSETTINGS KINETIC` が無かった（kinetic-start・kinetic-stop FAIL、回帰の
ws159-p004 は PASS。証拠 `/home/awe/zedBSD-worktrees/t1/build/t1-230/kinetic-run{1,2}/`）。

原因（読み）: Settings は指の移動の時刻を **event を読んだ時の自分の時計**（`se_clock`）で track に入れていた。Settings が 1 frame を描く間（QEMU の CPU の
描画で数十 ms）に移動が溜まり、まとめて同じ時刻で読まれ、離した（axis_stop）時刻も描画の後になる。速度の窓の時間が 0 になるか、最後の移動から
60 ms（`KL_AXIS_TRACK_REST_US`）以上たって「休んだ指」と判定され、速度 0 で飛ばない（その場合は何も log しなかった）。libkeiland の窓は compositor の
時刻（`window_stamp`）を使うので影響しない。

直し（32137e34）: Settings の axis と axis_stop の event に compositor の時刻（`se_event.axis_ms`、wl_pointer の time）を持たせ、track と
離した時の速度はその時刻で測る。診断の log を追加: 指が握った時 `ZSETTINGS KINETIC hold pane=…`、速度が足りない時 `KINETIC none pane=… velocity=… moves=…`。
build（settings）warning 0。再試験は T1 に依頼（同じ `kinetic-guest.sh`）。
