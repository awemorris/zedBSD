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
