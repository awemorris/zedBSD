<!-- awesome-plan project=zedbsd record=ws181-p001-design -->
# WS181 設計: 窓の状態、App Home の独立のモード、画面の端の gesture、整列のメニューと整列モード

Parent: [ws181-p001](phase.md)
版: 第 3 版（2026-10-07 P2。第 2 版で第 1 版への指摘 B1〜B4・S1〜S12・M1〜M9 を反映（§9）、第 3 版で第 2 版への指摘 BL1・BL2・S-a〜S-i・minor 1〜10 と、ユーザーの回答（§7）を反映（§10））

この文書は compositor（`userland/desktop/wayland/`）の挙動の設計で、実装は p002〜p004。ユーザーの原文は [ws.md](../ws.md) の「由来」。
前提にした今の実装: `layout.c`（session の `layout_mode`、ws142-p008）、`shell.c` の `window_dock`・`window_undock`・`layout_match`・
`layout_keep_front`・`layout_press_switches`・`bar_press`・pull（`glass_motion_take`・`pulled_rect`・`pull_back`）・`kwl_glass_button` の順、
`home.c` の `kwl_home_layer`（desktop の層を右下へずらして角を残す）・`kwl_home_button`・`kwl_home_motion`、`shell.c` の `wiseview_edge_press`
（下端から上で Wiseview）、`objects.c` の surface の消滅、`display.c` の `kwl_schedule`（毎 pass で `kwl_glass_tick`）。

## 0. 今の不具合の原因（目標 1・2）

- docked の窓 A から別の app B へ切り替えると（docked mode）、B が dock され、A は **dock のまま**後ろに残る（ws142-p007 §1「切り替え元は dock のまま後ろに残る」）。
- B を floating に戻すと `window_undock()` の終わりで `layout_mode = WINDOWED` になるが、A は `maximized = 1` のまま。WINDOWED では他の app の窓を隠さないので、docked の A が B の後ろに見える。
- A を click すると `layout_press_switches()` が「窓の mode で docked の他の app の窓への press は切り替え」として A を floating に戻す。ユーザーが見た「click すると floating になる」はこれ。
- docked の窓を閉じると `layout_keep_front()`（毎 tick）が次に前に来た窓を dock する（2026-10-06 の決定）。2026-10-07 の UAT はこれを floating に変える（§1.3）。

## 1. 窓の状態（p002）

### 1.1 状態の名前

窓（親を持たない toplevel）の状態は次の 5 つのどれか。`kwl_layout_state()`（layout.c、純関数）が今の値から決める（log と host 試験のため。描画と press は今どおり `layout_hides()` が決める）。

| 状態 | 条件 | 見え方 |
| --- | --- | --- |
| `minimized` | `surface->minimized`（**利用者が明示に**最小化した） | 描かない。docked mode が終わっても最小化のまま |
| `fullscreen` | `surface->fullscreen` | 全画面 |
| `dock-hidden` | docked mode で、表示中の desktop の前の app でない app の窓（最小化でない、maximized の値によらない） | 描かない、press も受けない。**docking の隠れ**。docked mode が終わると floating で見える |
| `docked` | `surface->maximized` で、上のどれでもない | dock の領域、title は system bar |
| `floating` | 上のどれでもない | 自分の場所と大きさ |

- `dock-hidden` は flag として持たず、mode と前の app から毎回決める（今の `layout_hides()` の規則のまま）。保存する隠れは `minimized` だけなので、2 つの隠れが混ざることは無い。
- 同じ app の他の窓は docked mode でも隠さない（2026-10-06 の決定「アプリ内のウィンドウは下に見えてもいい」）。docked のものも floating のものもある。
- dialog・sheet（親を持つ窓）は状態を持たず親に従う（今どおり）。

### 1.2 不変条件

- **I1**: `layout_mode = WINDOWED` の間、`maximized = 1` の窓は**どの desktop にも 1 つも無い**（最小化の窓、map の前の窓も含む）。
- **I2**: `layout_mode = DOCKED` の間、表示中の desktop で見える窓は前の app の窓だけ（前の app の docked の窓は複数あり得る: 同じ app の窓は隠さない）。他の app の窓は `maximized` の値によらず描かない。
- **I3**: 整列モード（§5）は `layout_mode = WINDOWED` の間だけある。`layout_set(DOCKED)` が全 desktop の整列モードを終える（§5.3）。

I1 は「docked mode を出る」1 つの関数 `layout_leave(server, front, x, y, via)`（shell.c、x・y は front を戻す場所。pull は pointer から計算した場所を渡す）で守る:

1. `front`（floating にする操作の対象の docked の窓、無ければ NULL）は今どおり animation つきで `window_undock()`。
2. それ以外の `maximized = 1` の窓（全 desktop、最小化の物、map の前の物も）は **animation 無しで** `window_float_quiet()`（どれをどうするかは純関数 `kwl_layout_leave_action()`: front は FLOAT、他の docked は QUIET、floating・全画面・dialog は KEEP）。全画面の窓で `fullscreen_docked = 1` の物はそのまま: windowed の mode で全画面を出ると、protocol.c が `restore_*`（floating の場所）へ戻す（p002 の実装で確かめた）。
3. `server->dock_owner[]` と `server->dock_owner_gone[]` を全 desktop で消す（§1.4）。
4. `layout_set(WINDOWED, via)`。
5. log: `KWL LAYOUT leave via=<via> front=<id|0> quiet=<数>` と、状態の要約 `KWL LAYOUT windows desktop=<n> mode=<docked|windowed> floating=<n> docked=<n> dock_hidden=<n> minimized=<n> fullscreen=<n>`（表示中の desktop、AAT が読む）。要約は leave・`window_dock`・持ち主が変わった時（手順 B）にも出す（S-f）。
6. 2 の順（minor 5）: 自分の場所を持つ窓を先に、`restore_default` の窓を後に戻す（後の窓の `kwl_glass_place` が先に戻った窓を避ける）。

- **M4（再帰しない）**: `window_undock()` は `layout_set()` を呼ばない形にする（今の終わりの `layout_set(WINDOWED)` を外す）。docked mode を出る操作（restore の button・bar の title の double click・pull・touchpad の TOP2・client の UNMAXIMIZE・title の triple click（`click_docked_third`、leave の後に `window_lower`、S-a））は全部 `layout_leave(surface, x, y, via)` を呼び、`layout_leave` が中で `window_undock` を呼ぶ。client の UNMAXIMIZE は、前の窓（持ち主）からの時だけ leave にし、後ろの dock-hidden の窓からの時はその窓だけを `window_float_quiet()` する（裏の app の要求で前の窓まで floating にしない、S-c）。`layout_match` の FLOAT は I1 の下では起きないが、念のため `window_undock` だけを呼ぶ（mode は変えない）。
- **`window_float_quiet(server, surface)`**（M5・S4）: `maximized = 0`。場所と大きさは `restore_*`。ただし `restore_default = 1`（`dock_restore_default()` が付けた既定の場所で、自分の floating の場所を持ったことが無い窓）の窓は `kwl_glass_place()` の段ずらしで場所を決める（dock で開いた窓が全部同じ場所に重ならない）。`kwl_glass_fit()` で領域の中へ。`server->anim`・`server->pull`・`server->click_docked`・`server->drag` がこの窓を指していれば消す。configure を送り `window_resized()`。log `KWL LAYOUT float-quiet surface=<id> x= y= w= h=`。
- `restore_default` は surface の新しい field。`dock_restore_default()` が 1 にし、`window_dock()`（本当の floating の場所を記録する）と `window_float_quiet()`（場所を決めた後）が 0 にする。

