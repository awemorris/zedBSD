<!-- awesome-plan project=zedbsd record=ws352-p001 -->

# ws352-p001: 解析（idle の時に起きる thread と timeout の一覧、各々が何を待つべきか）

Status: in-progress（一覧の確かめ済み、ユーザーの判断待ち。2026-11-13 P1、q2046-i02。cleared は Q1）
Parent: [WS352](../ws.md)

## 測り方

- 診断の kernel（試験だけ、`plan/ws352/tests/poll-diag.patch` = WS351 の wake-reason.patch に、thread ごとの poll の数・timeout で返った数・要求の待ち・空振りの起床の数と、kernel の poll の通知の数を 10 s ごとに klog）。
- 5330 で `plan/ws352/tests/5330-idle.sh`（/tmp/i915-hw.lock、~/boot-zedbsd.sh の一回だけの起動、watchdog=600、終わりに p4 を戻して sha256 照合）。
- survey0（2026-11-13 16:42〜16:47、診断の kernel 729f27810252c5eb = 直す前の tree、NVMe の userland 1.0.0-beta2+gd00e071（11-02））。成果物は `build/hw5330/ws352/survey0/`（git の外）。

## idle 10 s の poll（survey0、3 窓とも同じ形）

| pid/tid | 何か（nfds・待ち・code から） | timeout で返った数 /10 s | 要求した待ち |
| --- | --- | --- | --- |
| 7/49 | compositor の main loop（listener・client 3・wake・…、nfds 6） | 899 | 11 tick（10 ms） |
| 7/71 | compositor の input thread（wake＋device 3、nfds 4。touch pad がある間 `INPUT_PAD_TICK_MS 10`） | 909〜910 | 11 tick（10 ms） |
| 7/73・74・75・77 | compositor の daemon link の thread（audiod・networkd・bluetoothd・volumed・phone の 5 本、nfds 2/1、`KL_DAEMON_LINK_TICK_MS 250`） | 各 40 | 251 tick |
| 7/76 | 同（nfds 1、immediate 40 回も） | 40 | 0〜251 |
| 18/79 | settings --welcome（Welcome の Home は network の詳細を 1 s ごと） | 10 | 〜999 |
| 17/78 | files --desktop（folder の確かめ 2 s） | 5 | 2001 |
| 5/46・6/47・20/81・22/83・23/85 | daemon（ps は名前を出さない。22 は _bluetooth の bluetoothd） | 10〜2 | 1001〜9999 |
| （全体） | kernel の poll の通知（`poll_notify`） | 2206〜2255（約 220 回/s） | — |

- 空振りの起床（rescans）: どの poll も 10 s に 1700〜2000 回。kernel の poll は全部が 1 つの待ち行列（`src/kern/poll.c` の channel）で待ち、どの fd の変化でも `poll_notify()` が全員を起こし、各々が自分の fd を全部見直して寝直していた。約 220 回/s の通知 × 約 20 の poller ≒ 4000 回/s の無駄な起床。
- timer の割り込み（同じ kernel、10 s 平均）: CPU 0〜3 が 447〜1000/s、他 105〜298/s。
- その他（WS351 run6 の neardiag と同じ）: compositor の usync の 100 ms（render thread の `RENDER_WAIT_MS 100` の cond の timedwait）、kernel の 100 ms の sleeper 2 つ（kernel の thread、範囲の外）。

## 各々が何を待つべきか（code）

本来の形: 事象（fd・thread の wake・signal）が来るまで block し、期限は本当に要る時だけ、それも次の要る時刻まで。

### compositor の main loop（`userland/desktop/wayland/main.c` の `event_loop`）

- 今: `poll(..., 10)`（固定。`kwl_compose_waiting` は常に 0 で 2 ms の枝は死んでいた）。10 ms ごとに全部の tick を回し、時間で進む仕事も fd に無い状態も「次の pass」で拾っていた。
- 時間で進む仕事（期限）と、fd の無い状態の一覧（調べた結果）:
  - 期限: input の scan（2 s、device の事象の後 2 s は 100 ms）、ping の無応答（ping_ms+5 s）、titlebar の key repeat（repeat_next）、IME の再起動（restart_ms）と応答の待ち（watch_ms+500）、clipboard の read/write の打ち切り（2 s）、drag の release の判断（dnd_release_deadline）、fatal client の flush（2 s）、sleep と画面消灯（since+limit/2・limit、pause、休止の後の rest）、idle の lock（lock_input_ms+lock_idle_ms）、時計の分（実時刻の次の分）、lock 画面の card（30 s）と security key の owner（1 s）・grant（checking）、apps bar の hover（400 ms）・leave（300 ms）、Home の起動の忘れ（30 s）、corner の Notes の起動（5 s）、OSK の手書き（600 ms）、network の詳細（1 s）・起動の猶予・switch の待ち、volume の送り（50 ms）、sysmon の数え直し（object がある間 250 ms）と sample、machine の読みの打ち切り（10 s）、phone の held・sync、printer の file の確かめ（1 s）、desktop の再起動（2 s）、layer cache の静けさ（input+500・commit+500・frame+100・bake+1000）、frame の pacing（frame_done+frame_wait）、rotate の hold（1 s）、scanout の pointer の静止（2 s）と lend の拒み（5 s）、glass の blur の hold（200 ms）、displays の保留、hotplug の fence（250 ms、下）、test の shot（3 s）。
  - 動いている間は毎 frame 見る物（animation・指・drag）: desktop の slide、window の animation、Wiseview、Home の開閉・page・icon、通知の board、log の fade、power dialog、OSK の slide・contact、corner の contact・hint、touch の指の追従・titlebar の 2 本指、band の長押し、arrange の glide、spring-loading、sleep の pending、greeter の電源の spinner。これらは frame の完了（render thread の wake）で次の pass が来るが、pacing と終わりの判定のため「動いている間は 10 ms ごと」を残すのが安全。
  - fd が poll に無かった物: sessiond の control の fd（`kl_backend_session_tick` が非 blocking で読む。sleep の答え・lock・logout が来る）、clipboard の pipe、printer の daemon の socket、zedBSD の audio stream の socket（main thread で読む）、test の shot の listener。
  - thread が wake pipe を書かずに結果を置く物: machine の読み（machine-shell.c）、壁紙の loader（glass.c）、cursor thread の pin の解放（cursor-plane の copy）、shot の送り。
  - signal: 子の終わり（`waitpid(WNOHANG)` を毎 pass: Home・desktop・IME）、SIGTERM/SIGINT/SIGHUP は flag だけ（raw の `pthread_create` の thread 7 本は signal を block していない）。
  - 本来の形: 上の期限の最小まで待ち（無ければ -1）、fd を poll に足し、thread と signal は wake pipe を書く。描く物が scheduler の後に増えたら直ちに次の pass。
