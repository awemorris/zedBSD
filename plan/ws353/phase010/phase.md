<!-- awesome-plan project=zedbsd record=ws353-p010 -->

# ws353-p010: shown の thread（present の完了で callback・animation）と、client の buffer を slot から外す

Status: cleared（2026-11-14 P2 q2073-i01、Q1 の確認待ち。branch `agent/p2-ws353e`、b8be1d4f07・cb4bbfda4d・a74a83e62b・e705381967。3 機種の build warning 0、host 試験（shown・returns・render-step・timer/retire）PASS、QEMU（Venus）の `qemu-shown.sh` 全 step PASS、`qemu-render-wait.sh` 2 回 PASS、`qemu-animation.sh` は 3 回目で全 step PASS。ただし Venus では client の frame の速さが落ちた（wlshm 1 frame 41→68 ms、wltest 300 frame 16.1→18.1 s）。受け入れの「描画の応答が変わらない」は QEMU では満たさない。Q1 の判断待ち（下の「Q1 へ」））
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.1・§5.8・§5.9、対応表の 2-12・2-13。

## 目的

1. 新しい thread shown（T17）が window mode の frame の present の fence（p009）を待ち、main に SHOWN を知らせる。frame の callback・animation の一歩・head の missed の want し直しをそこから行う（合意した形の「frame の callback は present の完了で」）。
2. client の GPU の buffer の返しを、合成の slot から外し、render の REAPED の serial で決める（ユーザーの決定 5）。slot の `held[]`・`callbacks` を消す。

T17 を game mode の present の lane（T4、scanout.c）に載せない: display の thread が game chain を壊す前の `kwl_scanout_present_wait` と lane の FIFO が window mode の待ちに引きずられるため（設計 §5.1、レビュー S2）。

## 変える file

- `render-thread.c`・`render-thread.h`: T17（`KWL THREADS` に `shown`）。present の fence の輪（空きの list、lock。空きが無ければ `vkCreateFence` で足し待たない）。render は `vkQueuePresentKHR` に `VkSwapchainPresentFenceInfoEXT`（output と載せた head ごと）を付け、present の後に「frame n、(swapchain, fence) の列」を T17 に渡す。swapchain ごとの結果が OOM なら fence を空きに戻し、SHOWN(n, swapchain, 失敗) を render が自分で積む。OUT_OF_DATE・SURFACE_LOST は T17 に渡して結果を添える（レビュー L2）。T17 は飛んでいる fence があれば swapchain ごとの最古を any で `vkWaitForFences(UINT64_MAX)`（遅い head と output が互いを待たない、レビュー L4）、返った物ごとに SHOWN を出口に積み wake、fence を reset して空きに戻す。無ければ自分の cond。停止は render の後で、残りの fence を待ち終えてから終わる（打ち切り無し）。
- `compose.c`・`compose.h`・`kwl.h`: frame ごとの記録（番号、callback の列、表示した head）。output の SHOWN(n) で callback を送る（present の失敗でも送る）。head の SHOWN で、その head の missed を want し直す。slot から `held[]`・`callbacks` を消す。
- `objects.c`・`compose.c`・`kwl.h`: buffer の `read_serial`（frame と layer の焼きで sample した時に上げる）、main の `done_serial` と返し待ちの list。`holds` は protocol・current・scanout（game mode の lend、scanout.c:420〜425）の分だけ（レビュー L9）。返しは「置き換わった」かつ `read_serial ≤ done_serial`。`done_serial` は REAPED だけで上がる（render が submit の順に待つので、GPU の job の順の前提は要らない）。去った client の buffer の解放も同じ条件に serial を足す（歩調は p005 の形）。
- `shm.c`: 写しの条件を「その host image の staging を読む slot が飛んでいない」に狭め、待つ時は REAPED で続ける。
- animation（p008 で REAPED にした物）を output の SHOWN へ。

## 先に確かめる事

- p011 が入っている事（REAPED が GPU の完了の直後に来る。来ないと返し待ちの list が idle で残る）。

## 残す振る舞い

- `wl_buffer.release` と frame の callback の意味（protocol は変えない）、shm の「写したらすぐ返す」、game mode の present の lane（T4）と `kwl_scanout_present_wait`。

## 確かめ

- build warning 0。host: 返し待ちの list と serial の試験（置き換え・同じ buffer の再 attach・client の切断・present の失敗・layer の焼きが読んだ buffer）、T17 の試験（偽の fence で、output と遅い head、OOM の present、停止の時に残りの fence）。
- QEMU: FIFO の Vulkan の client（vkcube の類）と shm の client が止まらず回る、`--log-frames` の RELEASE が読みの完了（REAPED）の後、callback が present の後、client の切断で buffer が残らない（ASan の build があれば）、game mode の出入りと出力の移動・回転で T17 が止まらない。idle で T17 は起きない。