### 1.3 出来事ごとの規則

| 出来事 | 規則 | 今との違い |
| --- | --- | --- |
| 窓を dock（title の double click・maximize の button・title を bar へ drag・client の MAXIMIZE） | `window_dock()` → `layout_set(DOCKED)`。他の app の窓は `dock-hidden`。全 desktop の整列モードを終える（I3） | 整列の終わりが増える |
| docked の窓を floating に（restore の button・bar の title の double click・pull・touchpad の TOP2・client の UNMAXIMIZE） | `layout_leave(その窓)`: **全部の窓が floating**、最小化は最小化のまま | 後ろの docked の窓も floating に（目標 1） |
| docked mode で app の切り替え（Alt+Tab・Wiseview・bar の icon・App Home の起動中の app・activation） | `layout_match`: 切り替え先を dock（今どおり）。切り替え元は dock のまま後ろに残る（`dock-hidden`）。行き来のたびに大きさを変えない | 同じ |
| docked mode で `layout_match` を通らない前面化（Super+Alt+P の FOCUS previous（edit.c）・menu・titlebar の raise・Notes の角（corner.c）） | 毎 tick の観測（§1.4 の手順 B）: 前に来た窓が floating なら dock して持ち主に（切り替えと同じ）。全画面なら持ち主は変えない | 同じ（今の `layout_keep_front` と同じ結果を、観測の手順で） |
| docked の窓の title の triple click（double click の dock の取り消し、`click_docked_third`） | `layout_leave(その窓)` の後に後ろへ（全部 floating） | 変更（S-a。2 本指の flick は floating の title にだけ効くので docked の窓には無い、minor 2） |
| 窓の mode で app の切り替え・click | I1 により docked の窓は無いので、切り替え先は floating のまま前へ | `layout_press_switches()` を削除（目標 1 の「click で floating になる」が無くなる） |
| **表示中の desktop の docked の窓を閉じる**（app が自分で閉じる、kill -9、unmap を含む） | `layout_leave(NULL, "closed")`: 隠れていた窓は floating で見える、最小化の窓は最小化のまま | **変更**: 2026-10-06 の「次の窓も最大化」を 2026-10-07 の UAT で置き換える（目標 2、D1） |
| **表示中の desktop の docked の窓を最小化** | 閉じるのと同じ（`via = "minimized"`）。その窓は最小化で、戻すと floating | 変更（D2） |
| docked の窓を**連れて**隣の desktop へ（Ctrl+Alt+Shift+矢印: 窓と desktop が一緒に動く） | docked のまま運ぶ。持ち主は新しい desktop へ移る、mode は docked のまま | 同じ（S7） |
| docked の窓を**見えない** desktop へ（Wiseview の tile を他の desktop の絵へ drag） | 閉じるのと同じ（`via = "moved"`） | 変更（D2） |
| **見えない desktop** の docked の持ち主が閉じる・消える（見えない desktop の窓の最小化の要求は今も無視される、minor 3） | その desktop の持ち主を忘れるだけ（表示は変えない、mode は docked のまま）。その desktop へ移ると、前の窓を dock する（下の desktop の切り替えの行） | 新しく明記（B3）。表示中の画面が裏の出来事で変わらないため |
| docked mode で新しい窓が開く | docked で開く（ws099-p033、`kwl_glass_open_docked`）。map の後の観測で持ち主になる。前の docked の窓は `dock-hidden`（別の app）か下に見える（同じ app） | 同じ（持ち主を付ける時が map の後、B1） |
| docked mode で desktop を変える | 先の desktop の前の窓が floating なら dock して持ち主に（今の `layout_keep_front`、session の tablet mode）。空の desktop では何もしない | 同じ |
| 全画面の出入り | `layout_mode` を変えない。出る時は mode に戻る（`kwl_glass_unfullscreen_docks`） | 同じ |
| docked の窓が全画面になって、その窓を閉じる | 閉じるのと同じ（持ち主は全画面の間も同じ窓、§1.4 の (e)） | 変更 |
| 全画面の窓で touchpad の BOTTOM2・TOP2（`gesture_fullscreen`） | 今どおり `layout_set(DOCKED)` して全画面を出る（全 desktop の整列を終える、I3） | 整列の終わりが増える |

### 1.4 持ち主の観測（「docked の窓が無くなった」の検出）

compositor には unmap・消滅を shell に知らせる一般の hook が無く、前面化の経路も多い（B2）ので、**持ち主は毎 tick の観測で付け、消滅だけを objects.c から知らせる**。

- `server->dock_owner[KWL_APPS_DESKTOPS]`（`struct kwl_object *`）と `server->dock_owner_gone[KWL_APPS_DESKTOPS]`（`unsigned`）を `struct kwl_server`（kwl.h）に足す。shell.c の `DESKTOPS` は kwl.h の `KWL_APPS_DESKTOPS`（4）と同じ値なので、kwl.h の側の名前を使う（M1）。
- 消滅: `objects.c` の surface の消滅（今 `server->drag` などを消している所）から新しい `kwl_glass_forget(server, object)`（shell.c）を呼ぶ。この窓を指す `dock_owner[d]` を NULL にして `dock_owner_gone[d] = 1`、整列の枠（§5.1）の窓なら枠の窓を NULL にしてその desktop の `arrange.gone` を立てる。unmap は消滅でないので hook を足さない（M3: 観測の (b) が見る）。
- `kwl_glass_tick()` の `layout_keep_front()` を `layout_follow()` に置き換える（毎 pass）。docked mode の時だけ、次の順で:

**手順 A（持ち主の確かめ、overview・drag・pull の間も行う）** — 各 desktop `d` について、`P = dock_owner[d]`:

