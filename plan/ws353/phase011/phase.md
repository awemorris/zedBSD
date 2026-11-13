<!-- awesome-plan project=zedbsd record=ws353-p011 -->

# ws353-p011: render は合成の buffer を数で回す（飛んでいる間は最古の fence、1 ms の覗きを消す）

Status: cleared（2026-11-14 P2 q2067-i01、Q1 の確認待ち。branch `agent/p2-ws353d`、c8dcf0dd41・4aae3a92f0・97697a00ac・bbdd14db9f。3 機種の build warning 0、host の `render-step` 試験 PASS（ASan・TSan、基点の code では 11 件 FAIL）、QEMU（Venus）の `qemu-render-wait.sh` が最後の image で 2 回とも全 step PASS、基点と交互に流して描画の速さ・遅れが同じ。WS309 の guest 試験（resize・scanout・rotate）そのものと 5330 などの実機は未実施（下の「未実施」））
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.6・§5.6、対応表の 1-05・1-06・1-13。

## 目的

render の待ちを一度に一つにし、timeout 付きの覗きを無くす。合成の完了を知らせる専用の thread は置かない（ユーザーの決定 4、Q1 の指示）。slot が飛んでいる間は render が最古の fence を無期限に待ち、終わったら REAPED を出す（main は REAPED で `in_flight`・`bake`・`copies`・scene・shot を進めるので、render が完了を見ないと main と render が互いを待って止まる。設計 §5.6、レビュー S1）。

## 変える file

- `render-thread.c`・`render-thread.h`: `KWL_RENDER_POLL_NS`・`KWL_RENDER_ACQUIRE_NS`・`RENDER_WAIT_MS` を消す。step の機械（設計 §5.6 の 1.〜6.）:
  1. 依頼（stop・park・answer・task・want）を先に見て、出来る事をする。
  2. frame の want で slot に空きがあれば output の image を timeout 0 で一度だけ取る。取れたら head（timeout 0）と READY。
  3. 取れず、slot か task が飛んでいる: 最古（submit の順）の fence を `vkWaitForFences(UINT64_MAX)`、REAPED、1. へ。
  4. 取れず、何も飛んでいない: `vkAcquireNextImageKHR(UINT64_MAX)`。
  5. 依頼が無く、飛んでいる物がある: 最古の fence を無期限、REAPED、1. へ。
  6. 依頼が無く、何も飛んでいない: `changed` の cond を無期限。先回りの acquire（`render_ahead`）はここで timeout 0 の一回だけ（block しない。レビュー S4）。
  park・drain・stop は飛んでいる fence を順に無期限で待ってから答える。fence の reset は render だけ。
- `compose.c`: COMPLETED を REAPED の意味に（今の扱い compose.c:4770〜4840 は同じ）。render の op（`compose_op_wait` の slot の fence compose.c:5137、task の fence 5194、output の acquire 4941）の timeout は render が渡すので、render の側で `UINT64_MAX` と 0 だけを渡す（対応表 1-13、レビュー L7）。head の image は timeout 0 のまま（missed は p010 の SHOWN で。それまでは今の扱い）。

## 先に確かめる事

- 表示が止まった時（display の電源・park、出力の移動）に、render の 4. の acquire が返る事。i915 は flip の latch を 100 ms 待って EIO（xd-picture.c:35・1078〜1115）、worker が `chain->error` を立て（wsi-swapchain.c:4325〜4338）、acquire は error で返る（4592〜4603）と読んだ。amdrdna・nvrtx・videocore・ghci・venus の present が表示の止まりで返るかを読む。返らない driver があれば止まって Q1 に報告（display の thread の park の上限 10 s に掛かる。レビュー S4）。

## 残す振る舞い

- READY・answer・cancel の手順、task の順（task は frame の前に submit）、park と drain の意味、切り替えの打ち切り（T3 の 100 ms ずつ、最大 10 s）、image が空いている時の前の frame の GPU と次の frame の walk の重なり（2. の timeout 0）。

## 確かめ