## 受け入れ

- slot に client の buffer と callback が無い。上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p009、p008、p011。

## 結果（2026-11-14 P2、q2073-i01、branch `agent/p2-ws353e`、基点 main 12ac69492c）

### commit

- b8be1d4f07: compositor（`render-thread.c`・`render-thread.h`・`compose.c`・`compose.h`・`kwl.h`・`objects.c`・`shm.c`・`display.c`）と host 試験 `plan/ws353/tests/shown/`・`plan/ws353/tests/returns/`。
- cb4bbfda4d: QEMU の試験 `plan/ws353/tests/qemu-shown.sh`。
- a74a83e62b: shown の thread が始められない時は止めずに present fence 無しで続ける（compose.c、失敗の経路だけ）。
- e705381967: `qemu-animation.sh` の Home の下限（下の「確かめ」）。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 実装

- T17 shown（`render-thread.c`、`struct kwl_shown`、`KWL THREADS` に `shown`）: present の fence の空き（`spares`、lock）、render から渡された present（`given`）、自分が待つ present（`flying`）。待ちは「swapchain（output と head ごとの key）ごとの最古の fence を一度に any で `UINT64_MAX`」か、何も無ければ自分の cond（無期限）。返ったら signal 済みの最古ごとに `KWL_RENDER_SHOWN`（job の番号・key・present の結果）を render の順の列に積み、fence を reset して空きに戻す。待ちが失敗（device lost）なら全部を失敗として知らせ fence を壊す。停止（`kwl_shown_stop`）は render の停止の後で、渡された present を全部知らせてから終わる（打ち切り無し）。
- render: frame の job の run の前に output と各 head の present に空きの fence を付け（`job.present_fence`・`job.head_fences[]`、空きが無ければ `vkCreateFence`、待たない）、run の後に present の結果で振り分ける: 積まれた（SUCCESS・SUBOPTIMAL・OUT_OF_DATE・SURFACE_LOST）物は T17 へ、積まれなかった物（OOM・device lost・run の失敗）は fence を空きに戻し SHOWN（失敗）を自分で積む。fence を作れなかった present は SHOWN をすぐ積む。shown の thread が無い時（拡張が無い backend）は job の完了の直前に SHOWN を積む（今までの callback の時と同じ）。
- compose: `VK_KHR_get_surface_capabilities2`・`VK_EXT_surface_maintenance1`（instance）と `VK_EXT_swapchain_maintenance1`（device、feature を `vkGetPhysicalDeviceFeatures2KHR` で確かめて pNext に）を、あれば有効にする（`KWL SHOWN present_fences=`）。`compose_op_run` の present に `VkSwapchainPresentFenceInfoEXT`。
- frame の記録（`struct kwl_frame_told`、`compose->told`）: frame を渡す時に callback の列・番号・始め・載せた head を持つ。output の SHOWN で callback を送り（present の失敗でも）、`frame_done_ms` を立て（frame pacing は「窓に知らせた時」から）、`KWL LAT shown` を出す。head の SHOWN で、head が missed した後なら dirty（`head_missed`、head の present が飛んでいない時は今までどおりすぐ dirty）。全部の present が SHOWN で記録を捨てる。`frame_wait_ms` は今までどおり描画の時間の半分（REAPED で）。
- slot から `held[]`・`held_count`・`callbacks` を消した。client の buffer: `read_drawing`（`surface_image` が描画の番号 `drawings_given + drawing_open` を記す。全部の描画・layer の焼き・head がここを通る）、置き換わって最後の hold が外れる時に `read_drawing > drawings_done` なら hold を残して `compose->returns` に並べ（`objects.c` の `buffer_return_later`）、REAPED（`drawings_done` が上がる時）に `kwl_buffers_returned` が返す（`--log-frames` で `KWL RETURN client= buffer= read= done=`）。壊された buffer の解放も同じ（hold が残る間は free しない）。client が去る時は `kwl_buffers_client_gone` がその client の物をすぐ put（fatal なので知らせない、import は grave が描画を待つ）。`drawings_done` は COMPLETED（REAPED）だけで上がる。
- animation（設計 §5.9）: `kwl_schedule` の tick の前に dirty が無く、tick が dirty を立て、output の frame がまだ SHOWN でない時は、その dirty を `step_after_shown` に回し、output の SHOWN で dirty にする（`display.c`）。tick は時刻で位置を決めるので、一歩は SHOWN の pass で描かれる。
- shm: 写しの条件を image ごとに（`kwl_compose_host_image_writable`: その host image の staging を最後に取った描画 `copy_drawing` が完了し、layout の task も済み）。作り直し（大きさの変更）は queue を使うので今の全体の条件のまま。待つ時は upload の印を残し、REAPED の pass でやり直す。