1. `dock_owner_gone[d]`（P は消えた）: 0 に戻し、`d` が表示中なら `layout_leave(NULL, "closed")` して終わる。見えない desktop なら忘れるだけ。
2. P が NULL なら次の desktop。
3. P が (b) `mapped` でない、(c) 最小化した、(e) `maximized` でも `fullscreen` でもない: `d` が表示中なら `layout_leave(NULL, 理由)`（`closed`・`minimized`・`floated`）して終わる。見えない desktop なら `dock_owner[d] = NULL`。
4. (d) P の desktop が `d` でない: P の desktop が表示中（連れて行った）なら `dock_owner[P->desktop] = P`、`dock_owner[d] = NULL`（leave しない）。そうでなく `d` が表示中（見えない desktop へ送った）なら `layout_leave(NULL, "moved")` して終わる。どちらでもなければ `dock_owner[d] = NULL`。

**手順 B（前の窓の観測、表示中の desktop だけ）** — App Home・Wiseview・switcher が見えている間、drag・pull の間は行わない（今の `layout_keep_front` と同じ）:

- `T` = 前の窓（`kwl_top_window()` の sheet の親、dialog なら見えている親）、`mapped`・image あり。
- T が無い（空の desktop）: 何もしない。
- T が `maximized`: `dock_owner[desktop] = T`（新しい持ち主。今の持ち主と同じなら何もしない。変わった時 log `KWL LAYOUT owner desktop=<n> surface=<id>`）。
- T が全画面: 持ち主は変えない。
- T が floating（親を持たない）: 切り替えと同じく `window_dock(T, …, "front")` して持ち主に（今の `layout_keep_front` と同じ log `KWL LAYOUT front surface=<id> action=dock`）。

- 手順 A の 1・3 が手順 B より先なので、持ち主が閉じた時に次の窓が dock されることは無い（今の不具合の (4) の原因）。
- `window_dock()` は mapped の窓をその場で持ち主にする（観測の前に閉じても取りこぼさない、minor 1）。map の前の `kwl_glass_open_docked` は観測のまま（B1）。
- `window_minimize()` は終わりで `layout_follow()` を直に呼ぶ（tick を待たない。冪等）。`window_to_desktop()` は呼ばない: Ctrl+Alt+Shift+矢印は `window_to_desktop()` の後に `desktop_turn()` するので、その間に確かめると「見えない desktop へ送った」と誤る（p002 の実装で分かった）。次の tick で確かめる。
- 前の窓が全画面で、その下に docked の持ち主が残る（Notes の角など）間は、持ち主は下の窓のまま。全画面を出た窓は docked mode なら dock され（`kwl_glass_unfullscreen_docks`）、次の手順 B で持ち主になる。

### 1.5 試験（p002）

- host（`plan/ws181/tests/host-layout-state.c`）: `kwl_layout_state()` の表（mode × docked × 最小化 × 全画面 × 前の app か）。持ち主の観測の判定を純関数 `kwl_layout_owner_check(owner の写し, desktop, shown)` → keep / leave(理由) / forget / moved（layout.c）にして表で試す。`layout_leave` の後の I1 は、窓の配列を相手にする小さな関数を host で試す。
- 今の試験の追従（S10、Q1 の許可 2026-10-07）: `tests/scenarios/desktop/windows/layout-mode-switch.md` の 3〜5 段（後ろの docked の窓が見える・click で floating・閉じたら次も dock）は新しい規則に書き換える。ws142 の `plan/ws142/tests/host-layout.c` と `p010-guest.sh` の該当の段も直し、ws142-p007・p008 の phase.md に「2026-10-07 の UAT で WS181 が置き換えた」の行を足す（ws142 は incomplete で、p010 が使う回帰の試験なので直して残す）。
- QEMU（T1）: Files・Terminal・Calculator を開き:
  1. Terminal を dock → Alt+Tab で Files → Files を restore → 3 つとも floating（`KWL LAYOUT windows … docked=0`）、Terminal を click しても大きさが変わらない。
  2. Calculator を最小化 → Files を dock → Files を閉じる → `KWL LAYOUT leave via=closed`、Terminal は floating で見え、Calculator は最小化のまま（`minimized=1`）。
  3. （S12）docked mode で Terminal を開き、1 s 後も docked（`KWL LAYOUT owner desktop=1 surface=<Terminal>` と `KWL LAYOUT windows … mode=docked docked=1`、`leave` が無い）。
  4. （S12）docked の Files を `kill -9` → `leave via=closed`。
  5. （S12）desktop 2 で Terminal を dock、desktop 1 へ（Files が dock される）、guest の shell から desktop 2 の Terminal を閉じる → `leave` が無く Files は docked のまま。desktop 2 へ移っても何もしない（空）。
  6. （S12）docked mode で Super+Alt+P（FOCUS previous）→ 前に来た窓が dock → それを閉じる → `leave via=closed`。
  7. （S12）docked の Terminal を Ctrl+Alt+Shift+→ で連れて行く → desktop 2 で docked のまま（`leave` が無い）。

## 2. App Home の独立のモード（p003）

### 2.1 見た目

- 開く時、desktop の層（壁紙・窓）は**画面の上へ全部出ていく**（`kwl_home_layer()`: `x = 0`、`y = -progress × (高さ + HOME_SHADOW)`、`scale = 1 - 0.04 × progress`。`HOME_SHADOW` は層の影の幅）。
  開ききった後（progress = 1）は desktop の層を描かない。今の右下の角の覗き（`HOME_KEEP`・`HOME_KEEP_NEAR`・`HOME_NEAR`）と、それを press で閉じる処理を削除する。
- 下端から上への swipe（§3）で開く時、層は指に付いて上がる（`progress = 上への距離 / HOME_RISE_DISTANCE`、360 px）。離した時 `HOME_THRESHOLD`（0.30）以上で開き、未満で戻る。
- launcher の click・Super・左上の角からの drag で開く時も、同じ上への動き。左上の角の drag（右下へ）は今の対角の進み `(dx + dy) / 2 / HOME_DRAG_DISTANCE` を progress に使う（指の向きと層の向きは違うが、mouse 向きの古い入口なので動きを 1 つに揃える方を採る）。
- 閉じる時は層が上から戻ってくる（逆の動き）。
- Home は overview でない（S-g）: `layout_hides()` は Home の間も docked mode の他の app の窓を隠したまま（上へ出ていく層は、隠れた窓を含まない desktop のまま）。Wiseview・switcher は今どおり overview。
- **system bar は Home の上にも残す**（S9、ユーザーの回答「このまま」）: launcher で閉じる、時計・状態の icon が見える。層は bar の下を通って上へ出る。

### 2.2 開く・閉じる

press の取り合いの順（上ほど先）: 上端の帯（§3、touch）→ Home の launcher・左上の角 → Home の上の press。

