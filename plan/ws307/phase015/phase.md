<!-- awesome-plan project=zedbsd record=ws307-p015 -->

# ws307-p015 画面の lock の既定を never にし、never でも lock される不具合を直す

- 親: [WS307](../ws.md)。Queue: q1674 / q1674-i01（担当 P5、2026-10-29、worktree p5-ws307lock、基点 main 55ee43590）。
- Status: in-progress（lock の分は 2026-10-29 cleared。sleep の既定 Never（211103282）の QEMU は q1711-i01 の 3 回目で PASS（下）。Q1 の判定待ち）
- ユーザーの指示（原文、2026-10-29）:「WS307でついでに行ってほしいのですが、デスクトップがロックされるデフォルトの時間の設定を、neverをデフォルトにしてほしいです。また、neverにしても、現在は実際にはロックされてしまうみたいなので、直しておいてほしいです。」
- 範囲（Q1）: idle の後に画面を lock する時間の既定を never に（Settings の頁、compositor の既定、設定の file の既定。file に値がある利用者はその値を保つ）。never でも lock される原因を探して直す。sleep（suspend）の既定は範囲の外（変えない）。

## 調べた事（基点 55ee43590）

- Settings に「lock までの時間」の設定は無かった。Never を選べるのは Power の頁の「Sleep after」の 2 つの slider（`power.sleep.ac` 既定 30 分・`power.sleep.battery` 既定 15 分、`settings-keys.c`）だけ。
- compositor は sleep とは別に、login session で 10 分の無操作の lock を固定で持っていた: `userland/desktop/wayland/main.c` の `MAIN_LOCK_IDLE_MS`（10 分）を `--lock-idle` が無い時に `lock_idle_ms` にし、`shell.c` の tick が `lock_idle_ms` を過ぎたら `kwl_lock(server, "idle")`。この値は設定の file を見ず、`--lock-idle` の他に変える手段が無い（sessiond も `--lock-idle` を渡さない）。
- **原因**: Sleep を Never にしても、この固定の 10 分の lock は残るので、10 分の無操作で lock される。
- 他に lock する道（`kwl_lock` の呼び手）: `seat.c` の Super+L（"key"）、`home.c` の App Home の Lock Screen（"home"）、`backend-host.c` の蓋（"lid"、開けると password 無しで外れる蓋の覆い）、`sleep.c` の sleep の前の lock（"sleep-idle"・"sleep-lid"・"sleep-rest"。sleep は lock の後でだけ始まる設計）。画面の消灯（sleep の時間の半分、`kwl_screen_off`）は lock しない。時間で lock するのは shell.c の idle の lock と、sleep の時間（既定 30・15 分）で始まる sleep の前の lock の 2 つ。後者は sleep の設定（Never なら起きない）に従い、この Phase では変えない（sleep の既定を Never にするかは Q1 がユーザーに確認中）。

## 直し（commit 9db7a023b）

- 新しい設定の key `power.lock`（`settings-keys.c`、resolver compositor、INT 0〜240 分、既定 0 = never、KEPT）。desktop.conf に値が無い利用者は never、値がある利用者はその値のまま（新しい key なので既存の値は無い）。
- compositor（`wayland/settings.c`）: `power.lock` を `settings_apply_lock` で `lock_idle_ms` に（分 × 60000）。login session だけ、`--lock-idle` を渡された時はそちらが勝つ（既存の試験の `--lock-idle=20` のため）。変えた時は次の tick から（`KWL PREFERENCES key=power.lock applied value=N`）。`main.c` の固定の 10 分（`MAIN_LOCK_IDLE_MS`）を消した。`kwl.h` の注記。
- Settings（`settings/page-power.c`）: Sleep の card の下に「Screen Lock」の card、slider「Lock the screen after」（Never・1・2・5・10・15・30・60 分、端の語「Never」「1 hour」）。離すと `power.lock` を保存（`ZSETTINGS LOOK set key=power.lock`）。説明の行「The screen locks after this long without input. A sleep locks it too.」。`look.c`・`settings.h`（`lock_minutes`、既定 0）、`pages.c`（検索語に lock）。日本語（`locale/ja/settings.tr`、tr.py update）: 画面のロック・操作が無いままこの時間が過ぎると画面をロックします。スリープの時もロックします。・画面をロックするまで・1 時間。