- build warning 0。host: render の step の試験（WS309 の `render-task` の host 試験を広げる。偽の Vulkan で）: slot の満杯、acquire の block、park の最中の依頼、**layer の焼きの後に依頼が無い時に REAPED が来て main が次を頼める、2 frame の後に依頼が無い時に `in_flight` が 0 に戻る、present の失敗（OUT_OF_DATE）の frame の後に REAPED が来る、`kwl_compose_quiesce` と `kwl_compose_complete` が返る**（レビュー S1・L11）。
- QEMU: 描画の回帰（WS099 の C1・C2、`KWL PERF pass`）、出力の移動・回転・game mode の出入り（p012 の resize・scanout・rotate の guest 試験）、layer cache の焼き（静けさの後）の後に画面の変化が描かれる。WS352 の poll の診断で render の thread の timeout の起床が 0（描画中も）。

## 受け入れ

- 上の判定が PASS。render に timeout 付きの待ちが打ち切り（切り替えの間）の他に無い。結果・SHA・未実施を書く。

## 依存

- p004（順の都合だけ。main loop の Phase と p010 に依らない。REAPED が GPU の完了の直後に来るので、callback・animation・client の buffer の返しは今の意味のまま動く）。

## 結果（2026-11-14 P2、q2067-i01、branch `agent/p2-ws353d`、基点 main 8bcc72fbb0）

### commit

- c8dcf0dd41: `render-thread.c`・`render-thread.h`・`compose.c`（render の step の作り直し）と host の試験 `plan/ws353/tests/render-step/`。
- 4aae3a92f0: render の thread の数え（`struct kwl_render_counts`）と停止の時の 1 行 `KWL RENDER stopped`（compose.c）。
- 97697a00ac: host の試験に数えの確かめ。
- bbdd14db9f: render が fence を見た時刻（`outcome.completed_us`、前から field だけあって誰も書いていなかった）と `--log-frames` の `KWL LAT completed`、QEMU の試験 `plan/ws353/tests/qemu-render-wait.sh`・`config-amd64-render.mk`。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 実装

- `KWL_RENDER_POLL_NS`（1 ms）・`KWL_RENDER_ACQUIRE_NS`（50 ms）・`KWL_RENDER_FENCE_NS`（100 ms）を消した（`RENDER_WAIT_MS` は WS352 の merge で既に無い）。render の待ちは、最古の fence の `UINT64_MAX`、何も飛んでいない時の output の acquire の `UINT64_MAX`、何も飛んでいない時の `changed` の cond の 3 つだけで、一度に一つ。
- `kwl_render_step` の順（設計 §5.6 の 1.〜6.）:
  1. 一度の lock で依頼（task・answer・stop・park・want）を見る。task は submit（後の list より前）。
  2. 答えの list があれば走らせる（stop・park より前。park の後に list が残らない）。
  3. stop・park: 飛んでいる物があれば最古の fence を待って REAPED（一つずつ）、無くなったら stop は終わり、park は答える（`render_parked`）。park の間は task だけ submit と待ち。**park を求められている間は新しい want を取らない**（下の「設計からの変更」1）。
  4. cancel の答えを取る。
  5. want（frame・layer）で slot に空きがあれば `render_idle`: output の image を、何か飛んでいれば timeout 0、何も飛んでいなければ `UINT64_MAX` で取る。取れたら head（timeout 0）と READY。
  6. 出来る事が無く飛んでいる物がある: 最古（submit の順、slot と task を通して `serial` で比べる）の fence を `UINT64_MAX` で待ち REAPED（今の名前は `KWL_RENDER_COMPLETED`、意味は「fence を見た」に揃えた）。
  7. 何も飛んでいない・want も無い: 先回りの acquire（`render_ahead`）を timeout 0 で一回だけ、取れなければ cond で眠る。
- BLOCKED（cond で眠る）を返すのは何も飛んでいない時だけ。だから眠る前に全部の完了が main に届く（S1）。
- fence の待ちの結果が VK_SUCCESS 以外なら fence の失敗として完了を出す（無期限の待ちに VK_TIMEOUT は無い）。acquire が `UINT64_MAX` で VK_TIMEOUT・VK_NOT_READY を返した時（規格では無い）は、空回りせず次の変化を待つ。
- `struct kwl_render_message` に `serial`（thread が submit した順、1 から）を足した。p010 の `done_serial` の土台に使える。
- compose.c は comment（op の timeout は 0 か `UINT64_MAX`、COMPLETED は fence を見た後）と、停止の時の `KWL RENDER stopped jobs= tasks= fence_waits= task_waits= acquires_waiting= acquires_now= acquires_missed= sleeps=` の 1 行、`KWL LAT completed` の 1 行だけ。render の op の関数（`compose_op_wait` など）は timeout をそのまま渡すので変えていない。