| 操作 | 結果 |
| --- | --- |
| 下端から上への swipe（desktop の上） | Home を開く（§3） |
| launcher の click・tap、Super の単独 | 開く・閉じる（今どおり） |
| 左上の角から右下への drag | 開く（今どおり、層の動きは上） |
| Home の上で、縦が横より大きい下向きの drag（どこから始めても、上端の帯を除く） | **desktop の層を上から引き下ろして閉じる**。`HOME_PAGE_START`（10 px）を超えた時に向きを決め（`|dx| ≥ |dy|` なら頁の drag、`dy > 0` なら閉じる drag、上向きは何もしない）、閉じる drag は指に付く（`home_drag = 1 - dy / HOME_RISE_DISTANCE`）。離した時 `dy / HOME_RISE_DISTANCE ≥ 0.30` で閉じ（log `KWL HOME close via=pull-down`、minor 7）、未満で開き直す（`KWL HOME close back`）。今の「左上への drag で閉じる」をこれに置き換える |
| Home の上で下端から上への swipe | 何もしない（D4 の既定。今の `home_bottom_press`（同じ swipe で閉じる）は覗きの角と組だったので外す） |
| Home の上で上端の帯から下への swipe（touch） | Home を animation 無しで閉じ、Wiseview を上端から開く gesture を始める（§3） |
| Esc・app の起動・起動中の app の icon（BUG-232） | 閉じる（今どおり） |

### 2.3 試験（p003）

- host: `kwl_home_layer()` の値（progress 0・0.5・1 で x・y・scale）、Home の上の drag の向きの判定（頁・閉じる・何もしない）と閉じる閾値（純関数にして）。
- 今の試験の追従: `tests/scenarios/desktop/home/` の 5 本に、角の覗き・下端の swipe で閉じるを使う段は無い（2026-10-07 に grep で確かめた）。`launcher-button.md`・`super-key.md` の撮影の期待（層が右下に残る絵）があれば、層が見えない絵に直す。

## 3. 画面の端の gesture（p003）

pointer（mouse）と touch screen（first finger は shell の pointer の左 button、touch.c、`server->shell_source`）は同じ `kwl_glass_button` を通る。

| 始まり | 動き | 結果 | 今 |
| --- | --- | --- | --- |
| 下端（`y ≥ 高さ - 20`）、desktop の上 | 上へ | **App Home**（指に付く） | Wiseview |
| 上端の帯（§3.1、touch だけ） | 下へ `TOP_EDGE_START`（12 px）以上、縦が横より大きい | **Wiseview**（指に付く: `progress = 下への距離 / WISEVIEW_DISTANCE`、240 px） | 無し（bar の press） |
| 上端の帯 | 動かずに離す | その点での press と release を流し直す（bar の widget がそのまま働く） | 押した時に bar の press |
| 上端の帯 | 下以外へ 8 px を超える | press を流し直し、その後の motion は普通に流す | — |
| bar の docked の title（帯の下、または mouse） | どの向きでも drag | **pull**: 窓を floating にし、そのまま移動を続ける（§3.2） | 下向きに 140 px で外れる |
| 左上の角・右上の角 | 今どおり | App Home・Notes | 同じ |
| 全画面の上の下端 | 上へ | 全画面を出る（ws099-p015、今どおり） | 同じ |
| 全画面の上の上端の帯（touch） | 下へ | Wiseview（D6 の既定） | 無し |
| Wiseview の上の下端 | 上へ | Wiseview を animation 無しで閉じ、Home を開く gesture を始める | 無し |

- Super+Tab（Wiseview）・Super（Home）・Alt+Tab は変えない。下端の swipe が Home になるので、Wiseview の入口は上端の帯・Super+Tab・touchpad になる。
- **touchpad の gesture**（ws142-p009: BOTTOM2・UP3 → Wiseview、TOP2 → docked の窓を floating に）は既定では変えない（D3 で聞く）。

### 3.1 上端の帯（B4・S8）

- 帯: `y < TOP_EDGE_BAND`（10 px）、`x ≥ HOME_LAUNCHER_WIDTH`（40、launcher の幅。新しく kwl.h に定義し、home.c の直の 40 も置き換える、minor 6。左上の角 28 px もこの中）、`x < 幅 - CORNER_ZONE`（28、Notes の角）。
- 帯は bar が描かれている時（`bar_cover() == NULL`）だけ。全画面の窓の上に floating の窓がある時（cover あり）、上端は client の物（touch.c は press の時に route を決めるので、流し直しで client に届けられない、S-h）。全画面の窓だけの時は `kwl_glass_edge_button`・`kwl_glass_edge_motion` の両方に帯を置く。
- **touch だけ**（S8、ユーザーの回答「touch だけ」）: `server->shell_source == KWL_CONTACT_TOUCH`（pen は今は mouse と同じ扱い、minor 8）。mouse の press は今どおりその場で bar に届く（mouse の Wiseview は Super+Tab・touchpad）。
- 置き場所（B4）: `kwl_glass_button` の、power dialog・switcher・Wiseview・`click_docked_third`・corner（Notes）・keyboard の後、**Home より前**（launcher・media・IME・volume・network・menu・apps bar・titlebar の controls・`bar_press` が press を先に取る前）。全画面の上は `kwl_glass_edge_button` の同じ位置。
- press: 帯の中なら `server->band_press = 1`、始まりの x・y を覚えて press を取る（return 1）。
- motion（`glass_motion_take` の Wiseview の後、corner の前）: 下へ 12 px 以上で縦が横より大きい → Wiseview の上端の gesture（下）。横か上へ 8 px（`PRESS_MOVE_TOUCH`）を超える → `band_press = 0`、`server->band_replay = 1` で press を `kwl_glass_button` に流し直し（帯の判定を飛ばす）、その motion を普通に流す。
- release: gesture にならなかった press は、press と release を続けて流し直す。流し直す press は押した点で（pointer を一時に押した点へ戻して流し、戻す）。`band_press` は release・lock・Wiseview の開き・Home の開きで消し、`wiseview_top` は Wiseview の gesture の終わりで消す。帯が押さえている間の長押し（apps bar の preview）は帯の中では効かない（minor 8、§8）。全画面の上では流し直さない（帯の 10 px の tap は全画面の app に届かない。S8 で知らせる）。
- Wiseview の上端の gesture: `server->wiseview_gesture = 1`、新しい `server->wiseview_top = 1`、`wiseview_start_y`、`wiseview_current = 前の窓`（M2）。`wiseview_progress()` は `wiseview_top` なら `(pointer_y - start_y) / WISEVIEW_DISTANCE`（下向き）。離した時の閾値は今の `WISEVIEW_THRESHOLD`（0.35）。log `KWL WISEVIEW gesture via=top-edge`、開いたら今の `KWL WISEVIEW opening`。
- Home の上で帯の gesture になった時は Home を animation 無しで閉じる（`kwl_home_dismiss` の animation 無しの版 `kwl_home_close_now`、log `KWL HOME close via=top-edge`）。

### 3.2 dock bar を触ってからの drag（pull）