- 残る周期: input の scan（2 s。zedBSD は /dev/system の INPUT の事象で即座にも scan する）、display の hotplug の fence（250 ms。libvulkan の `vkRegisterDeviceEventEXT` の fence は kernel の topology の sequence を問い合わせて signal する形で、待てる事象が無い。kernel に display の system event か topology の待ちが要る＝範囲の外、提案）。

### compositor の他の thread

- input thread（`input-thread.c`）: touch pad がある間 10 ms。`kwl_touchpad_tick` の仕事は tap の後の drag の待ち（`TAP_DRAG_MS 300`）の終わりだけで、それも次の touch の始まり（`touch_begin`）が期限を見るので出力は変わらない。本来: tap の期限（`tap_deadline_ms`）まで、無ければ -1。pointer の加速・key repeat は input thread に timer を持たない（repeat は client と titlebar）。
- daemon link（`libkeiland-backend-zedbsd/daemon-link-zedbsd.c`）: 250 ms の tick。5 つの protocol（audio・volume・network・bluetooth・phone）の `poll_fill` は自分の期限（再接続・scan・answer）を既に下げている。本来: protocol の次の期限まで、無ければ -1。bluetooth は「scan が他の理由で止められている」間も期限が過ぎた scan の時刻を入れ、timeout 0 で回り続ける（pairing 中などの空回り）→ scan が始められる時だけ期限に入れる。
- render thread（`render-thread.c`）: 仕事が無い時 `pthread_cond_timedwait` 100 ms。main thread の変更は全部 cond を signal する。100 ms の目覚めの仕事は「先回りの image の acquire」を試し直す事だけ（frame が要る時は want の側で取る）。本来: 無期限の cond の wait。
- cursor thread: idle は無期限（fence の未完了の 5 ms・acquire の 1 ms は動いている時だけ）。

### client

- files --desktop: 2 s（folder の確かめ、`tab->checked_at + 2000`）。他に 50 ms 未満になるのは thumbnail・検索・device の点滅・scroll bar・touch の間だけ。
- settings --welcome: 1 s（Welcome の Home は network の詳細を毎秒）。
- keiland-ime: -1（key repeat の間と保存の 180 s だけ有限）。
- 測った 2〜22 ms の短い待ち（WS351 run5/6 の neardiag）は client の timeout ではなく、kernel の poll の共有の channel による空振りの起床で残り時間が短くなった寝直し（survey0 の rescans で確かめた）。

### kernel の poll の共有の channel（Q1 の指示で範囲に入れた）

- 今（`src/kern/poll.c`）: 全部の poll が 1 つの待ち行列で待ち、83 箇所の `poll_notify()`（pipe・socket・unix socket・evdev・GPU・tty・audio・…）がどの変化でも全員を起こす。起きた poll は自分の fd を全部 scan して何も無ければ寝直す。userland の timeout を直しても、誰かが pipe に 1 byte 書くたびに system の全 poller が起きる。
- 誰が notify するか（survey0 の idle の 220 回/s）: idle の compositor の 10 ms の pass ごとの wake pipe・input thread への pipe・client への socket の書き。他の thread の起床がまた notify を生む。
- 本来の形: 待つ物（fd の後ろの object）ごとに起こす。変化を起こした側は「何が変わったか」を名指しし、その object を待つ poll だけが起きる。

## 直し方（p002 で実施）

- kernel: poll の各 waiter が scan の間に自分の待つ object を集め（file type の poll op が `poll_wait_object(object)`）、`poll_notify_object(object)` はそれを待つ waiter だけを起こす。名指しの無い fd を含む poll と、名指しの無い通知（`poll_notify()`）は従来どおり全員。変換するのは idle の desktop が待つ pipe・socket（unix を含む）・evdev。GPU と tty は従来どおり（理由は p002）。
- compositor: `kwl_due()`（期限の集計）・`kwl_due_soon()`（動いている間の 10 ms）・`kwl_watch()`（非 blocking で読む fd を poll に足す）、SIGCHLD と停止の signal は wake pipe、raw の thread は signal を block、wake しない thread は wake を書くか動いている間だけ 10 ms。Linux・FreeBSD の backend は poll に無い fd を毎 pass 読む所が多い（D-Bus・ALSA・wpa・logind・audio の pump）ので、`kl_backend_wakes()` が 0 を返し、main loop は従来の 10 ms のまま。
- input thread・daemon link・render thread: 上の「本来」のとおり。
- 残す周期（提案、範囲の外）: display の hotplug の 250 ms（kernel の事象が要る）、files の folder の確かめ 2 s と settings の Welcome の 1 s（client の機能の作り。変化の通知の仕組みが要る）、daemon の 1〜10 s。

## compositor の timeout に依存している所の全部（2026-11-13 ユーザーの依頼で追加）

対象: `userland/desktop/wayland/` と、zedBSD で使う `libkeiland-backend-zedbsd/`・`libkeiland-backend/` の共通部。行番号は branch（508c992109 の tree、直し中の物を含む）、括弧の中は直す前（基点 main 6aff283baa）。

規則（2026-11-13 ユーザーの前提）: 周期の timeout は置かない。時刻の締め切りは予定がある間だけその時刻まで待つ。状態の変化は全部事象で知らせる。直す時は新しい起こし方を作らず、導入済みの仕組みを使う。