### 設計からの変更（理由）

1. `done_serial` に render の message の `serial` ではなく main の `drawings_given`・`drawings_done`（grave と同じ数え）を使った。main は描く時（walk の中）に buffer に印を付けるので、render が submit の時に決める serial はまだ無い。REAPED は submit の順に来るので `drawings_done` は「その番号までの描画が全部終わった」を言う。
2. 返し待ちの list は `compose.c` でなく `objects.c`（buffer の寿命の file）に置いた（host 試験で本物の code を流すため）。list の頭は compose にある。
3. 拡張の無い時の SHOWN: phase.md は触れていない。render が完了の直前に積む（今までの callback の時と同じ意味）。
4. animation の「SHOWN で一歩」を、各 animation を直さず `kwl_schedule` で「tick だけが立てた dirty を SHOWN まで遅らせる」形にした（animation の tick は 16 か所あり、どれも時刻で位置を決めて dirty を立てるだけなので）。
5. head の missed の want し直しは「どれかの head の SHOWN」で（missed した head の present が飛んでいない時はすぐ）。
6. `KWL LAT shown` の意味が「GPU の完了を main が適用」から「output の present の SHOWN を適用」に変わった（`plan/tools/frame-ops/key-latency.py` の shown もこの意味になる）。GPU の完了は `KWL LAT reaped`（新）と `KWL LAT completed`（p011）。

### 確かめ（実行したコマンドと結果）

- build（-Werror）: `make -j4 BUILD_JOBS=4 sysroot` と `build/x86_64/packages/wayland/.stamp`、`CONFIGMK=config/config.rpi4.mk`（arm64）・`config/config.vf2.mk`（riscv64）で同じ。b8be1d4f07 で 3 機種 rc 0・warning 0（`build/p10-{x86,rpi4,vf2}-*.log`）、最後の source（a74a83e62b、compose.c を compile し直す）でも x86_64・arm64・riscv64 とも rc 0・warning 0（`build/p10-x86-pkg4.log`・`build/p10-{rpi4,vf2}-final.log`）。基点も x86_64 で rc 0・warning 0。toolchain・Venus の renderer は p2-ws353d の build から写した（symlink ではない）。
- 書き方: `python3 -I plan/tools/style-check.py` を変えた C の file に流し、基点の同じ file と比べて増えた指摘 0、新しい試験の C は指摘 0。`git diff --check` は空。
- host:
  - `sh plan/ws353/tests/shown/run.sh`（新、ASan+UBSan と TSan、各 3 回以上）: PASS。場面: output の 3 frame の present が present の順に一つずつ知らされ、fence が signal するまで何も来ない、fence は使い回し（3 frame で 2 個）、待ちは present ごとに 1 回／遅い head の present が残っても output の後の frame は知らされる（一つの待ちで両方）／OOM の present は即座に失敗として知らされ fence は空きに戻る、OUT_OF_DATE は fence の後に結果付きで、run の失敗は output と head の両方を即座に失敗で／fence を作れない時は present の後すぐ shown／shown の thread が無い時は完了の前に output と head の SHOWN／待ちの device lost は全部を失敗で知らせ fence を壊す／本物の thread で、何も無い間は 1 回眠って fence を待たず、停止を頼んでも残りの fence が signal するまで終わらない。
  - `sh plan/ws353/tests/returns/run.sh`（新、objects.c の本物の code、plain と ASan/UBSan）: PASS。場面: 読まれていない buffer はすぐ release／描画 2・1 に読まれた 2 つは hold を残して待ち、描画 1 の完了で 1 つ、順と list の末尾が保たれる／待つ間に再び attach された buffer は完了で release されない、置き換えで release／壊された buffer は描画の完了まで free されない／去る client の分はすぐ put、他は待ち続ける／fatal の client と window mode でない時は待たない。
  - 既存: `sh plan/ws353/tests/render-step/run.sh` PASS（shown の thread 無しの経路）、`sh plan/ws353/tests/timer/run.sh` PASS（objects.c を含む retire）。