- 今の pull（`bar_press()`・`kwl_glass_press_move()` で始まり `glass_motion_take()` で追う）を使う。変える所:
  1. 外れる判定を下向きの距離から**押した点からの距離**（`hypot(dx, dy)`）に変える（横や斜めの drag でも外れる）。`server->pull_start_x` を足す。`PULL_DISTANCE` を 140 から 48 px に縮める（触って引き出す感じ）。`pulled_rect()` の補間の t も同じ距離 / 48（S2）。
  2. 外れた後は今どおり `server->drag` に移って、離すまで窓が指（pointer）に付いて動く。窓の中の掴んだ点は title の横の位置の比で決める（今どおり）。
  3. 外れた窓には `layout_leave(その窓, "pull")` が走り、他の窓も floating で見える（`window_undock` の代わりに）。
  4. **CSD の docked の窓**（S-d）: bar に title が無いので bar からは pull できない。client が自分の title の drag で move を要求した時（`KWL_TOPLEVEL_MOVE`、今は maximized の窓では拒否）、pull と同じく `layout_leave(その窓, pointer の下に title の上端が来る場所, "request-move")` して move を始める。
  5. **S2**: 外れた直後の pointer は bar の中にあり得る（横の pull）。`kwl_glass_toplevel_move_end()` の「bar の中で離すと dock」は、move の間に一度 bar の外（`pointer_y ≥ KWL_GLASS_BAR`）へ出た時だけ効かせる（`server->drag_left_bar`。title の press から始まる普通の move は始めから 1、pull から移った move は 0 で始まる）。

### 3.3 試験（p003）

- host: 端の判定の純関数（`kwl_edge_classify(x, y, width, height, source, fullscreen, home, wiseview)` → none / home-up / band / corner、新しい `edge.c`）と、帯の motion の判定（wiseview / replay / wait）、pull の外れの距離を表で試す。
- QEMU（T1、touch の注入 `/dev/input-inject` の MT と pointer）:
  1. 下端から上 → `KWL HOME open via=edge`、撮影（層が見えない Home）。
  2. 上端の帯から下（touch）→ `KWL WISEVIEW gesture via=top-edge`・`opening`。
  3. docked の title を横に drag（mouse）→ `KWL LAYOUT leave via=pull` の後に release で `KWL GLASS moved`（minor 7）。（S12）横に pull して bar の中で離す → dock されない（`KWL GLASS moved`、`dock via=drag` が無い）。
  4. （S12）帯の中の各 widget を touch で tap（時計 → Calendar、desktop の絵 → 切り替え、volume・network の icon → popup）→ 今と同じ log が release の後に出る。
  5. Home の上で下向きの drag → `KWL HOME close via=pull-down`。Home の上で下端から上 → 何も起きない。

## 4. 整列のメニュー（p004）

### 4.1 入口

- bar の仮想 desktop の切り替えの pill の**どこを tap・click しても** → 整列のメニューを開く（もう一度で閉じる）。desktop の切り替えはメニューの中に移す（D5、2026-10-07 ユーザーの回答「pill のどこでも」）。
- メニューは desktop の pill の下に出る glass の popup（network の menu と同じ描き方）。上の段に desktop の絵 4 つ（今の pill と同じ絵、tap でその desktop へ切り替えてメニューを閉じる。log は今の `KWL GLASS desktop=<n> via=menu`）、その下に整列の行が 5 つ、各行に小さな図（枠の配置の線画）と名前:

| ID | 名前（英、翻訳の catalog へ） | 日本語 | 配置 |
| --- | --- | --- | --- |
| `columns` | Side by Side | 左右に並べる（水平に等分） | 横に n 等分（n ≤ 4） |
| `rows` | Stacked | 上下に並べる（垂直に等分） | 縦に n 等分（n ≤ 4） |
| `right-main` | One on the Right | 右に 1 つ、左に縦の分割 | 右半分に 1 つ、左半分を縦に n-1 等分（n ≤ 4） |
| `left-main` | One on the Left | 左に 1 つ、右に縦の分割 | 左右の逆 |
| `grid` | Grid | 格子 | `cols = ceil(sqrt(n))`、`rows = ceil(n / cols)`、最後の行の余りは横に広げる（n ≤ 9） |

- 対象の窓: 表示中の desktop の、map 済み・最小化でない・全画面でない・親を持たない toplevel（desktop の icon の surface を除く）。docked mode の時は `dock-hidden` の窓も含む（整列は全部を floating にするので）。重なりの上から順に、その形の上限まで。上限を超えた窓は今の場所のまま下に残る（D7 の既定）。
- n = 1 は全部の形で 1 つの枠（作業の領域の全体、ただし floating）。n = 2 の `right-main`・`left-main` は左右の 2 等分になる。対象が 0 の時、行は薄く描き、選んでも何もしない。
- 操作: pointer の click・touch の tap・鍵盤（↑↓ と Enter、Esc で閉じる）。メニューの外の press で閉じる。メニューは帯の中でも外でも **release で開く**（minor 10）。
- 今の「pill の絵の tap で desktop を切り替える」を使う試験（`desktop_picture_at` の bar の press の経路、Wiseview の tile を絵へ drag する経路は別で変えない）は、p004 でメニューの中の絵の tap に追従させる。
- touch の tap は上端の帯（§3.1）を通るので、メニューは release の後に開く（p003 に依存、§6）。

### 4.2 枠の計算（`arrange.c`、純関数）

- 作業の領域: dock の領域（bar の下、画面の keyboard の panel を除く）から四方に `ARRANGE_MARGIN`（8 px）を引いた矩形。keyboard の panel は滑っている途中の値でなく落ち着いた値（`kwl_keyboard_reserved()`）を使う（M7）。枠の間は `ARRANGE_GAP`（8 px）。shell.c の private な `struct shell_rect` は使わず、arrange.h に `struct kwl_arrange_rect { int32_t x, y, width, height; }` を置く（M1）。
- 枠は title bar を含む: compositor が title を描く窓（`kwl_decoration_server`）の body は枠の上から `KWL_GLASS_TITLE + KWL_GLASS_GAP` 下から始まり、枠の下まで。client が自分で枠を描く窓（CSD）は body が枠の全体（S5）。body の幅・高さは整数に丸め、余りは最後の枠に足す。
- `kwl_arrange_slots(layout, n, area, slots[])` が枠の矩形を返す。

### 4.3 どの窓をどの枠に（目標 5「今の位置から大まかに」）

- 各窓の中心と各枠の中心の距離の 2 乗の和が最小になる割り当て（n ≤ 9 なので部分集合の DP: `dp[mask]`、2^9 × 9 の手間）。等しい時は重なりの上の窓を先の枠に（決まった結果になる）。
- 窓の中心は floating の場所の中心。docked の窓（とその時 `dock-hidden` の窓）は §4.4 の 1 の後の `restore_*` の場所（floating に戻った時の場所）で数える（S3）。
- `kwl_arrange_assign(centres[], n, slots[], order[])`（arrange.c、純関数、host で試す）。

### 4.4 適用