区分:
- **名残り**: 導入済みの仕組みで既に知らされる（または知らせる口が既にある）のに、単一 thread の事象駆動の時の timeout が残っている。導入済みの仕組み: main の **wake pipe**（`server->wake`、signal・backend の thread・machine・cursor の失敗が書く）、**lane**（`lane.c`、仕事の一覧と終わりの一覧、終わりで wake pipe に 1 byte。io-file・io-device・display・blur・present）、**render の message の列**（`render-thread.c`、render thread → main、wake pipe）、**render の `changed` の cond**（main → render）、**cursor の box の `changed`・`answered` の cond**、**input の box**（input thread → main は wake pipe）、**daemon link の wake pipe**、**font の worker**（wake pipe）。
- **道が無い**: 状態の変化を知らせる道がまだ無い（hotplug など）。
- **締め切り**: 本当に時刻の締め切りが要る（animation の frame は除く。key repeat・tap・画面の消灯・応答の打ち切りなど）。予定がある間だけ、その時刻まで。
- 「済」は branch で直した物。

### 1. thread ごとの待ち

| thread | 場所 | timeout（branch／直す前） | 何を待つ・見張る | 区分と理由 | 直す形 |
| --- | --- | --- | --- | --- | --- |
| main（event loop） | main.c:1200（1110） | 下の 2. の期限の最小、無ければ -1／**固定 10 ms** | listener・client・wake pipe・backend の fd、時間で進む仕事 | 直す前の 10 ms は名残り（thread の結果は wake pipe で来る）。branch は 2. の各行による | 2. の名残りと道が無い物を外せば idle は -1 |
| input | input-thread.c:273（267） | tap の期限まで、無ければ -1／**touch pad がある間 10 ms**（`INPUT_PAD_TICK_MS`） | evdev の fd・wake pipe。tap の後の drag の待ち（`TAP_DRAG_MS` 300） | 締め切り（tap の直後だけ）。直す前の 10 ms は名残り | 済 |
| main が input の受領を待つ | input-thread.c:430（424） | 100 ms の上限（`KWL_INPUT_DETACH_WAIT_MS`） | device を外す時の受領（input の box の `acknowledged` の cond） | 締め切り（応答が来ない故障の打ち切り、detach の時だけ） | そのまま |
| render | render-thread.c:675（668） | 無期限／**100 ms**（`RENDER_WAIT_MS`） | main の変更（`changed` の cond。main の変更は全部 signal する）。100 ms の目覚めは先回りの acquire の試し直しだけ | 直す前は名残り（`changed` の cond） | 済 |
| render（描画中） | render-thread.c:242〜317・738〜740、compose.c:4927 | job が飛んでいる間 1 ms（`KWL_RENDER_POLL_NS`）、acquire 50 ms | GPU の job の完了・swapchain の image と、その合間の main の要求 | 道が無い（GPU の完了〈libvulkan の notification の fd〉と main の `changed` の cond を一つの待ちで受ける口が無い）。idle では走らない | 案（描画中だけで idle の電力に効かない。この WS では触らない） |
| main が render の message・park を待つ | render-thread.c:433・471（429・467）、display-thread.c:303 | 呼び手の上限（display の切り替えの間 100 ms ごと、最大 10 s） | display の切り替え・park の答え（render の `answered` の cond） | 締め切り（切り替えの間だけの打ち切り） | そのまま |
| render の再試行 | render-thread.c:728・854（721・847） | `usleep(1000)` | memory 不足などの失敗の再試行 | 締め切り（失敗の時だけ） | そのまま |
| cursor | cursor-thread.c:408（401） | 変更が無ければ無期限。pin が残る間 5 ms（`CURSOR_PIN_POLL_US`）、refresh の終わりまで（hold）、再試行 1 ms（`CURSOR_RETRY_US`） | main の box の変更（box の `changed` の cond）、main が copy の pin を外したか | 無期限は済。hold は締め切り（動いている間）。pin の 5 ms は**道が無い**（i02 で訂正: pin を外すのは main ではなく cursor thread 自身で、copy の GPU の fence の完了を `cursor_unpin_done`〈cursor-thread.c の `vkGetFenceStatus`〉で見て外す。fence と box の `changed` の cond を一つで待つ口が無い）。再試行 1 ms は道が無い（swapchain の image が空いた事を知らせる道が無い。動いている間だけ） | 案（client の cursor を描いた直後だけ。idle では pin 0 で走らない） |
| cursor の copy・main が cursor の park を待つ | cursor-thread.c:302・484・346（295・477・339） | 100 ms の上限 | copy の fence・park の答え（box の `answered`） | 締め切り（打ち切り） | そのまま |
| scanout・compose の fence と acquire | scanout.c:1069・1074（1065・1070）、compose.c:5124・5180（5138・5194） | 100 ms・1 s・呼び手の上限 | 直接の scanout の image、片付けの fence | 締め切り（打ち切り、動いている間） | そのまま |
| daemon link（audiod・volumed・networkd・bluetoothd・phone の 5 本） | daemon-link-zedbsd.c:293（268） | protocol の次の期限、無ければ -1／**250 ms**（`KL_DAEMON_LINK_TICK_MS`） | daemon の socket・link の wake pipe、再接続・scan・答えの期限 | 直す前の 250 ms は名残り（socket と link の wake pipe）。branch は締め切り（再接続・scan の期限）。bluetooth の止められた scan の空回り（timeout 0）は名残り。**漏れ（i02 で追加）**: network は networkd への要求・scan の答えを待つ間 10 ms（network-zedbsd.c:638・639 `NETWORK_ANSWER_LOOK_MS`）。答えの fd を poll に入れず `network_readable()`（1075〜）で非 blocking に見る。理由の comment（自分の側の書きの shutdown で poll が POLLERR を返す）は kernel で既に直っている（unix-socket.c:2569〜2574、socket.c:1174〜1176 の BUG-149）ので名残り | 250 ms は済。network の 10 ms は答えの fd を `network_poll_fill` に足す（未実施、要求の間だけ） |
| sessiond の答え | session-zedbsd.c:185（185） | 20 s の上限 | 起動の時の sessiond の答え | 締め切り（起動の時だけ） | そのまま |
| machine | machine-shell.c（detached） | timeout 無し | /dev/system の読み | 直す前は main が 10 ms ごとに結果を見ていた（名残り） | 済（終わりに wake pipe） |
| glass の loader・prefetch、settings-store の writer、print の writer、font、lane、shot-send、手書き | glass.c:513・2194、settings-store.c:437、print.c:2048、glyph-worker.c:259、lane.c:241・327、shot.c:900、keyboard-hand.c:675 | 無期限の cond の wait | 仕事 | — | 終わりを main に知らせる所は 2.（loader・shot-send） |
| audio の pump（Linux・FreeBSD） | pump.c:860 | 動いている間 10 ms | ALSA・OSS | 範囲の外（zedBSD は audiod の socket を main が poll） | — |
| wpa（Linux） | network-wpa.c:616 | 答えの期限 | wpa_supplicant | 範囲の外 | — |