### 先に確かめた事（S4: 表示が止まった時に render の無期限の acquire が返るか）

- 読んだ（Explore の担当に調べさせ、要の行は自分で確かめた）: display の FIFO の chain の image が空くのは次の present が前の front を外す時（wsi-display.c:1494〜1502、hold の受け渡し）か present の job の retire（wsi-swapchain.c の `present_finish` が AVAILABLE にして chain の waiter を起こす）。compositor の output の chain は 3 枚（surface の minImageCount 2 に 1 枚足す）で、render が持つのは高々 1 枚、display が持つのは front の 1 枚なので、何も飛んでいない時の acquire は空きがあってすぐ返るか、飛んでいる present の retire で返る。
- 各 driver の kernel の present は時間の上限を持って返る（失敗か、前の front を外して成功）: venus（1 refresh＋各 host の命令 10 s、抜きは ENXIO・世代違いは ESTALE、消灯中は host 無しで front を替える）、amdrdna（flip の latch 100 ms で EIO、blank は即 0、system suspend は拒む）、nvrtx（flip の notifier 2 s で EIO、suspend は拒む）、videocore（adoption 100 ms で ETIMEDOUT、以後 EBUSY）、ghci（12 s、抜きは ENXIO、lease の終わりは EBUSY）、i915（`drv_i915_xd_picture_flip_wait` 100 ms で EIO、xd-picture.c:35・1080〜1117。抜きは ENXIO）。
- 例外は i915 の suspend: present の本体は i915 の worker（`drv_i915_worker_sync_display`）を通り、その完了待ち（worker.c:1996 の `while (item->done == 0)`）は上限が無く、suspend で worker が park している間は resume まで返らない。
- Q1 の問い（実害があるか）への答え: 実害は無いと読んだ。
  - sleep の経路（sleep.c）: 2 枚の frame（lock 画面）を描いた後に `kl_backend_power_action(SUSPEND)` を送り、同じ pass で `sleep_hold` を立てる（sleep.c:625）。`sleep_hold` の間 display の pass は want を上げずに戻る（display.c:169〜172）。answer が来るまで（resume の後）描かない。
  - 止まりうるのは libvulkan の swapchain の worker（kernel の present を同期で呼ぶ thread）で、render の thread ではない。`vkQueuePresentKHR` は worker に job を積むだけで、display の chain（GPU の present）では待たない（待つのは copy の fallback の chain だけ、wsi-swapchain.c:2990〜3006）。
  - render の slot の fence は GPU の job の fence で、present の job とは別。suspend の前に submit した job の fence は GPU の完了（遅くとも resume の後）で返る。
  - render が suspend をまたいで acquire に block するのは、(a) suspend の前に上がった FRAME の want が残り、(b) 何も飛んでおらず、(c) front 以外の 2 枚の present の job がどちらも parked の worker で止まっている、が重なった時だけ。そうなっても resume で worker が進めば retire して返る。suspend の間に render を待つ者はいない（main は `sleep_hold`、park は resume の後の display の切り替えから来て、上限 10 s の中で返る）。前の形（50 ms の打ち切りの繰り返し）との差は、suspend の間に render が起きないことだけ。
  - 残る危険: resume の後に i915 の worker が戻らない（resume の失敗）時は、render も acquire から返らない。その時は display も使えないので別の障害として現れる。実機での確かめは未実施（p016 の 5330 の sleep で見る事を推す）。

### 確かめ（実行したコマンドと結果）