1. 各対象の窓の今の描かれた矩形（`body_rect`）を glide の始まりとして覚える。docked mode なら `layout_leave(NULL, "arrange")`（全部を**静かに** floating に。前の窓にも undock の animation を付けない: glide と animation が重ならない、S3）。
2. 割り当て（§4.3）。各窓を割り当てた枠へ: `x, y, window_width, window_height` を枠の body に、`placed = 1`、`restore_default = 0`、configure を送る、`window_resized()`。
3. 動きは `ARRANGE_MS`（180 ms）で 1 で覚えた矩形から枠へ滑らせる。今の `server->anim` は 1 つの窓だけなので、整列は `struct kwl_arrange` の中に窓ごとの glide（from・to・始まりの時刻）を持ち、`body_rect()` がそれを見る（`server->anim` の次に）。
4. 整列モードに入る（§5）。log: `KWL ARRANGE apply layout=<id> desktop=<n> windows=<n> slots=<id>@x,y,w,h;...`。
5. メニューでまた形を選ぶと、その形で整列し直す（モードは続く）。

## 5. 整列モード（p004）

### 5.1 状態

- desktop ごとに `struct kwl_arrange { unsigned on; unsigned layout; unsigned count; unsigned gone; struct { struct kwl_arrange_rect slot; struct kwl_object *window; /* glide */ } slots[9]; }`（`server->arrange[KWL_APPS_DESKTOPS]`）。
- 整列モードの間、枠の窓の title の drag は**移動でなく入れ替え**になる（§5.2）。他の操作（click で前へ、閉じる、最小化、2 本指の flick で後ろへ）は今どおり（後ろへは重なりの順だけで、枠は変わらない）。
- 印（S6 の既定）: 整列モードの desktop は、bar の pill のその desktop の絵に形の線画（メニューの行の図と同じ）を重ねる。

### 5.2 title の drag で入れ替え（目標 6、S5）

- 移動が始まる経路は 4 つあり、全部で同じに扱う: title の press（`kwl_glass_button` の終わりの `server->drag`）、title の menu・検索の欄の press が動いた時（`kwl_glass_press_move`）、client の move の要求（`KWL_TOPLEVEL_MOVE`、CSD の窓）、touch の title（`ROUTE_TITLE` は待ちの後に pointer の press になり 1 つ目の経路を通る）。
- 整列モードの desktop の枠の窓なら、`server->drag` でなく新しい `server->swap`（窓、掴んだ点、元の枠の番号）を始める。窓は pointer に付いて動く（移動と同じ見た目）。pointer の下の枠（自分の枠を除く）を glass の縁で示す。
- 離した点が:
  - **別の枠の中** → 2 つの窓の枠を入れ替える。掴んだ窓はその枠へ、相手の窓は掴んだ窓の元の枠へ、どちらも `ARRANGE_MS` で滑る、configure を送る。log `KWL ARRANGE swap a=<id> b=<id> slots=<i>,<j>`。
  - **system bar の中**（`pointer_y < KWL_GLASS_BAR`、今の「title を bar へ drag で dock」）→ その窓を dock し、整列モードを終える（§5.3）。上の段の枠の title は bar の下にあるので、上の段への入れ替えは bar に入らずにできる（M8）。
  - それ以外（自分の枠、枠の外） → 元の枠へ滑って戻る。

### 5.3 整列モードが終わる時

| 出来事 | 結果 |
| --- | --- |
| 整列モードで title の **double click**（今の dock） | その窓を dock、整列モードを終える。log `KWL ARRANGE end desktop=<n> reason=dock` |
| title を **dock bar（system bar）へ drag** | 同じ |
| maximize の button・client の MAXIMIZE | 同じ |
| どの経路でも `layout_set(DOCKED)`（他の desktop での dock、全画面の窓の touchpad の gesture を含む） | 全 desktop の整列モードを終える（I3、S1） |
| docked の窓を floating に戻す（restore・pull・double click） | `layout_leave()` で**全部が普通の floating** になる。窓は整列の時の場所と大きさにいる（dock の前の `restore_*` が枠）が、**整列モードではない**（title の drag は普通の移動）。整列の解除の手順は要らない（目標 6） |
| 枠の窓が閉じる・最小化・別の desktop へ・全画面 | 整列モードを終える（残りの窓はその場の floating、詰め直さない: S6 の既定）。log `reason=closed` など |
| その desktop に新しい窓が開く、または他の desktop から窓が来る（親を持たない toplevel だけ。dialog・sheet は終えない） | 終える（新しい窓は今の置き方で）（S6・M9） |
| 枠の窓の大きさを縁で変える | 終える |
| 他の desktop へ切り替える | 終えない（その desktop の整列は残り、戻ると続く）（M9） |
| メニューでまた形を選ぶ | その形で整列し直す（モードは続く） |

- 終わった後の窓は全部ただの floating で、整列の記録は残さない（`on = 0`、枠の pointer を消す）。
  - **変更（2026-10-08 ユーザーの決定 (b)、[ws177-p037](../../ws177/phase037/phase.md)）**: 終わった時に desktop ごとの「形と各枠の窓」を compositor の中だけで覚える（再起動で消える）。同じ desktop で同じ形をまた選び、対象が同じ窓で同じ数なら、近さでなく前と同じ枠へ戻す（log `KWL ARRANGE recall`）。整列中に同じ形を選び直した時は今の枠を保つ。窓が消えたらその記憶の枠は空になり、一致しなくなる。窓は floating のまま（整列モードではない）は変わらない。
- surface の消滅は `kwl_glass_forget()`（§1.4）が枠の窓を NULL にして `gone` を立て、次の tick で終える。
- **検出**（S-e）: 毎 tick の `arrange_follow()` が各 desktop の整列について、枠の窓が mapped・最小化でない・全画面でない・同じ desktop にいる・`window_width/height` が枠の body のまま（縁の大きさの変更で変わる）を確かめ、外れたら理由つきで終える。新しい窓は `kwl_glass_mapped()`（display.c の map）で、適用の時に覚えた `map_order` の上限より新しい親を持たない toplevel が整列の desktop に map された時だけ終える（D7 で枠の外に残った古い窓では終えない）。他の desktop から来た窓は `window_to_desktop()` で終える。
- **入れ替えの後始末**（minor 9）: `server->swap` は forget・`desktop_turn`・最小化・lock・`window_float_quiet`・整列の終わりで消す。swap の間の client の move の要求は無視する。整列した窓は適用の時に枠の順で前へ上げ、上限を超えた窓はその下に残す。

### 5.4 試験（p004）