### 2. main loop の期限（`kwl_due`・`kwl_due_soon`。直す前は全部が 10 ms ごとの pass で時刻を比べていた）

| 場所 | 期限 | 何を見張る | 区分と理由 | 直す形 |
| --- | --- | --- | --- | --- |
| output-switch.c:171・175・177 | **250 ms**（`OUTPUT_HOTPLUG_MS`）、出力を失った時 10 ms | display の hotplug の fence（`vkGetFenceStatus`） | **道が無い**（fence に待てる事象が無い。idle の main loop の timeout 55/10 s のうち 40） | hotplug を待つ名前の付いた thread が fence を待ち、signal で wake pipe（導入済み）。libvulkan の event の fence の待ち（今は 1 ms ごとに見る）を display node の POLLPRI（`GPU_DISPLAY_EVENTS` の ACK の後の topology の変化、kernel にある）で block する |
| main.c:1026 | 2 s、device の事象の後 2 s は 100 ms | 新しい evdev の node を seat に足す scan | zedBSD は**名残り**（/dev/system の INPUT の事象は backend の fd で既に来て、即 scan する〈events-zedbsd.c:207〜209 → backend-host.c:240〜253 が `input_scan_time = 0`〉。周期は保険）。事象の後 2 s の 100 ms（`KWL_INPUT_SETTLE_SCAN_MS`）は**道が無い**（事象と node の出現の差を知らせる物が無い。事象の後だけ） | zedBSD では事象だけ。事象の無い backend（Linux・FreeBSD）は残す |
| printers-shell.c:278・280 | 1 s | printers の settings file の他の session の変更（printers の object がある間） | **道が無い**（file の変更の通知が zedBSD に無い） | 案（範囲の外: file の変更の通知、または printd が settings を持ち変化を push） |
| network.c:349 | 1 s | network の詳細の読み直し（network の view が開いている間） | **道が無い**（networkd が詳細の変化を知らせない。未確認） | 案（networkd の通知）。view が開いている間だけ |
| sysmon.c:240・250 | 250 ms・sample の次 | 負荷の表示の sample（sysmon の object がある間） | 締め切り（購読者がいる間の sample の予定） | そのまま |
| cursor-plane.c:475 | 10 ms | cursor thread が copy を手放したか | **名残り**（cursor thread は wake pipe を持つ〈`wake_main`、今は失敗の時だけ書く〉） | 手放す時に `wake_main` を書く |
| glass.c:877 | 10 ms | 壁紙の loader の終わり（flag だけ） | **名残り**（wake pipe） | loader が終わりに wake pipe を書く |
| shot.c:276 | 10 ms | 画素の送り（shot-send）の終わり | **名残り**（wake pipe） | shot-send が終わりに wake pipe を書く |
| system.c:562 | 10 ms | system bar の要求に止められた鍵の保存の一歩の再試行 | **名残り**（止めた要求の終わりは lane の終わりで main に来る） | 止めた要求が終わった pass で進める（期限を外す） |
| system.c:2613 | 10 ms | sessiond が EBUSY の間の enrolled の問い | **名残り**（sessiond の fd は `kl_backend_wait()` で poll に入っている） | sessiond の答えで問い直す |
| audio-stream.c:252 | 10 ms | 待てない audio の stream（Linux・FreeBSD） | zedBSD は不要（socket を poll） | — |
| main.c:1191 | 10 ms | 事象で起こさない backend（Linux・FreeBSD） | zedBSD は不要 | — |
| main.c:1597 | 10 ms | 足す fd の表が満ちた時 | 不要（起きない保険） | — |
| main.c:1115 | 10 ms | 去った client の buffer を数個ずつ解放（残っている間） | 名残り（残りは次の pass で直ちにでよい。入力を先に見る順は pass の中で保てる） | 要検討（idle では残りが無い） |
| main.c:1120・1135・1195 | 0（直ちに） | 解放できる buffer、送り終えた fatal client、scheduler の後の描く物 | — | — |
| animation（`kwl_due_soon` 10 ms、動いている間だけ）: apps-bar.c:666、greeter.c:1038、keyboard.c:1063・1066・1074、arrange-shell.c:651、corner.c:473、home.c:1538、notify-log.c:166、power-dialog.c:302、notify-popup.c:179、touch.c:625・632、shell.c:3493、sleep.c:95 | 10 ms | slide・fade・指の追従・spring・spinner・sleep の保留 | 名残り（描いた frame の終わりは render の message で来て次の pass になる。frame の間隔は display.c:255 の pacing）。ただし描かずに時刻だけで進む物（sleep の保留・spring の判断）は締め切り | 要検討（idle では走らない） |
| main.c:1137 | fatal_time+2 s | fatal client の flush の打ち切り | 締め切り | そのまま |
| main.c:1189 | 試験の上限 | `--timeout` | 締め切り | そのまま |
| main.c:1577（shell.c:3496・greeter.c:995 が毎 pass 呼ぶ） | 実時刻の次の分（実時刻が読めない時 main.c:1570 の 1 s） | 時計 | 締め切り。ただし画面の消灯の間も毎分起きる（要検討: 時計が見えない間は要らない） | そのまま（消灯の間は案） |
| shell.c:8161（i02 で追加） | 10 ms | band の長押し（押している間） | 締め切り（長押しの閾値の時刻で足り、10 ms ごとは要らない） | 案（`band_since_ms` ＋閾値まで。押している間だけ） |
| media-library.c:214（i02 で追加） | 10 ms | media の helper の pipe と終わり（helper が走る間） | 名残り（pipe は `kwl_watch`、子の終わりは SIGCHLD の wake pipe〈main.c:491〜503〉で知れる） | 案（helper が走る間だけ） |
| handoff.c:115 | sessiond の答えの期限 | sleep の答え・logout | 締め切り | そのまま |
| sleep.c:464・473・493・502 | since+limit/2・pause_until・rest・since+limit | 画面の消灯・sleep の始め・休止の後 | 締め切り | そのまま |
| shell.c:3401 | lock_input+lock_idle | idle の lock | 締め切り | そのまま |
| titlebar-shell.c:780・805 | repeat_next | title bar の key repeat | 締め切り | そのまま |
| apps-bar.c:687・696 | 400 ms・300 ms | apps bar の hover・leave | 締め切り | そのまま |
| keyboard.c:1116 | 600 ms | OSK の手書きの認識 | 締め切り | そのまま |
| greeter.c:1026・2753・2761 | 30 s・security key の owner・grant | lock の card・key の確かめ | 締め切り | そのまま |
| toplevel.c:499 | ping+5 s | client の無応答 | 締め切り | そのまま |
| input-method.c:309・311 | restart・answer+500 ms | IME の再起動・答えの打ち切り | 締め切り | そのまま |
| clipboard.c:219・223 | 2 s | clipboard の read・write の打ち切り（fd は poll） | 締め切り | そのまま |
| data.c:435 | dnd_release_deadline | drag の release の判断 | 締め切り | そのまま |
| desktop.c:825 | 2 s | desktop の再起動の間隔 | 締め切り | そのまま |
| home.c:1534、corner.c:475 | 30 s・5 s | 起動の忘れ・Notes の起動の待ち | 締め切り | そのまま |
| network.c:359・1858 | 30 s・switch_until | 起動の猶予・切り替えの待ち | 締め切り | そのまま |
| notify-log.c:168 | used+10 s | 通知の log を閉じる | 締め切り | そのまま |
| displays-shell.c:254 | 3 s | displays の保留の答え | 締め切り | そのまま |
| output-rotate.c:639 | 1 s | rotate の hold | 締め切り | そのまま |
| scanout.c:199・919 | 2 s・5 s | pointer の静止・lend の拒み | 締め切り | そのまま |
| layer-cache.c:248 | 静けさ（input+500・commit+500・frame+100・bake+1000） | layer の cache の焼き | 締め切り | そのまま |
| display.c:255 | frame_done+frame_wait | frame の pacing | 締め切り | そのまま |
| glass.c:892 | 200 ms | blur の hold | 締め切り | そのまま |
| volume.c:194 | 50 ms | volume の送りの間隔 | 締め切り | そのまま |
| machine-shell.c:251 | 10 s | machine の読みの打ち切り | 締め切り | そのまま |
| shot.c:287 | 3 s | shot の答えの打ち切り | 締め切り | そのまま |
| phone-shell.c:549・555 | 10 s・150 s | phone の held・sync の待ち | 締め切り | そのまま |
| system.c:1535 | scanning_until | network の scan の表示 | 締め切り | そのまま |