## 試験

- host: `sh plan/tools/settings/host-store.sh build/p015-host/host-store` → 49 passed 0 failed（足した 3 つ: file に値が無い時 power.lock は 0 で未選択、241 は EINVAL、file の `power.lock=10` は 10 のまま）。`sh plan/tools/settings/host-settings.sh` → 38 passed 0 failed。`plan/tools/style-check.py` の新しい違反 0（settings.c の 2 件は元から）。`tools/i18n/tr.py check` 0 problems。
- build: `AAT_CONFIG=plan/ws307/tests/config-amd64-p015.mk plan/tools/aat/build-image.sh build/ws307-p015`（AAT の config から clang・libcxx・emacs を外した物。sysroot は main の build/amd64/sysroot の複写）→ OK、変えた file の warning 0。
- QEMU: `VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install plan/ws307/tests/p015-lock.sh build/ws307-p015/hdd-image.img OUTDIR`（Venus の guest、`plan/ws307/tests/p015-lock.py` の 3 項目、AAT の aatlib。判定は session の行と keiland-shot の PNG、SSH で。console・serial の log は使わない）。
  - **run3（2026-10-29、image は 9db7a023b の source で build、試験の script はこの記録と同じ commit の物）: PASS**（`build/ws307-p015-run3/summary.txt`）:
    - p015.default-never pass: `KEILAND-SETTINGS value key=power.lock value=0`、Power の頁の PNG（Sleep の card の下に Screen Lock、Lock the screen after: Never）、Settings を閉じた後 660 秒（前の固定の 10 分より長い）の無操作で `KWL LOCK locked` 無し、desktop の PNG。
    - p015.slider pass: Settings の slider を 1 分へ（`ZSETTINGS LOOK set key=power.lock value=1`・`KWL PREFERENCES key=power.lock applied value=1`、PNG に 1 minute）、Never へ戻す（`applied value=0`）、150 秒の無操作で lock 無し。
    - p015.locks pass: `keiland-settings set power.lock 1` の後、約 1 分の無操作で `KWL LOCK locked reason=idle user=user manual=0`、lock 画面の PNG、password で `KWL LOCK unlocked`。後で power.lock を 0 に戻した。
    - PNG: `/home/awe/zedBSD-worktrees/p5-ws307lock/build/ws307-p015-run3/png/`（p015.default-never-power-page.png・p015.default-never-after-660s.png・p015.slider-one-minute.png・p015.slider-never.png・p015.slider-after-150s.png・p015.locks-locked.png・p015.locks-unlocked.png）、記録 `records/`、session の log の写し `logs/`。
  - run1: p015.locks pass、他の 2 つは試験の側の誤り（最初の session で Settings の Welcome が動いていて App Home が launch でなく switch した）で fail。AAT の `common.before` で app を止める形に直した。
  - run2: guest の SSH が 240 秒で応えず（同じ image、host の load 約 7）。script が guest を止めた後で画面を撮れず、原因は未解析。script に SSH の失敗の時の QMP の screendump（`no-ssh.png`）を足した。run3 は同じ image で通った。
- 実機: 未実施（範囲の外）。

## 残り・Q1 へ

- sleep の既定（`power.sleep.ac` 30 分・`power.sleep.battery` 15 分）は変えていない。sleep する機械では sleep の時間で「sleep の前の lock」が起きる（`sleep.c`、設計どおり）。sleep を Never にすれば時間では lock されない。既定を Never にするかは Q1 がユーザーに確認中。
- 蓋を閉じた時の lock（"lid"）は時間の lock ではなく、開けると password 無しで外れる覆いなので変えていない。
- 残す試験: `plan/tools/settings/host-store.c` の power.lock の 3 つの check（回帰、Q1 の了承）。`plan/ws307/tests/p015-lock.sh`・`p015-lock.py`・`config-amd64-p015.mk` はこの Phase の物。

## q1698-i01（2026-10-30、P4、worktree p4-ws307o、branch agent/p4-ws307o、基点 d3abf7c27）: sleep の既定を Never に