- host（`plan/ws181/tests/host-arrange.c`）: 5 つの形 × n = 1〜上限の枠（重ならない、領域の中、隙間 8 px、CSD の body）、割り当ての DP（左右に並んだ窓は `columns` で左右の順を保つ、4 隅の窓は `grid` で同じ隅へ、総当たりと同じ最小値）。
- QEMU（T1）: 窓 3 つで今の desktop の絵を click → メニュー（撮影）→ `right-main` → `KWL ARRANGE apply`（撮影）→ 左上の窓の title を右の枠へ drag → `KWL ARRANGE swap` → 1 つを double click → `reason=dock` と `KWL LAYOUT mode=docked` → restore → `KWL LAYOUT windows … docked=0` で、title の drag が普通の移動（`KWL GLASS moved`、swap でない）。
- （S12）整列中に desktop 2 で窓を dock して desktop 1 に戻る → desktop 1 の整列は終わっている（`reason=dock`、`layout_set(DOCKED)` の時）。CSD の窓（GTK4 の見本）を含む 2 つの整列 → CSD の窓の body が枠の全体。

## 6. Phase の分け方・依存・見積もり

依存（S11）: p002 → p003（pull の外れが `layout_leave` を使う）、p002 → p004（整列の適用が `layout_leave`・`restore_default` を使う）、p003 → p004（pill の touch の tap が上端の帯を通る、`drag_left_bar`）。

| Phase | 内容 | 依存 | 見積もり |
| --- | --- | --- | --- |
| p002 | §1: `kwl_layout_state`・`kwl_layout_owner_check`、`layout_leave`・`window_float_quiet`・`restore_default`、`dock_owner` と `layout_follow`（手順 A・B）、`kwl_glass_forget`、`layout_press_switches` の削除、`window_undock` から `layout_set` を外す、最小化・移動・閉じるの規則、log、host 試験、ws142 の試験と scenario の追従 | p001 | 0.7 LW |
| p003 | §2・§3: `kwl_home_layer` の上への動き、覗きの削除、Home の上の下向きの drag、`kwl_home_close_now`、端の判定（下 → Home、上端の帯の遅らせと流し直し、`wiseview_top`）、Home ↔ Wiseview の移り、pull の距離と `drag_left_bar`、host 試験、scenario の追従 | p002 | 1 LW |
| p004 | §4・§5: `arrange.c`（枠・割り当て）、メニュー（描画・入力）、適用と glide、整列モードの入れ替え（`server->swap`、4 つの経路）・終わり・印、log、host 試験、AAT の scenario を draft で | p002・p003 | 1.3 LW |

## 7. 人の判断

**ユーザーの回答（2026-10-07、クリック、Q1 経由）**: D3 touchpad は「変えない」。**D5 は「pill のどこでも」**（既定と違う: pill のどこを tap してもメニュー、メニューの中に desktop の切り替えも置く、§4.1 を直した）。S6「整列モードを終える」。S8「touch だけ」。D1・D2・D4・D6・D7・S9 は「全部このまま」。N1〜N3（第 2 回の review で足した点）の回答（2026-10-07、クリック）: N1「docked mode を終える」（既定どおり、他に窓が無くても）、N2「そうする」（帯の 10 px は docked の title の上でも Wiseview）、N3「何もしない」（見えない desktop の持ち主が閉じても保留しない、忘れるだけ）。

以下は Q1 に送った時の文（既定の案つき）:

- **D1（知らせ）**: docked の窓を閉じた時、2026-10-06 の決定（次の窓も最大化）を 2026-10-07 の UAT（他の窓は floating で表示）で置き換える。見えない desktop の docked の窓が閉じた時は、表示を変えない（その desktop の持ち主を忘れるだけ）。
- **D2**: docked の窓の**最小化**と、**見えない desktop への移動**（Wiseview の tile の drag）は閉じるのと同じ（docked mode を終えて他の窓を floating で見せる）。**連れて行く移動**（Ctrl+Alt+Shift+矢印）は docked のまま。既定: そうする。
- **D3**: touchpad の gesture（ws142-p009: 2 本指の下端・3 本指の上 → Wiseview、2 本指の上端 → docked の窓を floating に）。既定: 変えない（UAT の文は「画面の端」）。案: 画面に揃えて 2 本指の下端 → Home にする（Wiseview は 3 本指の上・Super+Tab、2 本指の上端は今どおり）。
- **D4**: Home の上で下端から上への swipe は何もしない（今は Home を閉じる）。Home を閉じる gesture は「Home の上の下向きの drag」（と launcher・Super・Esc）。既定: そうする。
- **D5**: 整列のメニューは**今の desktop の絵**の tap で開き、他の desktop の絵は今どおり切り替え。案: pill のどこでもメニューを開き、メニューの中に desktop の切り替えも置く。既定: 今の desktop の絵。
- **D6**: 全画面の上で上端の帯から下（touch）→ Wiseview（下端から上は今どおり全画面を出る）。既定: そうする。
- **D7**: 形の上限（`columns`・`rows`・`right-main`・`left-main` は 4、`grid` は 9）を超えた窓はその場に下に残す（最小化はしない）。既定: そうする。
- **S6**: 整列モードの印は、bar の pill のその desktop の絵に形の線画を重ねる。枠の窓が閉じた・最小化した時は整列モードを終え、残りは詰め直さない。案: 同じ形で残りを詰め直してモードを続ける。既定: 終える。
- **S8**: 上端の帯（10 px）は touch だけに効かせる（mouse の bar の click は今どおりその場で、mouse の Wiseview は Super+Tab）。全画面の app の上でも touch の上端 10 px の tap は app に届かない。案: mouse にも効かせる（bar の click が離した時になる）。既定: touch だけ。
- **S9**: App Home の上にも system bar を残す（launcher で閉じる、時計・状態）。既定: 残す。
- **N1**（第 2 回の review で足した、未回答）: docked の窓を閉じた時、他に窓が無くても docked mode を終える（次に開く app は floating で開く。2026-10-06 の「tablet mode は session の状態」と衝突する）。案: 他に窓が無い時は docked mode を保つ。既定: 終える（規則を 1 つに）。
- **N2**（未回答）: touch で bar の上の 10 px から下への drag は、docked の title の上でも pull でなく Wiseview（pull は 10 px より下を押す）。既定: そうする。
- **N3**（未回答）: 見えない desktop の docked の窓が閉じた時の代わりの案: その desktop の leave を保留し、その desktop へ移った時に docked mode を終える。既定: 保留しない（忘れるだけ、D1）。

## 8. 作らない物（正常系の外、plan/ws177/backlog-p2.md へ）

- 整列の時、client の最小の大きさが枠より大きい窓（client の描いた大きさのまま枠の左上に置く。重なりを避ける詰め直しはしない）。
- 大きさの固定の窓の整列（枠の中央に置く letterbox はしない）。
- 整列モードの間の画面の回転・解像度の変更（モードを終える、枠を計算し直さない）。
- 整列モードを再起動の後に覚えること。
- 鍵盤だけでの入れ替え。
- touchpad の gesture の変更（D3 の既定）。
- 上端の帯の流し直しで、全画面の app に press を届けること（S8）。
- 帯の中での長押し（apps bar の preview）（minor 8）。