### 3. compositor の外で、compositor の待ちに効く物

- libvulkan の `vkWaitForFences`（sync.c の `VULKAN_SYNC_POLL_NS` 1 ms）: notification の無い fence（display の event の fence）は 1 ms ごとに見る（周期の timeout）。hotplug の fence は display node の POLLPRI（ACK の後の topology の変化）で block できる。
- kernel の GPU の poll（gpu.c `gpu_poll`）は object を名指ししない。POLLPRI だけを待つ poll は session を名指しすれば、system の名指しの通知では起きない（GPU の通知は全部が名指し無しなので取りこぼさない）。
- bt-hci（`src/drivers/generic/bt-hci.c`）は名指しが無く、bluetoothd の poll が system の全部の名指しの通知で起きていた（diag4: rescans 1077/10 s、every 1077）。branch で名指しにした（controller）。
- libvulkan の hotplug の待ちの作りかけ（b05187933a、下の「未 commit だった変更」）。

## 一覧の確かめ（2026-11-13、q2046-i02）

前の attempt（i01）の一覧を code と照らし直した。行番号は b05187933a の tree。依頼の範囲（2026-11-13 ユーザーの認識の要点）: timeout を使う所の一覧と、すでに入れた thread 化と事象の仕組みでそれが要らなくなっているかの確かめ。新しい直しは進めない。

### 確かめで直した所

- cursor の pin の 5 ms（表 1）: 「main が pin を外すので box の `changed` で足りる＝名残り」は誤り。pin を外すのは cursor thread 自身で、copy の GPU の fence の完了を見て外す（cursor-thread.c の `cursor_unpin_done`）。→ 道が無い（GPU の fence と cond を一つで待つ口が無い）。
- daemon link（表 1）: network だけ、networkd の答えを待つ間 10 ms の見張りが残っていた（`NETWORK_ANSWER_LOOK_MS`、network-zedbsd.c:638）。理由の kernel の振る舞いは既に直っている → 名残り。
- 表 2 の漏れ: shell.c:8161（band の長押し、10 ms）、media-library.c:214（media の helper、10 ms）、main.c:1570（時計の分の予備の 1 s）、input の scan の事象の後の 100 ms。
- 時計の分（main.c:1577）は画面の消灯の間も毎分起きる（idle で残る周期の一つ。表 2 に注記）。
- 他の行（main.c:1200、input-thread.c:273、render-thread.c:675、daemon-link-zedbsd.c:293、session-zedbsd.c:185、表 2 の期限の各行）は code のとおり。

### 4. compositor の外: libvulkan

| 場所 | timeout | 何を待つ | 区分と根拠 | 直す形 |
| --- | --- | --- | --- | --- |
| sync.c:1704〜1708（`VULKAN_SYNC_POLL_NS` 1 ms、`vulkan_sync_pause` sync.c:1014） | 1 ms ごと | notification の無い fence（未提出・display の event・記録の無い fence） | **道が無い**（kernel の job を持たない fence には起こす物が無い）。提出した fence は `sync_notification_wait`・`_poll` で事象を待つ（1693〜1695） | hotplug の fence だけ b05187933a で display node の POLLPRI に（下）。REFRESH の event の fence は `GPU_DISPLAY_REFRESH` の待ちがある（未実施、idle では使わない） |
| wsi-display-nodes.c:260〜（b05187933a、未使用） | 呼び手の上限まで、無ければ無期限 | display node の topology の次の変化 | 下の「未 commit だった変更」 | compositor の側（下）が要る |

