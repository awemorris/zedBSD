<!-- awesome-plan project=zedbsd record=ws352-p001 -->

# ws352-p001: 解析（idle の時に起きる thread と timeout の一覧、各々が何を待つべきか）

Status: in-progress（2026-11-13 P1、q2046-i01。cleared は Q1）
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
| cursor | cursor-thread.c:408（401） | 変更が無ければ無期限。pin が残る間 5 ms（`CURSOR_PIN_POLL_US`）、refresh の終わりまで（hold）、再試行 1 ms（`CURSOR_RETRY_US`） | main の box の変更（box の `changed` の cond）、main が copy の pin を外したか | 無期限は済。hold は締め切り（動いている間）。pin の 5 ms は名残り（pin を外す main が box の `changed` を signal すれば足りる）。再試行 1 ms は道が無い（swapchain の image が空いた事を知らせる道が無い。動いている間だけ） | pin の 5 ms を box の `changed` に（idle では pin 0 で走らない） |
| cursor の copy・main が cursor の park を待つ | cursor-thread.c:302・484・346（295・477・339） | 100 ms の上限 | copy の fence・park の答え（box の `answered`） | 締め切り（打ち切り） | そのまま |
| scanout・compose の fence と acquire | scanout.c:1069・1074（1065・1070）、compose.c:5124・5180（5138・5194） | 100 ms・1 s・呼び手の上限 | 直接の scanout の image、片付けの fence | 締め切り（打ち切り、動いている間） | そのまま |
| daemon link（audiod・volumed・networkd・bluetoothd・phone の 5 本） | daemon-link-zedbsd.c:293（268） | protocol の次の期限、無ければ -1／**250 ms**（`KL_DAEMON_LINK_TICK_MS`） | daemon の socket・link の wake pipe、再接続・scan・答えの期限 | 直す前の 250 ms は名残り（socket と link の wake pipe）。branch は締め切り（再接続・scan の期限）。bluetooth の止められた scan の空回り（timeout 0）は名残り | 済 |
| sessiond の答え | session-zedbsd.c:185（185） | 20 s の上限 | 起動の時の sessiond の答え | 締め切り（起動の時だけ） | そのまま |
| machine | machine-shell.c（detached） | timeout 無し | /dev/system の読み | 直す前は main が 10 ms ごとに結果を見ていた（名残り） | 済（終わりに wake pipe） |
| glass の loader・prefetch、settings-store の writer、print の writer、font、lane、shot-send、手書き | glass.c:513・2194、settings-store.c:437、print.c:2048、glyph-worker.c:259、lane.c:241・327、shot.c:900、keyboard-hand.c:675 | 無期限の cond の wait | 仕事 | — | 終わりを main に知らせる所は 2.（loader・shot-send） |
| audio の pump（Linux・FreeBSD） | pump.c:860 | 動いている間 10 ms | ALSA・OSS | 範囲の外（zedBSD は audiod の socket を main が poll） | — |
| wpa（Linux） | network-wpa.c:616 | 答えの期限 | wpa_supplicant | 範囲の外 | — |

### 2. main loop の期限（`kwl_due`・`kwl_due_soon`。直す前は全部が 10 ms ごとの pass で時刻を比べていた）

| 場所 | 期限 | 何を見張る | 区分と理由 | 直す形 |
| --- | --- | --- | --- | --- |
| output-switch.c:171・175・177 | **250 ms**（`OUTPUT_HOTPLUG_MS`）、出力を失った時 10 ms | display の hotplug の fence（`vkGetFenceStatus`） | **道が無い**（fence に待てる事象が無い。idle の main loop の timeout 55/10 s のうち 40） | hotplug を待つ名前の付いた thread が fence を待ち、signal で wake pipe（導入済み）。libvulkan の event の fence の待ち（今は 1 ms ごとに見る）を display node の POLLPRI（`GPU_DISPLAY_EVENTS` の ACK の後の topology の変化、kernel にある）で block する |
| main.c:1026 | 2 s、device の事象の後 2 s は 100 ms | 新しい evdev の node を seat に足す scan | zedBSD は**名残り**（/dev/system の INPUT の事象は backend の fd で既に来て、即 scan する。周期は保険） | zedBSD では事象だけ。事象の無い backend（Linux・FreeBSD）は残す |
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
| main.c:1577 | 実時刻の次の分 | 時計 | 締め切り | そのまま |
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