## 9. design-reviewer の指摘（第 1 版）と反映

2026-10-07 の design-reviewer（第 1 版に対して）の指摘と、第 2 版で反映した場所。

| 指摘 | 内容（要約） | 反映 |
| --- | --- | --- |
| B1 | `kwl_glass_open_docked()` は map の前、持ち主をそこで付けると (b) で直ちに leave | §1.4: 持ち主は手順 B の観測（mapped・image あり）でだけ付ける。§1.3 の新しい窓の行 |
| B2 | `layout_match` を通らない前面化（FOCUS previous・corner・`window_to_desktop`・lower・flick） | §1.4 の手順 B（毎 tick の観測）。§1.3 に前面化・lower の行 |
| B3 | leave で `dock_owner[]` を全部消す、gone を desktop ごと、裏の desktop の持ち主の消滅 | §1.2 の 3、§1.4 の `dock_owner_gone[]` と手順 A の 1・3、§1.3 の見えない desktop の行、D1 |
| B4 | 上端の帯の遅らせは `bar_press()` でなく `kwl_glass_button` の先頭近く、流し直し、launcher の幅 40 | §3.1 |
| S1 | I3、dock の全経路で整列を終える | §1.2 の I3（`layout_set(DOCKED)` が終える）、§5.3 |
| S2 | 横の pull を bar の中で離すとまた dock、`pulled_rect` の補間、`pull_start_x` | §3.2 の 1・4（`drag_left_bar`） |
| S3 | 整列の適用で anim が 2 つ重なる、中心は leave の後の `restore_*` | §4.3・§4.4 の 1 |
| S4 | dock で開いた窓が同じ場所 | §1.2 の `window_float_quiet` と `restore_default` |
| S5 | 移動の開始の経路、`server->drag` と別の状態、CSD の枠 | §5.2、§4.2 |
| S6 | 整列モードの印、閉じた時の詰め直し、「新しい窓」は toplevel | §5.1 の印、§5.3、§7 |
| S7 | 連れて行く移動は docked のまま、Notes の角も | §1.3、§1.4 の手順 A の 4、D2 |
| S8 | 上端の帯を mouse にも効かせるか、全画面で 10 px | §3.1、§7 |
| S9 | Home の上の bar、左上の角の向き、帯と下向きの drag の優先 | §2.1・§2.2、§7 |
| S10 | ws142 の試験と決定を変える許可 | §1.5（Q1 が許可、2026-10-07） |
| S11 | Phase の依存 | §6 |
| S12 | 試験を足す | §1.5 の 3〜7、§3.3 の 3・4、§5.4 |
| M1 | `DESKTOPS`・`struct shell_rect` は shell.c の private | §1.4（`KWL_APPS_DESKTOPS`）、§4.2（`kwl_arrange_rect`） |
| M2 | `wiseview_progress` の向き、`wiseview_current` | §3.1（`wiseview_top`） |
| M3 | unmap の hook | §1.4: 消滅だけ `kwl_glass_forget`、unmap は観測の (b) |
| M4 | `window_undock` → `layout_leave` の再帰 | §1.2（`window_undock` は mode を変えない） |
| M5 | `window_float_quiet` が `anim`・`pull`・`click_docked` を消す | §1.2 |
| M6 | I2 を同じ app の docked の窓に合わせる | §1.2 の I2 |
| M7 | 整列の領域は落ち着いた値 | §4.2 |
| M8 | 上の段の枠への入れ替えで bar に入る | §5.2 |
| M9 | 整列中の desktop の切り替え、窓が他の desktop から来る | §5.3 |
| D の判定 | D3・D5 は既定にせず聞く、D2 の移動、D6 と帯の mouse | §7 で全部を案つきで聞く（D3・D5 は案を並べた） |

## 10. design-reviewer の指摘（第 2 版）と反映

2026-10-07 の design-reviewer（第 2 版に対して、blocking 2・should-fix 9・minor 10）。p002 の実装の途中で受けたので、実装にも反映した。

| 指摘 | 内容（要約） | 反映 |
| --- | --- | --- |
| BL1 | `window_to_desktop()` から直の `layout_follow()` は、連れて行く移動で「見えない desktop へ送った」と誤る | §1.4（直に呼ばない、tick に任せる）。実装は始めからこの形 |
| BL2 | leave で `fullscreen_docked = 0` にすると全画面を出た窓が docked の大きさの floating になる | §1.2 の 2（消さない）。実装は始めからこの形 |
| S-a | triple click が leave の経路から漏れ、表がコードと逆 | §1.2 の M4、§1.3 の行。実装済み（7ffe6036f） |
| S-b | `layout_leave` の署名の食い違い、pull の x・y | §1.2（x・y を持つ） |
| S-c | 後ろの窓の UNMAXIMIZE が docked mode 全体を終える | §1.2 の M4。実装済み（436594e02） |
| S-d | CSD の docked の窓は bar から pull できない | §3.2 の 4（client の move の要求で leave して move、p003） |
| S-e | 整列の終わりの検出が消滅以外に無い、D7 との衝突 | §5.3 の検出（`arrange_follow`、map の時の `map_order`） |
| S-f | 試験 3 の `windows` の行が出ない、`mode=` の項 | §1.2 の 5（要約を dock・持ち主の変化でも出す、`mode=` を足す）、§1.5 の 3。実装済み |
| S-g | Home の間に dock-hidden の窓が層に描かれる | §2.1（Home は overview でない）。実装済み（436594e02） |
| S-h | 帯と bar_cover・`kwl_glass_edge_motion` | §3.1 |
| S-i | D3・D5 の既定と §9 の食い違い | ユーザーが回答（§7、D5 は既定と違う「pill のどこでも」） |
| minor 1 | 観測の前の取りこぼし | §1.4（`window_dock` でその場で持ち主に）。実装済み |
| minor 2 | 2 本指の flick は docked の窓に効かない | §1.3・§1.5 から外した |
| minor 3 | 見えない desktop の最小化の要求は起きない | §1.3 |
| minor 4 | owner の結果の種類 | 実装の `KWL_LAYOUT_OWNER_MOVED` が「持ち主を表示中の desktop へ移す」に当たる（変更なし） |
| minor 5 | quiet の順 | §1.2 の 6。実装済み（2 回に分けて回す） |
| minor 6 | `HOME_LAUNCHER_WIDTH` は無い | §3.1（新しく定義） |
| minor 7 | log の名前 | §3.3（`KWL GLASS moved`、`KWL HOME close via=pull-down`）。実装済み |
| minor 8 | 帯の細部 | §3.1・§8 |
| minor 9 | swap の後始末 | §5.3 |
| minor 10 | メニューを開く時 | §4.1（release） |
| 漏れの質問 | 他に窓が無い時、CSD の pull、帯と pull の優先、見えない desktop の保留 | §7 の N1〜N3、CSD は §3.2 の 4 |