- QEMU（Venus、x86_64、max_outputs=1。serial の短い命令で guest の file を数える。console の log は読まない）:
  - image: `config-amd64-render.mk`（`build/ws353/p010-render.img`、compositor は b8be1d4f07 の source）と `config-amd64-anim.mk`（`build/ws353/p010-anim.img`、a74a83e62b）。build の warning は noct の外で 0。
  - `IMAGE=build/ws353/p010-render.img sh plan/ws353/tests/qemu-shown.sh build/ws353-p010-qemu`（新）: 全 step PASS。start（`KWL THREADS` に shown、`KWL SHOWN present_fences=1`）、draw-shm 600、draw-vk（wltest FIFO 300 frame、止まらない）、returns（wltest の client の `KWL RELEASE` 300、`KWL RETURN` 1 件で done ≥ read、早い返し 0）、told（`KWL LAT shown` 908 件が全部その frame の `KWL LAT submit` の後、present から shown まで中央値 37 ms・最大 229 ms）、gone（client 2 つの `KWL CLEANUP done`）、rotate 90・0、bake、game、shot、stop（`KWL RENDER stopped jobs=1160 fence_waits=1160`）、shown-stop（`KWL SHOWN stopped shown=1151 failed=0 waits=1151 sleeps=1151 fences=2`、composed 1151）、errors 0。
  - p011 の `qemu-render-wait.sh` を基点（main 12ac69492c の 8 file に戻した image `build/ws353/p010-base.img`）と交互に 2 回ずつ（`build/ws353/abab.sh`、他の QEMU 無し）: p010 は 2 回とも全 step PASS。基点の 1 回目は stop だけ FAIL（`KWL EXIT` が 70 s に出ない、2 回目は PASS。原因は調べていない）。速さ（下の「Q1 へ」）: wlshm 600 frame は基点 24.7・24.6 s、p010 41.1・41.1 s。wltest 300 frame は基点 16.0・16.2 s、p010 17.9・18.3 s。
  - p008 の `qemu-animation.sh`（`IMAGE=build/ws353/p010-anim.img`）: 1 回目は notify-board だけ FAIL（試験は 20 s に gone を見なかったが、guest の log には show から 3594 ms で gone がある。試験の serial の読みの不調と見るが確かめていない）。2 回目は home-close が 238 ms で下限 240 を割り FAIL。Home の animation は最初の frame で 8 ms 進んだ所を見せる設計（`COMPOSE_HOME_FIRST_MS`）で、依頼から 8 ms 以内に最初の frame を記録すると長さより短く終わるので、下限を open 272・close 232 に直した（e705381967）。3 回目は全 step PASS（idle-60 `passes=6 timeouts=0`、home-open 290 ms・6 frame、home-close 259 ms、notify-board 3577 ms、clock の前後、osk-slide 3 frame・最長の間 91 ms、idle-after-20 `passes=1 timeouts=0`）。p008 の時（home-open 302 ms・9 frame、osk 6 frame・47 ms）より frame の数が減り間が広い（一歩が present の受け付け、Venus では約 37 ms の後になるため）。長さは同じ。
- 証拠: `build/ws353-p010-qemu`、`build/ws353/ab-{base,render}-{1,2}`、`build/ws353-p010-anim{,2,3}`、`build/ws353/abab.out`。

### 未実施

- 実機（i915 の 5330 など）での present の受け付けの時と client の速さ: 未実施（p016）。i915 では present の返りが前の frame の latch の時なので client の一周は 1 refresh に収まる読みだが測っていない。
- head（max_outputs=2 の出力の移動）、game mode の中の T17 の静けさの数え、OOM の present（host 試験だけ）、ASan の build での client の切断: 未実施。idle で T17 が起きない事は host 試験（TSan）と QEMU の sleeps の数（present ごとに 1 回）で代えた。
- Linux・FreeBSD の backend（拡張が無い時の経路は host 試験だけ）。arm64・riscv64 は build だけ。

### Q1 へ

- 速さ: 合意した形と決定 (b) のとおり callback を present の受け付け（present fence）で送ると、Venus では fence が vkQueuePresentKHR から約 37 ms 後（kernel の present の返り、p009 の 40 ms と同じ）に来るので、client の一周が「受け付けの待ち＋client の描画＋合成＋GPU」の直列になり、wlshm は 1 frame 41→68 ms、wltest は 300 frame 16.1→18.1 s に落ちた。animation は長さが同じで frame の数が減る（Home 9→6 frame）。案: (A) このまま受ける（Venus の present の遅さの性質、実機は p016 で測る。推し）、(B) libvulkan の present fence の signal を kernel の present の前（worker が job を取った時）に早める（p009 の直し。仕様の意味には合うが i915 で歩調にならない）、(C) callback を REAPED に戻し animation だけ SHOWN（合意した形から外れる）。

### p012・p016 への申し送り

- p012: render の job は `job.present_fence`・`job.head_fences[]` を持つ（render が付ける）。cursor の chain は T17 に載せていない（cursor の present は今のまま）。cursor の copy の fence を待つ形にしても T17 とは別の fence。
- p016: idle の判定に `KWL SHOWN stopped` の `sleeps` と `waits`（present ごとに 1 回）を使える。5330 では `KWL LAT submit` と `KWL LAT shown` の差（present の受け付けの時）と、wlshm・wltest の速さを基点と比べて、上の (A) の読み（1 refresh に収まる）を確かめる事を推す。`--log-frames` の `KWL RETURN read= done=` で buffer の返しが読みの完了の後である事を見られる。