- build（-Werror）: `make -j4 BUILD_JOBS=4 sysroot` と `build/x86_64/packages/wayland/.stamp`、`CONFIGMK=config/config.rpi4.mk`（arm64）・`config/config.vf2.mk`（riscv64）で同じ。c8dcf0dd41 で 3 機種 rc 0・warning 0（`build/p11-{x86,rpi4,vf2}-{sysroot,pkgs}.log`）。最後の source（bbdd14db9f）で arm64・riscv64 の wayland の package を作り直して rc 0・warning 0、render-thread.c・compose.c の compile を log で確かめた（`build/p11-{rpi4,vf2}-final.log`）。x86_64 は QEMU の image の build（`make userland`・`make disk`、`CONFIGMK=plan/ws353/tests/config-amd64-render.mk`）で最後の source を compile、warning 0（`build/p11-img-{userland,disk}.log`）。
- host: `sh plan/ws353/tests/render-step/run.sh`（ASan+UBSan と TSan、偽の Vulkan の fence と image を cond で持ち、thread は本物の thread で走らせる）が両方 PASS（3 回以上）。場面: idle で acquire・fence の待ち 0、layer の焼きの後に依頼が無くても完了が来る（fence の待ちは 1 回）、2 frame の後に依頼が無くても両方が submit の順に来て `in_flight` が 0、present の失敗（OUT_OF_DATE）の frame の完了が来る、slot が満杯の間は acquire も READY も無く最古の完了の後に READY、飛んでいる物がある時の acquire は timeout 0 の 1 回、何も飛んでいない時は `UINT64_MAX` で block し 1 回だけ、表示の失敗で block の acquire が返り ACQUIRE_FAILED、park の最中の want は READY を出さず park の後に出す、task と job の待ちは submit の順、submit の失敗は順番に、park の間の task、待たない park は task の後、stop は task と frame を待つ、`kwl_compose_complete` と同じ形（`kwl_render_wait_message` の繰り返し）で待つ main が返る、全部の待ちが 0 か `UINT64_MAX`、fence ごとに待ちは 1 回（jobs 11・fence_waits 11、tasks 7・task_waits 6）。同じ試験を基点の render-thread.c で build すると 11 件 FAIL（timeout 付きの待ち、fence の待ちが 2100 万回、park の最中の READY など）。
- QEMU（Venus、x86_64、max_outputs=1、`-display dbus,gl=on`。serial の短い命令で guest の file を数える。console の log は読まない）: `IMAGE=build/ws353/p011b.img sh plan/ws353/tests/qemu-render-wait.sh build/ws353/final-{1,2}` が 2 回とも全 step PASS:
  - draw-shm（wlshm 600 frame）: compositor 601 frame を 24.8 s（frame の間隔 50 ms 以下が 585〜587）。draw-vk（wltest FIFO 300 frame）: 303〜304 frame を 15.6〜15.7 s。
  - rotate 90・0（turn は render の park）: 各 turn の後 3 s に 61〜65 frame。
  - bake: turn の後の静けさに layer の絵が焼かれ（KWL LAYER baked）、その後に 61 frame（S1 の (1)）。
  - game: mview の game mode の出入り（KWL SCANOUT）、その後 window mode で 61 frame。
  - shot: 書けた。stop: `jobs=1239 tasks=6 fence_waits=1239 task_waits=6 acquires_waiting=4 acquires_now=1226 acquires_missed=1 sleeps=2438`（2 回目 1235・6・1235・6・6・1219・0・2408）。fence の待ちは job ごとに 1 回で、見回りは無い。errors: KWL FAILED・VULKAN_ERROR・KWL RENDER stalled 0。
- 基点との比べ（応答が変わらない事）: 基点の 3 file だけを戻した image（`build/ws353/base.img`）と交互に 2 回ずつ（`build/ws353/abab.sh`、他の QEMU が動いていない時）: wlshm 600 frame は基点 24.8 s・24.8 s、p011 24.8 s・24.7 s。wltest は基点 16.5 s・15.6 s、p011 15.5 s・16.2 s。frame の間隔の分布も同じ。`--log-frames` の probe（wlshm 200 frame、`build/ws353/probe.sh`）で submit→main の適用（KWL LAT shown）の中央値は基点 10.3 ms・p011 10.3 ms、frame の間隔の中央値は両方 40 ms。p011 の submit→render が fence を見た（KWL LAT completed）の中央値は 9.9 ms・最大 52 ms。
  - 違いは 1 つ: 新しい window の最初の frame の「submit→main が完了を適用」が p011 で約 190〜235 ms（基点 10〜50 ms）。`KWL LAT completed` で見ると render が fence を見たのは submit の 16 ms 後で、残りは main がその window の最初の Vulkan の仕事（`vk_outside_max_us` 182 ms）をしている間に完了の message が待った分。次の frame の walk はどちらも acquire の約 230 ms 後で、描画の遅れにはなっていない。
  - 最初の 2 回（`build/ws353/rw1`・`rw3`）は wlshm が 5〜12 fps だったが、同じ時に他の担当の QEMU が host で動いていた。交互の比べはその後に、他の QEMU が無い時に流した。