### 5. client（idle の desktop で動く物）

| 何 | 場所 | timeout | 何を待つ | 区分と根拠 | 直す形 |
| --- | --- | --- | --- | --- | --- |
| files --desktop | files/ui.c:765〜769（`fm_ui_due`）、main.c:1041 | folder の最後の確かめ＋2 s | 表示の folder の変化 | **道が無い**（kernel にファイルの変化の通知〈kqueue の vnode・inotify の類〉が無い。src/kern に無いことを確かめた） | 案（範囲の外: 変化の通知の仕組み） |
| files | ui.c:701〜704 | 20 ms | thumbnail・picture の子 process（走る間） | 名残り（子の pipe を `preview_poll` が非 blocking で見る。pipe は wait の fds〈window.c:426〜433 の `fds`〉に足せる） | 案（作る間だけ） |
| files | ui.c:705〜730、main.c:1045 | 0〜50 ms・33・16 ms | 検索・task・入力の直後・device の点滅・scroll bar・drag・touch | 締め切り・animation（操作の間だけ） | そのまま |
| settings（--welcome・Home・Sharing・network の page） | settings/network.c:209・295（`NETWORK_DETAILS_MS` 1 s） | 最後の問い＋1 s | network の詳細（address・activity・DNS・保存の network） | Welcome・Home・Sharing が見るのは address だけ: **道が無い**（networkd の watch が送る `kl_network_state`〈keiland_system.h:141〉に address が無く、詳細は問いの答え〈`KL_SYSTEM_CHANGED_DETAILS`〉だけ）。network の page の activity は締め切り（表示の間の sample） | 案（範囲の外: address の変化を state に入れる） |
| settings | main.c:797・807〜（`se_ui_wait` ほか） | About の分・kinetic | 時計・scroll の慣性 | 締め切り（表示・動きの間） | そのまま |
| keiland-ime | ime/main.c:470〜477 | key repeat・保存（180 s）の時だけ、無ければ -1 | compositor・wake | 締め切り（idle は -1） | — |
| libkeiland の clipboard・primary の受け | ui/clipboard.c:1185、ui/primary.c:183 | 締め切りの中を 100 ms 刻み | 貼り付けの pipe | 締め切り（貼り付けの間だけ。100 ms の刻みは要らないが害は無い） | そのまま |
| libkeiland の起動の読み | instance.c:724（`INSTANCE_READ_MS` 500） | 500 ms | 起動の答え | 締め切り（打ち切り） | そのまま |

前の一覧の「client の 2〜22 ms の短い待ちは kernel の共有の channel の空振りの寝直し」は survey0 の rescans と合う（code の timeout は上の 2 s・1 s・-1）。

### 6. daemon

| daemon | 場所 | timeout | 何を待つ | 区分と根拠 | 直す形 |
| --- | --- | --- | --- | --- | --- |
| sessiond（session の間） | sessiond/session.c:219〜279 | **1 s**（交換の間 100 ms） | session の子の終わり（毎 pass `waitpid(WNOHANG)`）、control・交換・sleep・seat の fd | **名残り**（子の終わりは control の fd の HUP で知れる。SIGCHLD の handler は無い〈main.c:197〜201 は TERM・INT・PIPE だけ〉。seat の事象の fd は poll に入っている） | 案: 1 s をやめ、SIGCHLD を EINTR か self-pipe で（範囲の外の daemon） |
| sessiond（greeter の間） | sessiond/greeter.c:186〜189 | 1 s（交換の間 100 ms） | 同じ | 名残り（同じ） | 同じ |
| bluetoothd | bluetoothd/main.c:749〜751 | 期限、ただし**最大 1 s**（-1 も 1 s） | HCI・client・system の事象と期限 | 1 s の上限は**名残り**（`btd_timeout()`〈1172〜〉が期限を全部集めている）。seat の持ち主の見直し 5 s（`BTD_SEAT_CHECK_MS`、controller が ready の間）は道が無い（未確認: 持ち主の変化を知らせる事象を見つけていない）。controller の無い間 2 s（`BTD_RETRY_MS`）は締め切り | 案（範囲の外） |
| audiod | audiod/main.c:178〜182、device.c:180〜193 | mmap の device は **1 s**、write の device は period の半分、device 無しは period ごと | device・client・route の期限 | mmap の 1 s は名残り（frontier は dsp の POLLOUT〈audio.c の period の割り込みが名指しで通知〉で来る。DMA が idle でも回るなら period ごとに起きるが、それは timeout ではない。未確認）。write の device の半 period は道が無い（dsp に「残りが N 以下」の知らせが無い）。device 無しの period は締め切り（device の代わりの時計。鳴らす物が無い間も回る＝要検討） | 案（範囲の外） |
| networkd | networkd/main.c:740〜741、3013〜3059 | 期限（無線の再 scan・DHCP・IPv6・有線の retry・確かめの rollback）、control の受けの途中は 20 ms | 要求・route・IPv6 の事象 | 期限は締め切り。20 ms は名残り（受けの途中の control の fd を poll に足せば足りる。受けの途中だけ） | 案 |
| volumed | volumed/main.c:185〜198 | 期限（事象の後の settle 300 ms）、無ければ -1 | 事象・client | 締め切り（idle は -1） | — |
| printd（印刷の daemon） | printd/main.c:148（`PD_TICK_MS` 1 s） | 1 s | job の終わり・60 s の idle の通知 | 名残り（job の thread の終わりは pipe で、idle は最後の仕事＋60 s の期限で足りる）。走るのは backend が起こした間（未確認） | 案 |

### 7. kernel の周期の sleeper（範囲の外、提案だけ）

survey0 の neardiag（診断の kernel 729f27810252c5eb、`build/ws352/k/vmunix-diag` の symbol で引いた）:

| thread | 場所 | 周期 | 何を待つ | 区分と根拠 |
| --- | --- | --- | --- | --- |
| pid 0 tid 8・9（entry ffffffff80437283 = `xhci_port_worker`、5330 の xHCI 2 つ） | src/drivers/pci/pci-xhci.c:3611〜3626（`XHCI_ROOT_POLL_MS` 100） | **100 ms** | root port の変化 | **名残り**: port の変化は割り込みが `port_pending` を立てて worker を起こす（`port_change_defer`、1383〜1396）。100 ms の周期は「保留の detach の次の pass」と「眠りと起こしの競い」の保険（3620〜3621 の comment）。本来: 起こしは waitq の sequence で競いを閉じ、保留がある間だけ期限 |
| 同じ tid 8・9（diag2・diag3 の kernel 5289d1e4・c5b97dc5 では entry ffffffff8043758a） | — | 100 ms | — | map の無い kernel。tid と周期が同じなので同じ worker と推定（未確認） |
| pid 0 tid 40（diag2、entry ffffffff8022045e = `flusher` の中） | src/kern/buf.c:4176〜4180（`BUF_FLUSH_INTERVAL` 1 s） | **1 s** | 古い dirty の buffer の書き出しと flusher の hook | **道が無い**（buffer が dirty になった事・hook の仕事を flusher に知らせる口が無い。dirty が無い間は要らない） |
| cpuperf の governor | src/kern/cpuperf.c:3822〜3826 | 100 ms（governor が要る間だけ） | 負荷の sample | 締め切り（sample。要らない時は無期限で眠る） |
| thermal の poll | src/kern/thermal.c:57・411 | 1 s | 温度の sample | 締め切り（割り込みの無い sensor の sample。neardiag の上位には出ていない） |
| pageout | src/kern/vm/pageout.c:320〜323 | 100 ms（最初の要求の後だけ） | 空きの memory | 名残り（`vm_reclaim_pageout_kick` が起こす。周期は保険。idle では始まっていない） |

### 8. branch に既にある直し（agent/p1-ws352、基点 main 6aff283baa）

kernel（poll を「変化した object を待つ poll だけ」起こす。目的: 1 つの共有の待ち行列で、どの fd の変化でも system の全 poller〈idle で約 20〉が起きて寝直していた〈survey0: 通知 約 220 回/s、rescans 各 1700〜2000/10 s〉のをやめる）:

| file | 行 | 何のため |
| --- | --- | --- |
| src/kern/poll.c | `struct poll_waiter` 51、`poll_notify` 94、`poll_notify_object` 119、`poll_slot_key` 147、`poll_wait_object` 164、`poll_follows` 192、`poll_sequence` 212、`poll_sleep` 235、`poll_wait_ready` 387、`poll_scan` 449 | 眠る poll ごとの waiter（自分の wait queue と名指しの object 16 個、溢れ・名指し無しは every）。名指しの通知はその object を待つ waiter と every だけ、`poll_notify()` は全員。sequence で scan と競う変化を取りこぼさない |
| include/kern/poll.h 23〜30、include/kern/thread.h 183〜（`poll_waiter`） | 宣言 | 同上 |
| src/kern/pipe.c | 通知 234・327・380・389・575・594、待ち 434 | pipe を名指し |
| src/kern/net/socket.c・socket-file.c | socket.c 254・461・548・587・946・1233、socket-file.c 407・409 | socket を名指し |
| src/kern/net/unix-socket.c | 710・869・1806・2068・2265・2495・2608〜2637・2754 | 自分と peer を名指し |
| src/drivers/generic/input.c | 通知 857・953・1088・2467・2627、待ち 2063 | evdev の device を名指し |
| src/kern/filedesc.c | `notify_table_pollers` 1406、`notify_slot_pollers` 1427、呼ぶ所 573・678・773・956・1208・1268・1354・1390・1563 | descriptor table を scan する poll だけ、さらに fd の番号ごと（`poll_slot_key`、508c992109）に。fd の close で他の番号を待つ poll を起こさない |
| src/kern/system-event.c 82〜127、src/drivers/generic/system-device.c 51・463・469 | | /dev/system の subscriber を名指し |
| src/drivers/audio/audio.c | 通知 421・960、待ち 1885 | dsp の device を名指し |
| src/drivers/generic/bt-hci.c（839faa674c） | 通知 324・378・430・1320、待ち 809 | controller を名指し（bluetoothd の poll が system の全部の名指しの通知で起きていた。diag4 rescans 1077/10 s） |
| src/drivers/gpu/gpu.c（839faa674c） | 2451〜2460 | POLLPRI だけを待つ poll は session を名指し（topology・device の喪失・失敗の通知は全部名指し無しなので取りこぼさない。i915 の hpd.c:694〜699・1226〜1233 で確かめた） |

userland（compositor と zedBSD の backend。詳細は [p002](../phase002/phase.md)）:

| file | 行 | 何のため |
| --- | --- | --- |
| wayland/main.c | `kwl_due` 1533、`kwl_due_soon` 1548、`kwl_due_next_minute` 1561〜、`kwl_watch` 1586〜、loop 1186〜1200、SIGCHLD 491〜503 | 固定の 10 ms をやめ、期限の最小まで（無ければ -1）。非 blocking で読む fd を poll に。SIGCHLD・停止の signal は wake pipe |
| wayland/input-thread.c 266〜269、touchpad.c 434 | `input_pad_timeout`・`kwl_touchpad_due` | touch pad がある間の 10 ms をやめ、tap の期限まで |
| wayland/render-thread.c 666〜676 | `pthread_cond_wait` | 100 ms の timedwait をやめ、無期限 |
| wayland/cursor-thread.c 403〜409 | `cursor_wait` | 変更が無ければ無期限 |
| wayland/threads.c 60・129 ほか（cursor・input・render・glass・settings-store の pthread_sigmask） | | thread を signal を block して作る（signal を main だけに） |
| wayland/machine-shell.c 340 | | machine の読みの終わりに wake pipe |
| wayland/ の各 tick（表 2 の「期限」の行） | | 10 ms ごとの比較をやめ、各々の期限を `kwl_due` で出す |
| libkeiland-backend-zedbsd/daemon-link-zedbsd.c 284〜293 | | 250 ms をやめ、protocol の次の期限まで |
| bluetooth-zedbsd.c 760〜765、audio-zedbsd.c 379〜383、volume-zedbsd.c 393〜397 | | scan が始められる時だけ scan の期限、retry の期限 |
| session-zedbsd.c・backend.c 213（`kl_backend_wait`）、daemon-link-zedbsd.c 195・seat-linux.c 355・seat-freebsd.c 629（`kl_backend_wakes`） | | sessiond の fd を poll に。事象で起こせない backend は従来の 10 ms |

