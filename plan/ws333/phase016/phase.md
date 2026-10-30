<!-- awesome-plan project=zedbsd record=ws333-p016 -->

# ws333-p016: 見張りと非常の停止

Status: cleared（2026-10-31 Q1、main a57216e81）
Parent: [WS333](../ws.md)
Queue: q1774-i01（P6）

## 目的

design.md §9 の p016 と §4.11: 見張りの非常の判定と停止の手順（signal の disposition の口、init の SIGUSR2・ACK・上書きの禁止、REBOOT・HALT の hook、SLEEP の排他と `kern_thermal_resume`、停止の道の flag、sync の助けの thread、`drv_acpi_poweroff_emergency`・platform の既定・halt、水準の検査、記録の file）。

## 判断（Q1）

- 2026-10-31 ユーザーから、不在の間（昼頃まで）実装・試験を進める趣旨の指示を受けた。
- `thermal.emergency=off`（非常の停止を止める起動の switch）は作らない。ユーザーの決定（非常の水準で電源を落とす）を止める switch なので、ユーザーに聞くまで保留（design.md §11.2 の 6、Q1 の判断）。水準の検査だけで守る。
- UAPI（`include/uapi/thermal.h`、p004）はユーザーの review が要るので作らない。

## 作った物

- `src/kern/thermal-guard.c`（新）: 非常の手順。
  - 状態 NONE → SIGNALED（init に SIGUSR2 の handler がある）／CUTTING（無い）→ INIT（init の ACK）→ CUTTING（ACK が 10 秒で来ない、または SIGUSR2 から 120 秒で電源が落ちない）→ HALTED（電源が切れない）。前にだけ進む atomic の値で、`kern_thermal_emergency_active()` はこれを読む（S0 idle との排他の 1 つの flag）。
  - 始めに事象 EMERGENCY（sensor の index と温度）と GET_STATUS の欄（`struct kern_thermal_status` の `emergency`・`emergency_sensor`・`emergency_millicelsius`）。
  - init の disposition は既存の `signal_action_set(process, SIGUSR2, NULL, &previous)`（置き換えずに読む）で足りたので signal.c に口は足していない。handler なら `process_find_ref(1)` → `signal_send_process(SIGUSR2)`。
  - `kern_thermal_emergency_ack(pid)`（thermal_private.h）: pid 1 以外は EPERM、SIGNALED でなければ EINVAL。
  - 停止の道の flag（FREE・INIT・GUARD）: init の HALT・POWEROFF・REBOOT は `kern_thermal_shutdown_claim()` で取り、guard が持っていれば EBUSY。guard は直の切りの前に取り、init が持っていれば SIGUSR2（無ければ非常の始め）から 120 秒まで待ってから取り上げる。
  - 直の切り: sync の助けの kernel thread（`mount_sync_all()`）を作り 10 秒まで待つ → `kern_platform_poweroff_emergency()`（weak。無い platform は `kern_platform_poweroff()`）→ 戻れば HALTED と `kern_platform_halt()`。
- `src/kern/thermal.c`: 判定。
  - 「2 つ続けて」は push の時に数える（受けた読みが critical 以上なら +1、下なら 0、無効・範囲外の読みも 0。跳ねで捨てた読みは数えない）。見張りは数が 2 以上の ZONE で非常。
  - 古さ: ZONE が 30 秒読まれず、最後に受けた値が critical 以上で非常。resume の後は新しい読みが来るまで古さの判定をしない（`resumed`）。
  - 水準の検査: critical が 50 °C 未満、または最初に受けた値 + 5 °C 未満なら非常の判定では「無し」とし、log を 1 行（sensor ごとに 1 回）。事象 CRITICAL の跨ぎと hw.thermal の表示は firmware の値のまま。
  - 見張りは写しの lock を離してから `kern_thermal_emergency_run()` を呼ぶ（以後この thread は非常の手順に入る）。
- `include/kern/thermal.h`: `kern_thermal_emergency_active()`・`kern_thermal_shutdown_claim()`。`include/kern/thermal_private.h`: 状態の定数、status の欄、ACK と guard の内部の口。
- `include/kern/platform.h`: `kern_platform_poweroff_emergency()`（weak）。`src/kern/platform/pcat.c` が定義（`drv_acpi_poweroff_emergency()`）。他の platform は定義せず `kern_platform_poweroff()` へ（rpi5 の poweroff は firmware の準備を呼ばないので同じ）。
- `src/drivers/acpi/acpi-event.c`・`include/drivers/acpi/acpi.h`: `drv_acpi_poweroff()` の本体を `soft_off_enter(tell_firmware)` に移し、`drv_acpi_poweroff_emergency()`（`_PTS` を評価しない、event の lock は取る）を足した。
- `src/drivers/generic/system-device.c`: HALT・POWEROFF・REBOOT を `system_stop()` に。pid 1 の確かめ → 停止の道を取る → 非常の間は POWEROFF に置き換え → `system_shutdown_prepare()` → 非常の間に poweroff が失敗したら init に戻さず halt。`KERN_SYSTEM_SLEEP` は非常の後 EBUSY。file の開発の管理情報（WS・Phase・BUG の番号）を消した。
- `src/kern/sleep.c`: user を凍らせる前に非常なら sleep を中止（result EBUSY、device "thermal"、事象 sleep.failed）。resume の終わりに `kern_thermal_resume()`。file の先頭の開発の管理情報を消した。
- `userland/base/init/main.c`: SIGUSR2 の handler（POWEROFF を要求し、以後 SIGINT・SIGTERM・制御の socket の要求で上書きしない）。猶予は SIGTERM の後 2 秒・SIGKILL の後 2 秒（進行中の待ちも縮む）。終わりの要求は POWEROFF（再試行の途中に来ても）。最初に `/var/log/thermal-emergency` へ 1 行（`thermal emergency at <UNIX 時刻>, <ms> ms after init started`、fsync）と console の 1 行。
- vmunix.mk（12 + pcat・pc98 の .o の列）に `thermal-guard.c`。sparcv9 は別の commit。
- 試験: `plan/ws333/tests/thermal-host/`（shim に process.h・signal.h・mount.h・platform.h、scenario を 9 足した）、`plan/ws333/tests/emergency-qemu/`（`emergency-qemu.sh IMAGE OUT`、`hot-zone.asl`、`qmp-watch.py`）。