- 試験の経過: 最初の script は WS309 の guest の試験と同じ SSH の形で書いたが、この image の系統（mainwake の config）では SSH が答えず（原因は調べていない。serial では sshd は動いていた）、qemu-main-wake.sh と同じ serial の形に書き直した。bake の step は最初「静けさで焼かれるのを待つ」形で、基点でも焼かれず FAIL（Home の絵が古くならないと焼かない）。turn の後の焼きを使う形に直した。

### 設計からの変更（理由）

1. park（と stop）を求められている間は新しい want を取らない（前は frame が飛んでいる間の park の最中にも acquire して READY を出していた）。park を待つ main は答えられず、READY の image を持ったまま park に入る事になるため。host の試験の K で確かめた。依頼を先に見る順（設計 §5.6 の 1.）の中の優先の付け方で、park・drain の意味は変えていない。
2. 何も飛んでいない時の acquire は timeout 0 を一度試さずに初めから `UINT64_MAX`（設計の 2.→4. と同じ結果で、呼び出しが 1 回少ない）。
3. 停止の時の `KWL RENDER stopped` の数えの 1 行と `KWL LAT completed` を足した（phase.md に無い）。WS352 の poll の診断（kernel の patch）の代わりに、render の待ちが fence ごとに 1 回で見回りが無い事を QEMU で数えるため。数えは render の thread だけが書き、join の後に読む。
4. COMPLETED の名前は REAPED に変えず、意味（fence を見た後）を comment に書いた（P1 の file を含む利用の所を触らないため）。

### 未実施

- WS309 の guest 試験（resize・scanout・rotate・task・grave）そのもの: 標準の image の作り方（`plan/tools/guest/test-image.sh`）と WS309 の config が旧い build の変数（`ZEDBSD_CONFIG`・`BUILD`）のままで今の Makefile（`CONFIGMK`）で作れず、SSH の harness もこの image の系統で答えなかった。rotate・game mode（scanout の出入り）・layer の焼きは `qemu-render-wait.sh` で代えた。出力の移動（resize-guest の head の抜き差し）は未実施（p004 の `qemu-hotplug-follow.sh` が render の park を含む移動を通るが、この Phase では流していない）。
- WS099 の C1・C2: 未実施（criteria の image も旧い build の形）。
- WS352 の kernel の poll の診断: 未実施（`KWL RENDER stopped` の数えで代えた）。
- 実機（i915 の suspend をまたぐ場面、5330・Pi・VF2）: 未実施。arm64・riscv64 は build だけ。
- cursor の thread（p012）・client の buffer（p010）は範囲の外で触っていない。

### p010・p012 への申し送り

- p010: REAPED（今の `KWL_RENDER_COMPLETED`）は render が fence を見た直後に、submit の順に必ず来る（依頼が無くても、bake・present の失敗でも）。message の `serial`（render が submit した順、slot と task を通して 1 から）を `done_serial` に使える。frame の完了の時刻は `outcome.completed_us`。client の buffer の hold（`slot->held`）と callback は今のまま slot にある。
- p010: 新しい window の最初の frame で、main が長い Vulkan の仕事の間は完了の適用（callback・buffer の返し）が遅れる（QEMU で約 175 ms）。render の側ではなく main の仕事の長さ。
- p012: cursor の thread の acquire も、何も飛んでいない時の `UINT64_MAX` は display の hold の受け渡しと present の retire で返る（上の S4）。i915 の suspend の間は present の worker が resume まで止まる。
- Q1 へ: `plan/ws309/tests/render-task/` は新しい step（無期限の待ち）では hang するので、`plan/ws353/tests/render-step/` で置き換えた（消すのは Q1、merge と同じ commit で、と返事を受けた）。
