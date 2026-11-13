<!-- awesome-plan project=zedbsd record=ws352-p002 -->

# ws352-p002: 直しと試験

Status: in-progress（2026-11-13 P1、q2046-i01。cleared は Q1）
Parent: [WS352](../ws.md)。解析は [p001](../phase001/phase.md)。

## 変えた所（agent/p1-ws352、基点 main 6aff283baa）

### kernel: poll は変化した object を待つ poll だけを起こす

- `src/kern/poll.c`・`include/kern/poll.h`・`include/kern/thread.h`: 共有の 1 つの待ち行列をやめ、眠る poll ごとの waiter（自分の wait queue と、待つ object の表 16 個、表に収まらない・名指しの無い fd がある時の every）。scan の間 `thread->poll_waiter` を立て、file type の poll op が `poll_wait_object(object)` で自分の答えが読む object を名指しする。`poll_notify_object(object)` はそれを待つ waiter と every の waiter だけを起こし、`poll_notify()`（名指し無し）は従来どおり全員。どの通知も channel の sequence を 1 つ進め、poll は scan の前に読んだ sequence が変わっていなければだけ眠る（scan と競う変化は取りこぼさない）。各 poll は自分の process の descriptor table も待つ。
- 名指しにした file type と通知（同じ object を poll op と通知の両方が使う）:
  - pipe（`src/kern/pipe.c`）: pipe。読み・書き・reader/writer の増減（FIFO の open を含む）の 6 箇所。
  - socket（`socket.c`・`socket-file.c`）: socket。enqueue・error・close・release・setsockopt の buffer・`socket_poll_notify`。unix socket（`unix-socket.c`）: poll は自分と peer（connection の相手、datagram の peer）を名指しし、送り（peer）・受けの完了（自分。送り手は自分を peer として待つ）・shutdown（自分と peer）・connect（自分と listener）・close（peer）を名指し。
  - evdev（`src/drivers/generic/input.c`）: device。emit・key・unregister・grab・resync の 5 箇所。
  - descriptor table（`src/kern/filedesc.c`）: table。fd の close・差し替えは、その table を scan する poll だけを起こす（以前は全員）。
  - /dev/system の event（`src/kern/system-event.c`・`src/drivers/generic/system-device.c`）: subscriber。届けた subscriber ごとに名指し（16 を超えたら全員）。購読していない open の poll は every。
  - audio の dsp（`src/drivers/audio/audio.c`）: device。period の割り込み・stop。
- 名指ししない（従来どおり全員を起こす）物とその理由:
  - GPU（`gpu.c` 13 箇所・`gpu-fence.c`）: readiness が descriptor・imported fence の alias・completion の observer の間で共有され（「Descriptor and imported-fence observers receive the same terminal state」）、1 つの変化が複数の file・handle の答えを変える。どの object を名指しすれば全部の poller に届くかの対応表を作らないと取りこぼす。idle では GPU の通知は 0 回（下の測り）。
  - tty（`tty.c` 15 箇所）・console・pty: master/slave・controlling terminal・vt の間で状態が共有される。terminal は idle の wake の源ではない。
  - 他（tcp・route socket・bt-hci・hidraw・smartcard・cam・thermal・venus・i915 の hpd・amdrdna・ghci）: idle の源でない。名指し無しは正しさを保つ（全員を起こすので取りこぼさない）。
- 取りこぼしが無い根拠:
  1. poll は scan の前に sequence を読み、channel の lock の下で sequence が同じ時だけ waiter を繋いで眠る（繋ぐのと確かめるのが同じ critical section）。全部の通知が lock の下で sequence を進めるので、scan が始まった後の変化は眠りを止める。
  2. 繋がった後の object X の変化は、X を名指しした waiter と every の waiter を起こす。X を名指しするのは、X の状態を読んで答える poll op だけ。
  3. 名指しにした各 file type で、poll op が読む状態は名指しした object の物だけで、その状態を変える全部の箇所が同じ object を通知する（上の一覧、置き換えは元の `poll_notify()` の箇所ごと）。名指しの無い fd（regular file・未変換の type・handle）を含む poll は every。
  4. 確かめ: QEMU（下）で pipe・stream・datagram・listener の各変化で poll が 1 ms 未満で返る、他の pipe の 200 回の変化では起きない、4 つ同時と全 CPU の負荷の下でも同じ。

### compositor（`userland/desktop/wayland/`）と backend

- main loop（`main.c`）: 固定の 10 ms をやめ、`server->due_ms`（`kwl_due(server, at)` で下げる、`kwl_due_soon()` は動いている間の 10 ms）まで、無ければ -1 で待つ。`kwl_watch(server, fd, events)` で非 blocking で読む fd を poll に足す。scheduler の後に増えた描く物は直ちに次の pass。service の期限・retire・fatal client も期限。`kwl_compose_waiting`（常に 0）を消した。
- 期限を出す所: input の scan、ping、system（network の retry・scan の期限・enrolled の EBUSY）、sysmon（object がある間だけ 250 ms、以前は object が無くても 4 回/s）、machine、printer（daemon の socket を poll、file は 1 s）、media の helper（動いている間 10 ms）、phone、audio stream（zedBSD は socket を poll）、IME、titlebar の key repeat、clipboard（pipe を poll、打ち切りの期限）、drag の release、shot、sleep・画面消灯・pause・rest・pending、idle の lock、時計の分（実時刻）、greeter・lock の card・security key、通知の board・log、power dialog、band、apps bar、Home、corner、OSK、touch、network、volume、arrange の glide、output の hotplug（250 ms）・displays、desktop の再起動、layer cache、frame の pacing、rotate、scanout、glass の loader・blur、cursor の copy。
- fd・thread・signal: sessiond の fd を `kl_backend_wait()` で poll に（sleep の答え・logout の期限も）。machine の thread は wake pipe を書く。SIGCHLD と SIGTERM/INT/HUP の handler は wake pipe を書く。raw の `pthread_create` の 7 本（cursor・input・render・glass の 2 本・settings-store・print）は signal を block して作る。fullscreen の間の sleep の止めは、止めが終わった pass の時刻から数える（`inhibiting`）。
- input thread: touch pad の tap の期限まで（`kwl_touchpad_due()`）、無ければ -1。
- render thread: 仕事が無い時は無期限の cond の wait（100 ms の timedwait をやめた）。
- daemon link（zedBSD）: protocol の次の期限まで、無ければ -1。audio・volume の retry は期限が過ぎていれば 0。bluetooth は scan が始められる時だけ scan の期限を入れる（止められている間の空回りを止めた）。
- backend の口（`keiland-backend.h`、HAL ではない）: `kl_backend_wait()`、`kl_backend_wakes()`（zedBSD 1、Linux・FreeBSD 0 → main loop は従来の 10 ms）、`kl_backend_print_descriptor()`、`kl_backend_media_busy()`、`kl_backend_audio_stream_descriptor()`。

## 試験

（下に追記）