## 設計からの差（理由）

- init の `EMERGENCY_ACK` の ioctl と `/dev/thermal-inject` は /dev/thermal の UAPI（p004、ユーザーの review 待ち）が無いので作れない。kernel の口と host 試験までにし、ioctl と init の呼びは p005 後半へ。それまで init の道は ACK が無く、SIGUSR2 から 10 秒で kernel の直の切り（init が先に POWEROFF に入れば init の道）になる。
- QEMU の注入は UAPI の注入の口の代わりに、QEMU の `-acpitable` で温度が上がる ThermalZone の SSDT を足し、acpi-thermal の本物の道で試す。「init に送らない道」は boot の parameter を使わない設計なので QEMU では試さず host 試験だけ。
- 非常の CRITICAL の事象は水準の検査を通さない（表示と事象は firmware の値、電源を落とす判定だけが検査を使う）。

## 確かめた事

- host 試験 `plan/ws333/tests/thermal-host/run.sh /tmp/p6-ws333/host`: 22 scenario 全部 PASS（p005 の 13 と、非常 8・passive 1）。3 回続けて PASS、ASan・UBSan の build でも PASS。非常の scenario: handler 無し（直の切り、SIGUSR2 無し、sync の後に poweroff_emergency、事象と status、ACK は EINVAL、init の claim は EBUSY）、ACK（pid 2 は EPERM、2 回目は EINVAL、10 秒を越えても切らず 120 秒で切り init から道を取り上げる）、ACK 無し（10〜30 秒で切る）、init が先に停止中（120 秒まで待つ）、sync が止まり poweroff が失敗（10 秒で進み HALTED と halt）、判定しない場合（50 °C 未満・最初の値 + 5 °C 未満・DEVICE・間の無効の読み・間の低い読み）、古さ（30 秒後）、resume の後の抑えと次の読み。
- わざと壊す確かめ（/tmp の写しで 7 つ）: 無効の読みで数を戻さない、水準の検査を外す、ACK の待ちを 120 秒に、resume の抑えを外す、passive の 5 秒を外す、guard の EBUSY を外す、sync の待ちを 100 秒に — どれも該当の scenario が FAIL。
- 全 20 port の vmunix（`plan/tools/ports/build-vmunix-all.sh /tmp/p6-ws333/all`）: 20 port 全部 rc=0・warning 0（最初の回は pcat・pc98 の vmunix.mk の .o の列に足し忘れて link の失敗、足して 2 port をやり直し rc=0・warning 0）。amd64 の `bin/init` warning 0。
- QEMU amd64（KVM）: 下の「QEMU」の節。

## QEMU

- image: `make ZEDBSD_CONFIG=plan/ws333/tests/config-amd64-boot.mk BUILD=/tmp/p6-ws333/img16 disk-image`（rc=0、warning 0、`flock /tmp/zedbsd-image-build.lock` の下）。
- `plan/ws333/tests/emergency-qemu/emergency-qemu.sh /tmp/p6-ws333/img16/hdd-image.img /tmp/p6-ws333/emq4`: **PASS**（KVM、`-cpu host`）。
  - boot 1（`-acpitable` の `hot-zone.aml`: 40 °C を 2 分、その後 10 °C ずつ上がり critical 90 °C）: QMP の SHUTDOWN 事象の reason が `guest-shutdown`（ACPI の S5 の書き。reset や crash ではない）、QMP の接続から 148 秒。最後の画面（`boot1-049.png`、3 秒ごとの撮影）に kernel の `thermal: emergency: sensor 0 at 100050 mC reached its critical level; turning the power off`、init の `init: thermal emergency: turning the power off` と `init: stopping services`。
  - boot 2（SSDT 無し、boot 1 が書いた disk）: boot-test.sh で login、root で `cat /var/log/thermal-emergency` が `thermal emergency at 1793394264, 145648 ms after init started`（`boot2/login-after.png`）。init が非常の時に `/var/log` に書け、電源断の後に残る事を確かめた（design.md §12 の 8）。boot 2 は新しい kernel の amd64 の起動の回帰（login prompt）も兼ねる。
  - 画面の PNG の写し: worktree の `build/review/ws333-p016/boot1-049.png`・`login-after.png`。console・serial の log は判定に使っていない。
  - 最初の 3 回（emq1〜emq3）は試験の script の誤り（QMP の読みの timeout、PNG の screendump、root の password と prompt の `$`）で FAIL。kernel・init の変更は無し。
- 限界: 電源が切れた道が init の POWEROFF（`_PTS` あり）か guard の直の切り（10 秒の門）かは、撮影の間隔（3 秒）と ACK の無さから区別していない。「init に送らない道」（handler 無し）は QEMU では試していない（host 試験だけ）。

## 未実施・残り

- init の `EMERGENCY_ACK` の ioctl と init の呼び、`/dev/thermal-inject` の非常の道の選び（p005 後半、p004 の後）。
- zone を持つ amd64 の実機での確かめ（実機の割当が無い。5330 は zone が無い）。
- Pi 5（DT の critical 110 °C）の実機（p020 の後）。
- 規約の全文の見直し（p024）。