### 未 commit だった変更（b05187933a で残した。完成させていない）

- 何の直しか: display の hotplug を待つ所。compositor は hotplug の fence を 250 ms ごとに `vkGetFenceStatus` で見ている（output-switch.c:141〜195、`OUTPUT_HOTPLUG_MS`）。この fence には kernel の job が無く、`vkWaitForFences` で待っても libvulkan は 1 ms ごとに見る（sync.c:1704〜1708）。前の P1 の案（表 2 の output-switch.c の行）は「hotplug を待つ名前の付いた thread が fence を待ち、signal で wake pipe」。その libvulkan の側を作り始めた物。
- 中身:
  - wsi-display-nodes.c `vulkan_wsi_display_node_topology_wait()`（新規、260〜）: この renderer の display node ごとに今の sequence を読み（`GPU_DISPLAY_EVENTS`）、発見の open でそれを ACK し、和が cursor を超えていれば直ちに戻る。超えていなければ node の fd を POLLPRI で poll（上限は呼び手の時間、無ければ無期限）。kernel の gpu_poll は ACK した sequence より新しい topology で POLLPRI を返す（gpu.c:2489〜2500）。node が 8 を超える時は 1 ms の pause に戻る。
  - sync.c `sync_fences_wait()`（1540〜1541・1573〜1574・1616〜1625・1696〜1702）: 待つ fence が全部「cursor を持つ hotplug の fence」の時、最も低い cursor で上の関数を呼ぶ（それ以外は従来どおり）。
  - sync-internal.h 75〜76: 宣言。
- どこまで出来ているか: libvulkan の側だけ。x86_64 の flags（`build/x86_64/packages/libvulkan/.flags` ＋ `-std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror`）で sync.c・wsi-display-nodes.c・wsi-display-control.c の compile が警告 0 で通る（object は scratch に出した。library の link・試験は未実施）。compositor の側（fence を `vkWaitForFences` で待つ thread と、signal で wake pipe）は無く、今は誰も呼ばない＝動作は変わらない。
- 気になる所（未確かめ）: 発見の open の ACK は他に使う所が無い前提（wsi-display.c:2089〜2141 の lease の待ちは別の open）。display node が無い・全部 EOPNOTSUPP の時は和 0 で fds 0 の poll になり、上限が無ければ signal まで戻らない（fence も決して signal しないので意味は同じ）。複数の thread が同時に待つと ACK を互いに進めるが、ACK は観測した sequence までなので取りこぼしは無いはず。

### 件数の要約（表 1〜7、i02 の確かめの後）

数え方: 表の行ごと（1 行に区分が 2 つあれば両方、animation の 16 箇所は 1 つ）。branch で直した物は直す前の区分で数える。Linux・FreeBSD だけの行（表 2 の audio-stream.c:252・main.c:1191、表 1 の pump・wpa）と起きない保険（main.c:1597）は数えない。

| 区分 | 表 1 thread | 表 2 main の期限 | 表 4 libvulkan | 表 5 client | 表 6 daemon | 表 7 kernel | 計 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 要らない（名残り） | 7（うち済 6） | 9 | 0 | 1 | 6 | 2 | **25**（済 6） |
| 道が無い | 3 | 4 | 1（hotplug は作りかけ） | 2 | 2 | 1 | **13** |
| 締め切りが要る | 9 | 31 | 0 | 6 | 5 | 2 | **53** |

- 済の 6: main loop の固定 10 ms、input thread の 10 ms、render の 100 ms、daemon link の 250 ms、bluetooth の止められた scan の空回り、machine の結果の見張り。
- 名残りで未済の主な物: idle で回る物は sessiond の 1 s（session.c:271〜279）、bluetoothd の 1 s の上限（main.c:749〜751）、audiod の mmap の 1 s（device.c:185〜186）、xHCI の port worker の 100 ms ×2（pci-xhci.c:3625）、compositor の input の scan の 2 s（main.c:1026、zedBSD）。動いている間だけの物は cursor の copy・glass の loader・shot・system の 2 つ・media の helper・retire・animation（表 2）、networkd・network の答えの 10〜20 ms、files の子の 20 ms、printd の 1 s。
- 道が無い主な物: display の hotplug の 250 ms（output-switch.c:171〜177。libvulkan の側は b05187933a で作りかけ）、files の folder の 2 s（file の変化の通知が kernel に無い）、settings の Welcome・Home の 1 s（address の変化が state に無い）、compositor の printers の 1 s・network の詳細の 1 s（view がある間）、kernel の buffer の flusher の 1 s、GPU の fence と cond を一つで待つ口（render の 1 ms・cursor の pin 5 ms・再試行 1 ms、動いている間だけ）。
- 締め切りの主な物で idle に残るのは時計の分（画面の消灯の間も。要検討）と thermal の 1 s。他は予定がある間・操作の間だけ。
- idle で残る周期（branch の tree の code から。実機の測りは未実施）: compositor の hotplug 250 ms・input の scan 2 s・時計の分、files 2 s、settings（Welcome）1 s、sessiond 1 s、bluetoothd 1 s（と seat 5 s）、audiod 1 s、kernel の xHCI 100 ms ×2・flusher 1 s・thermal 1 s。

### 未実施

- 実機・QEMU での測り（依頼の範囲の外。code を読んで確かめた）。表 6・7 の「走るか」の一部（printd が常駐か、audiod の DMA が idle で回るか、bluetoothd の seat の事象、diag2・3 の entry ffffffff8043758a）は未確認。
- b05187933a の link・試験、compositor の側。
- 表の「案」の直し（ユーザーの判断待ち）。