- ユーザーの回答（2026-10-30、原文、ws.md）:「sleep の既定もneverにします。」
- 直し（commit 211103282）: `userland/desktop/wayland/sleep-rules.h` の `KWL_SLEEP_AC_MINUTES`・`KWL_SLEEP_BATTERY_MINUTES` を 30・15 → 0（compositor の既定）、`userland/desktop/settings/look.c` の `LOOK_SLEEP_AC`・`LOOK_SLEEP_BATTERY` を 0（Settings の既定）、`userland/desktop/settings-keys/settings-keys.c` の `power.sleep.ac`・`power.sleep.battery` の既定を 0（設定の file の既定、画面の lock の `power.lock` と同じ置き場所）。desktop.conf に値がある利用者はその値のまま。comment の日付の経緯を外した。sleep の時間の半分で画面を消す動き（`sleep.c`）も、時間が 0 の時は起きない（既存の `limit != 0U` の条件）。
- 試験: `plan/tools/settings/host-store.c` に `test_sleep`（値の無い file で power.sleep.ac・battery は 0 で未選択、file の 30・15 はそのまま）→ `sh plan/tools/settings/host-store.sh build/tmp/p015s-host/host-store` 53 passed 0 failed、`host-settings.sh` 38 passed 0 failed。`p015-lock.py` を 4 項目に（default-never で power.lock・sleep.ac・sleep.battery が 0、16 分（960 秒）の無操作で lock も「KWL SLEEP screen off」も無し、新しい p015.sleep-time: power.sleep.ac 2 で 90 秒の内に「KWL SLEEP screen off via=idle」、0 に戻し Esc で「KWL SLEEP screen on via=input」）、`p015-lock.sh` は 4 項目を期待。
- image: `AAT_CONFIG=plan/ws307/tests/config-amd64-p015.mk plan/tools/aat/build-image.sh build/ws307-p015s` rc 0、変えた file の warning 0（warning は openssh・openssl・libavcodec の外部の source の既存の物だけ）。
- QEMU（`VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install plan/ws307/tests/p015-lock.sh build/ws307-p015s/hdd-image.img OUT`）: **未達**。run1（`build/ws307-p015s-run1`）: SSH は通ったが 180 秒で `KWL READY` を読めず FAIL。run2（`build/ws307-p015s-run2`）: SSH が 240 秒で応えず FAIL、`no-ssh.png` は起動の splash。どちらも同じ時に clang の package の build が走り host の load が約 30 だった。同じ image を手で起動した時（load 約 10）は session が上がり `KWL READY … role=normal` を session.log に読めた（`build/tmp/p015dbg`）。原因は load の見込みだが未確定。
- 次の 1 手: host の load が低い時（他の build を止めて）に同じ command で run3。3 回目も同じなら gdbstub・QMP で起動の停止を解析する（AGENTS.md §9）。

## q1711-i01（2026-10-30、P6、worktree p6-ws307、branch agent/p6-ws307、基点 main 5aca73738）: QEMU の 3 回目

- image: `AAT_CONFIG=plan/ws307/tests/config-amd64-p015.mk plan/tools/aat/build-image.sh build/ws307-p015s`（`flock /tmp/zedbsd-image-build.lock`、sysroot は main の build/amd64/sysroot の複写）rc 0、`check-amd64-native-image: OK`。source は 211103282 を含む main 5aca73738。
- 実行: 他の build を重ねず（host の load 約 14、開始の時）、`VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install sh plan/ws307/tests/p015-lock.sh build/ws307-p015s/hdd-image.img build/ws307-p015s-run3` → **PASS**（`build/ws307-p015s-run3/summary.txt`、p6-ws307 の worktree）:
  - p015.default-never pass: power.lock・power.sleep.ac・power.sleep.battery が 0、960 秒の無操作で lock も画面の消灯も無し。Power の頁の PNG で Sleep の 2 つの slider と Screen Lock が Never（目で確かめた）。
  - p015.slider pass: 1 分と Never が適用され、150 秒で lock 無し。
  - p015.locks pass: 1 分の idle で lock、password で unlock。
  - p015.sleep-time pass: power.sleep.ac 2 分で 1 分で画面が消え、Never に戻し Esc で点いた。
  - PNG: `build/ws307-p015s-run3/png/`（8 枚）。判定は session の行と SSH の screenshot（console・serial の log は使っていない）。
- 前の 2 回の未達は host の load（clang の build と重なり load 約 30）の見込みのまま、原因は確定していない（今回は同じ script・image の形で通った）。
- 実機: 未実施（範囲の外）。Q1 の cleared の判定待ち。
