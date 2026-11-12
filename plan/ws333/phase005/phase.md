<!-- awesome-plan project=zedbsd record=ws333-p005 -->

# ws333-p005: 熱の core（前半: core・sensor の push・hw.thermal を写しから）

Status: cleared（2026-11-12 Q1 判定: core の host 試験・全 platform の build・QEMU の回帰を満たした。riscv64 virt は新しい build に無く取り下げ）
Parent: [WS333](../ws.md)
Queue: q1762-i01（P6、前半）、q2013-i01（P1、後半）

## 目的

design.md §9 の p005 の前半（p004 の UAPI に依らない所）: 熱の core（写し・sensor の push（acpi-thermal と rpi5-fan）・fan・meter・strategy と受け手（世代の 2 周目）・boost・3 つの thread・事象・ioctl の範囲の口と振り分け・`hw.thermal` を写しから 2 source で）。後半（`/dev/thermal`・`/dev/thermal-inject` と試験の受け手・`/sbin/thermal` の熱の表示）は p004 のユーザーの review の後。

## 判断（Q1）

- 2026-10-31 ユーザーから、不在の間（昼頃まで）実装できる WS は実装だけでも進め、試験できるなら試験もする、という趣旨の指示を受けた。Q333-1〜7 は Q1 が推しで進める判断をした。UAPI（`include/uapi/thermal.h`、p004）はユーザーの review が要るので作らない。
- p006 の `src/kern/cpuperf.c`（P7、まだ main に無い）は main に入る前提で書き、host 試験は模擬の受け手で行う。

## 作った物（前半）

- `src/kern/thermal.c`: [p003](../phase003/phase.md) の `include/kern/thermal.h` の口の実装。
  - 表（受け手 16・ioctl の範囲 4・sensor 32・fan 8・meter 8）は static で外さない。登録は写しの spinlock の下で埋め、数を release で公開。どの入口も最初に `thermal_prepare()`（一度だけ lock と queue を用意）を呼ぶので、`kern_thermal_init()` の前の登録（cpuperf の受け手、driver の sensor）は保たれる。
  - sensor の値の規則（WS303 §4.3）: −40〜150 °C の外は無効、最後に採った値から 20 °C 以上の跳ねは 1 回だけ捨てる、水準は 0 以下と 127 °C 以上を「無し」。
  - strategy: SET は検査（FIRMWARE・未知の値・軸の不整合・reserved）→ 写しの lock の下で世代を進め写しを置き strategy の thread に依頼 → 5 秒（kernel の tick）まで待つ。thread は 1 周ごとに始めの世代を、その世代を済ませていない受け手に登録の順に lock なしで渡し、1 周の後に新しい世代があればもう 1 周（全部の受け手に）。戻り: 受け手 0 か全部 EOPNOTSUPP → EOPNOTSUPP（strategy は記録）、acting が全部失敗 → 最初の errno、一部 → 0 と PARTIAL、上書き → 0 と SUPERSEDED、5 秒 → 0 と PENDING（結果は事象 STRATEGY）。UNCONFIRMED・EOPNOTSUPP は失敗に数えない。後から登録された受け手には今の世代を渡す。GET は CUSTOM（`is_custom` を lock なしで）・PENDING・PARTIAL と、最初に名前を返した受け手の firmware の mode の名前。
  - boost: fan ごとの段（`KERN_THERMAL_FAN_ALL` は全部）。全部を検査してから stamp（拒んだ要求は何も変えない）、strategy の thread が driver に渡し、最新の要求の結果（最初の errno）を返す。5 秒で PENDING。成功ごとに事象 BOOST。
  - poll の thread（1 秒）: 受け手の `poll`、meter の読み（32 bit 以下の計数の一周を mask で、単位の分数の余りを次へ、seq で 64 bit の和を守る）、5 秒ごとに fan の読み（STALE、HAS_RPM と HAS_LEVEL があり max(自動, boost) > 0 で rpm 0 が 3 回続けば STALLED と事象 FAN_STALLED）。
  - 見張りの thread（1 秒、優先度は既定より 2 高い、写しの lock だけ）: 30 秒読みが来ない sensor を STALE に、ZONE の critical と CPU の sensor の passive の跨ぎを事象 CRITICAL・PASSIVE（値 1 は達した、0 は 3 °C 下回って離れた）。`kern_thermal_resume()` は古さの数え始めを今に置き直す（sleep.c の hook は p016）。
  - 事象: open ごとの 64 の ring（`struct kern_thermal_reader`、mask は `1 << 種類`、溢れは古い物を捨て次の take の最初に OVERFLOW 1 つで数を伝える）。事象の lock は葉。
  - ioctl: `kern_thermal_ioctl_range_register()`（0・逆順・255 超・NULL は EINVAL、重なりは EBUSY）と `kern_thermal_ioctl_dispatch()`（group 'T' でない・範囲に無い番号は ENOTTY）。core の範囲 1〜31 の handler は後半。
  - `hw.thermal`: `kern_thermal_init()` が sysctl の source を 2 つ登録し（文脈は 0 と 16、16 sensor ずつ）、写しから `struct thermal_entry` を作る。
- `include/kern/thermal_private.h`（新）: 後半の `/dev/thermal` の device と host 試験が使う kernel の中の口（写しの GET、strategy の SET・GET、boost、受け手の結果、振り分け、事象の reader）。struct は kernel の物で UAPI の layout ではない（UAPI への写しは後半、p004 の後）。
- `src/kern/entry.c`: `net_init()` の後に `kern_thermal_init()`。
- 全 port の vmunix.mk に `src/kern/thermal.c`（sysctl.c の隣。p006 の cpuperf.c の行とは別の行）。sparcv9 は別の commit。
- `src/drivers/acpi/acpi-thermal.c`: 自分の写しと `kern_thermal_register` を外し、`kern_thermal_sensor_register` と 5 秒ごとの push（水準を先に、次に温度）に。file の先頭の開発の管理情報（WS・Phase の番号）を消した。log の行は `acpi: temperature sensor <path> flags 0x.. <n> mC`（valid の欄を外した。照合する道具は無いと grep で確かめた）。
- `src/drivers/platform/rpi5/rpi5-fan.c`: 写しの entry・lock・`thermal_copy` を外し、登録（ZONE、CPU）と読みごとの push（critical は DT の値）に。fan の登録・受け手・boost は p020。
- p006 の持ち越し（Q1 の指示で p005 の範囲に）: main（f7a9ae9e7、p006 を含む）を merge し、`src/kern/cpuperf.c` の熱の core の weak の宣言と呼ぶ所の NULL の検査を外した（core は全 port に入る）。受け手の `poll` は poll の thread が毎秒呼ぶ。
- 試験: `plan/ws333/tests/thermal-host/`（`run.sh <out>`、host の thread で 3 つの thread を動かし、時計は 20 倍速）、`plan/ws333/tests/config-amd64-boot.mk`（起動の確認の小さな image）。

## 設計からの差（理由）

- strategy の mutex は置かない: 世代の進めと写しは写しの spinlock の下で 1 つの操作で、受け手を呼ぶ間はどの lock も持たない。mutex が守る物が無い。boost も同じ。
- 見張りの thread は p005 では古さと跨ぎの事象だけ。非常の判定と停止は p016、passive の上限（poll の thread → 受け手の `limit`）は p021。
- 置き換わった世代は結果を記録しない（chooser は起こされ SUPERSEDED を返す）。GET は最新の世代が答えるまで PENDING。

## 確かめた事

- host 試験 `plan/ws333/tests/thermal-host/run.sh /tmp/p6-ws333/host`: 13 scenario 全部 PASS（登録と表の上限、ioctl の範囲と振り分け、strategy の検査・受け手の答えの組（EOPNOTSUPP・errno・PARTIAL・UNCONFIRMED）・PENDING と後の事象・SUPERSEDED と 2 周目・後から登録・CUSTOM・firmware の名前・poll の周期・init の前の登録と SET、sensor の範囲・跳ね・水準・hw.thermal の 2 つの半分、見張りの CRITICAL・PASSIVE の hysteresis と STALE と resume、fan の写し・STALE・STALLED・boost（ENOENT・EOPNOTSUPP・EINVAL・errno・ALL）、boost の PENDING、meter の一周・余り・STALE・読み手の並行、事象の mask・溢れ・subscribe・detach）。3 回続けて PASS、ASan・UBSan の build でも PASS。
- わざと壊す確かめ（/tmp の写しで 5 つ）: PARTIAL を消す、跳ねの規則を消す、世代の比較を壊す、計数の mask を外す、hysteresis を外す — どれも該当の scenario が FAIL。
- 全 20 port の vmunix（`plan/ws333/tests/build-vmunix-all.sh /tmp/p6-ws333/all`、各 make 30 分の上限）: 20 port 全部 rc=0、warning 0（amd64・intelmac・pcat・pc98・rpi4・rpi5・odroid-c5・visionfive2・zybo・armv7-virt・riscv64-virt・riscv32-virt・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・mips32-rg350・sun4u・x68k）。20 port の `thermal.o` が最後の `thermal.c` より新しい事を確かめた。arm64・armv7・i386・riscv64 の sysroot は main の checkout の物を worktree の `build/<arch>/sysroot` に symlink で読むだけ（p003 と同じ）。

- merge の後（main f7a9ae9e7 を merge した d299b5c84 と cpuperf.c の weak 外しの後の tree）: 全 20 port の vmunix をやり直して 20 port 全部 rc=0・warning 0、20 port の `thermal.o`・`cpuperf.o` が最後の source より新しい事、amd64 の vmunix に `kern_thermal_*` の weak・未定義の symbol が無い事（llvm-nm）を確かめた。cpuperf の host 試験（`plan/ws333/tests/cpuperf-host/run.sh`）PASS、熱の host 試験も merge の後に 13 scenario PASS。
- QEMU amd64（`make ZEDBSD_CONFIG=plan/ws333/tests/config-amd64-boot.mk BUILD=/tmp/p6-ws333/img disk-image`、kernel の warning 0。`BOOT_MODE=uefi-usb plan/tools/boot-test.sh`、BOOT_THEN で root の login と `sysctl hw.thermal`）: PASS。画面の PNG（`/tmp/p6-ws333/boot2/login-after.png`）を見て、login の後に `hw.thermal: sensors=0`（QEMU の q35 は ACPI の thermal zone も _TMP の device も無いので 0。hw.thermal の 2 つの source の header が正しく読めた）を確かめた。console・serial の log は判定に使っていない。
- 未実施: sensor のある機械（5330 の DPTF の participant、Pi 5）での hw.thermal の値の確かめ（実機の割当が要る）、arm64 の QEMU raspi4b の起動。

## 未実施・残り

- 後半（p004 の後）: `/dev/thermal`（cdev・node・core の ioctl 1〜31・read と poll）、`/dev/thermal-inject` と試験の受け手、`/sbin/thermal` の熱の表示。
- WMI の attach の試し直し（poll の thread、p017）、passive（p021）、非常（p016）、Pi 5 の fan の登録（p020）。
- 規約の全文の見直しは p024。

## 後半（2026-11-12 P1、q2013-i01）

2026-11-12 ユーザーが p004 の UAPI の案を承認した（Q1 経由、電力の読みは案の推し A: kernel の 1 秒の写しを誰でも読め、CPU を起こす読みは書き込みで開いた fd だけ）。その案のとおりに実装した。

### 作った物

- `include/uapi/thermal.h`: 案（`plan/ws333/proposed/include/uapi/thermal.h`）のまま置いた。`include/kern/thermal.h` と `thermal_private.h` がそれを include し、同じ名前の macro（strategy・種類・flag・事象・名前の長さ・非常の状態）の二重の定義を外した。`KERN_THERMAL_FAN_ALL` は `KERN_THERMAL_ALL`、`KERN_THERMAL_READER_RING` は `KERN_THERMAL_EVENT_QUEUE` を指す。
- `src/kern/thermal-device.c`（新）: `/dev/thermal`（cdev 0x00010008、node root:desktop 0664）と、`CONFIG_THERMAL_TEST_INJECT` の build だけの `/dev/thermal-inject`（0x00010009、node 0600、open は root だけ）。core の範囲 1〜31 の handler（request の値の全体で照合、他は ENOTTY）、kernel の写しから UAPI の record への写し、SET の判定（書き込みで開いた fd だけ。誰が書き込みで開けるかは node の mode と group）、reserved の検査、SUBSCRIBE（open ごとの mutex、mask 0 で外す）、read（blocking と O_NONBLOCK、128 byte の record）、poll、EMERGENCY_ACK（呼び手の pid）。注入: 同じ名前の sensor は更新（core の 32 の表を共有）、受け手は 4 つまで（決まった errno を決まった ms の後）、非常の道（init・direct）。`kern_thermal_device_register()` を `src/kern/entry.c` が `kern_thermal_init()` の後に呼ぶ。
- core（`src/kern/thermal.c`・`thermal-guard.c`）に proposal §6 の「kernel の側に足す事」: 事象の通し番号（全事象を数える）と sensor・fan の名前の写し、配った事象で `poll_notify()`、購読の open の上限 16（`kern_thermal_reader_attach` が ENOSPC、`kern_thermal_reader_init` を分けた）、blocking の read の待ち（`kern_thermal_reader_wait`、detach で起こす）、`READING_ARMED`（zone の critical が水準の検査を通った）、strategy の最後の結果（status と GET_STRATEGY の error、受け手の数）、`kern_thermal_strategy_set` が世代を返す、非常の始まりの時刻、試験の直の道（`kern_thermal_emergency_direct`）。
- `src/kern/devfs.c`: `thermal-inject` を 0600 に、`thermal` を root:desktop（`KERN_THERMAL_NODE_GROUP` 45）の `KERN_THERMAL_NODE_MODE` 0664 に（GPU の node の video の group と同じ形）。desktop の group は `userland/base/etc/group`（既定の利用者 `user`）と、Settings が作る利用者を入れる account-admin。
- `userland/base/init/main.c`: 起動の時に `/dev/thermal` を開いておき（O_CLOEXEC）、SIGUSR2 の後の最初の `note_emergency()` で `KERN_THERMAL_EMERGENCY_ACK` を送る（p016 の持ち越し）。
- `/sbin/thermal` の熱の表示（`userland/base/thermal/main.c`）は p013 と一緒に作った（下の p013 を見る）。

### mk の案（2026-11-12 の時点では当てていなかった。後に main に当たった、下の「締め」）

Q1 に 2026-11-12 に送った。

1. `mk/kernel.mk` の KERN_SOURCES の `src/kern/thermal-guard.c` の次に `src/kern/thermal-device.c`。
2. `mk/config.mk` の Tests の節に `# [Kernel Test: Thermal Injection] (/dev/thermal-inject)` と `CONFIG_THERMAL_TEST_INJECT ?= n`。`mk/kernel.mk` の Kernel Test の並びに `ifneq ($(filter y,$(CONFIG_THERMAL_TEST_INJECT)),)` / `KERN_CPPFLAGS += -DCONFIG_THERMAL_TEST_INJECT=1` / `endif`。
3. 新しい `userland/base/thermal/Makefile`: `PROG := thermal`、`SRCS := main.c`、`CLASS := core`、`DATA := /etc/service.d/thermal=userland/base/thermal/service`、`INSTALL_DIR := sbin`、`include ../package.mk`。
4. `/sbin/thermal` を全部の image に: `mk/userland.mk` 14 行の常に入れる並びに `thermal`（推し）、または各 platform の defaults.mk。
5. （3 の DATA で入らなければ）`mk/rootfs.mk` の service の並びに `--file /etc/service.d/thermal=userland/base/thermal/service --mode /etc/service.d/thermal=0644`。

1 が無いと main の vmunix は `kern_thermal_device_register` が未定義で link できない（commit 4901dd5691 以降）。

### 確かめた事（後半）

- host 試験 `plan/ws333/tests/thermal-host/run.sh <out>`（thermal-device.c を `-DCONFIG_THERMAL_TEST_INJECT=1` で足した）: 29 scenario 全部 PASS（前半の 22 と新しい device の 7: node と番号と照合、sensor と ARMED と ENOENT で何も写さない事、strategy の書き込みの fd（読みだけの fd は root でも EPERM、書き込みの fd は root でなくても通る）・EOPNOTSUPP・PARTIAL・最初の errno・PENDING・受け手の一覧、fan と meter と boost（読みだけの fd は EPERM）、事象の購読・record・poll・通し番号の飛び・OVERFLOW・16 の上限・blocking の read と detach で EINVAL、非常の直の道と時刻、init の ACK）。3 回続けて PASS、ASan・UBSan の build でも PASS。
- build（y の config: `CONFIG_KERN_CPU_PERF=y`・`CONFIG_DRIVER_X86_POWER_CONTROL=y`）: x86_64 の全 object を warning 0 で compile。`kern_thermal_device_register` の未定義の 1 つで link が止まる（mk の 1 が無いため）。thermal-device.c を同じ flag で手で compile（inject の有無の 2 通り、warning 0）し、link の command に足して手で link した vmunix に未定義の strong の symbol は無い。rpi5（arm64）と vf2（riscv64）の既定の config でも同じ（object は warning 0、手の link で未定義なし）。他の port は未実施。
- QEMU amd64（q35・KVM・OVMF・NVMe、PS/2 の keyboard。`plan/ws333/tests/thermal-qemu/run.sh IMAGE OUT [emergency|direct]`、画面は `plan/tools/boot-test.py` の読み取り、console・serial の log は読まない）。image は `make CONFIGMK=<y の config、CONFIG_USERLAND_PROGRAMS を小さく> rootfs` と `build/x86_64/hdd-image.img` の target で作り、vmunix は上の手の link（inject あり）、`/sbin/thermal` と `thermal-inject` は手で build して `build/x86_64/sbin/` に置いた（mk を変えないため。正式な image の作り方は mk の答えの後）:
  - get-set: PASS。`thermal` が `strategy firmware generation=0`・`emergency none`、注入の sensor が `sensor 0 zone0 kind=zone cpu=yes temperature=45.000 passive=80.000 critical=95.000 flags=valid,armed`、`thermal strategy powersave` が `generation=1 flags=-`（受け手 cpu は hypervisor の関門で unsupported、注入の受け手は ok）、`/etc/thermal.conf` に保存、`thermal restore` が generation 2、manual の 3 軸、`thermal cpu`（4 CPU、core=P）、`power: no energy meter`、boost の拒み。画面 `build/review/ws333-p005/qemu-get-set.png`。
  - emergency（init の道）: PASS。注入の zone を critical に 2 回 → guest の中で status を読み続け、SIGNALED の後の状態 `emergency init sensor=0 temperature=92.000`（init の ACK）を disk に残す → QEMU が自分で終わる → 同じ disk で起動し直し、その status と init の記録 `/var/log/thermal-emergency`（`thermal emergency at …, … ms after init started`）を画面で読んだ。画面 `build/review/ws333-p005/qemu-emergency-init.png`。
  - direct（試験の直の道）: PASS。残した status が `emergency cutting`、QEMU が自分で終わる。画面 `build/review/ws333-p005/qemu-emergency-direct.png`。
- 2026-11-12 P1（q2015-i01、desktop の group）: `thermal-host` を書き込みの fd の判定に書き直して 29 scenario（745 checks）PASS、6 回続けて PASS、ASan・UBSan の build でも PASS。fan の最初の読みの check が core の thread の速さで落ちる事が 1 回あった（読まれる前の flags を前提にしていた）ので、読まれた後の値も受けるように直した。x86_64（`CONFIG_KERN_THERMAL=y`・`CONFIG_DRIVER_X86_POWER_CONTROL=y`、inject の有無の 2 通り）と vf2 の既定の vmunix が warning 0。QEMU は [p013](../phase013/phase.md) の「確かめた事」。
- 未実施: riscv64 の virt の QEMU、`thermal watch` の QEMU（host 試験の read の道だけ）、全 20 port の build。

### 気づいた事（main の build、WS333 の外）

- `mk/platform-x86_64/bootloader.mk` が `tools/build/finalize-bios-stage2.noct` と `--path=tools/build` を使うが、file は `mk/old-tools/build/` に、`zedbuild.noct` は `mk/tools/` にある。`make disk` は bootloader で止まる。Q1 に報告した。

## 締め（2026-11-12 P1、q2016-i01）

mk の案は main に当たっていた（`mk/kernel.mk` の `CONFIG_KERN_THERMAL` の節に `thermal-device.c`、`mk/test-config.mk` の `CONFIG_THERMAL_TEST_INJECT`、`userland/base/thermal/Makefile`）。その上で確かめ直した。

### 全 platform の vmunix

`plan/ws333/tests/build-vmunix-all.sh <log dir> [platform...]` を新しい build（`PLATFORM=`・`CONFIGMK=`、`build/<platform>`）に書き直した。platform ごとに sysroot、次に variant を順に build する: default（defaults.mk のまま）、thermal（`CONFIG_KERN_THERMAL=y`、x86 は `CONFIG_DRIVER_X86_POWER_CONTROL=y` も）、x86_64 は inject（thermal に `CONFIG_THERMAL_TEST_INJECT=y`）、defaults が y の rpi4・rpi5・vf2 は nothermal（`CONFIG_KERN_THERMAL=n`）。

| platform | default | thermal | 他 |
| --- | --- | --- | --- |
| x86_64 | rc=0 warning 0（MSR の道の symbol 0） | rc=0 warning 0（`cpux86_hwp*` 6） | inject rc=0 warning 0（`thermal_inject_*` あり） |
| intelmid64 | **rc=2**（`cpu-x86-control.c` の compile: `drv_cpu_x86_wrmsr` が未宣言） | **rc=2**（`cpux86-*.c` の duplicate symbol） | 原因は mk、下の「mk の案」 |
| i386 | rc=0 warning 0 | rc=0 warning 0 | |
| pc9821 | rc=0 warning 0 | rc=0 warning 0 | |
| rpi4 | rc=0 warning 0（defaults で y） | — | nothermal rc=0 warning 0 |
| rpi5 | rc=0 warning 0（defaults で y） | — | **nothermal rc=2**（`rpi5-fan.c` の `kern_thermal_sensor_register`・`kern_thermal_sensor_levels` が未定義） |
| vf2 | rc=0 warning 0（defaults で y） | — | nothermal rc=0 warning 0 |

log は worktree の `build/tmp/all1/`・`build/tmp/all2/`（git の外）。sysroot は 7 platform とも rc=0 warning 0。旧い build の 20 port（armv7-virt・riscv64-virt・m68k ほか）は新しい build に platform が無いので build できない。

### QEMU amd64（新しい build の正式な image）

image: `make CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y CONFIG_THERMAL_TEST_INJECT=y CONFIG_GRAPHICAL_LOGIN=n CONFIG_USERLAND_PROGRAMS="login passwd su useradd account-admin id ls cat thermal sync service passkey" programs` と同じ変数で `build/x86_64/hdd-image.img`（warning 0、`check-x86_64-hdd-image: PASS`）。`/sbin/thermal` と `/etc/service.d/thermal` は `userland/base/thermal/Makefile` が入れた（rootfs の staging に `etc/service.d/thermal` を確かめた）。`thermal-inject` だけは package の外なので、`build/tmp/tinj/` に `PROG := thermal-inject` の小さな Makefile（`userland/base/package.mk` を include）を置いて `make -C build/tmp/tinj PLATFORM=x86_64` で `build/x86_64/sbin/` に入れた。vmunix は上の inject の build。

`plan/ws333/tests/thermal-qemu/run.sh build/tmp/img1/hdd-image.img build/tmp/q1 [mode]`（QEMU の名前を `p1-ws333f-thermal` に直した。画面の読み取り、console・serial の log は読まない）:

- get-set: PASS。起動の時の service の `thermal restore` が balanced を generation 1 で渡した（`strategy balanced generation=1 result=unsupported`、受け手が 0 の時）。以後の保存と restore・manual・`thermal cpu`（4 CPU、core=P）・`power: no energy meter on this machine`・boost の拒み（Operation not supported）を画面 `build/tmp/q1/get-set-after.png` で見た。
- emergency（init の道）: PASS。QEMU が自分で終わり、2 回目の起動で `emergency init sensor=0 temperature=92.000` と `/var/log/thermal-emergency` の行。
- direct: PASS。`emergency cutting sensor=0 temperature=92.000`、QEMU が自分で終わる。
- group: PASS（desktop の group の `user` は strategy を変えられ、group の外の `bob` は EPERM）。

画面は worktree の `build/tmp/q1/*.png`。見えた事（WS333 の外）: 小さな image では `mk/rootfs.mk` が入れる bluetoothd・volumed・audiod の service が program の無いまま再起動を繰り返し、console に `init: exec /sbin/... No such file` が続く。

### 未実施

- riscv64 の virt の QEMU: 新しい build に riscv64 virt の platform が無い（`mk/platform-vf2` は `HAL_BOARD_VISIONFIVE2` 固定で、QEMU に VisionFive 2 の machine は無い）。mk に platform を足さないと build できない。riscv64 の `/sbin/thermal` は vf2 の sysroot で compile まで（p013 の記録）。
- `thermal watch` の QEMU（host 試験の read の道だけ）。

### cleared の材料

- 受け入れ（design.md §9 の p005）: core の host 試験 PASS（29 scenario、q2015-i01）、全 platform の build は intelmid64 を除いて warning 0（intelmid64 は mk の重複で、WS333 の source の問題ではない）、QEMU の回帰（amd64 の 4 本）PASS。
- 残る穴は build system の側（riscv64 virt の platform、intelmid64 の mk、rpi5 の n の組）。WS333 の source に直す所は見つかっていない。

### mk の案（Q1 に送った、当てていない）

1. `mk/platform-intelmid64/vmunix.mk` の KERN_SOURCES から `cpu-x86-control.c`・`cpux86-hwp.c`・`cpux86-power.c`・`cpux86-pstate.c` の 4 行を外す（`mk/platform-x86_64/vmunix.mk` と同じ形。これらは `mk/kernel.mk` の `CONFIG_DRIVER_X86_POWER_CONTROL` の節が入れる）。
2. `mk/config.mk` の `CONFIG_DRIVER_RPI5_FAN` の注を `# [RPi5 fan and processor temperature] (/dev/thermal)` と `#  - Depends: CONFIG_KERN_THERMAL` に（hw.thermal は p022 で外れ、driver は熱の core に push する。n の組は link できない）。
3. riscv64 virt の QEMU で試すなら、riscv64 virt の platform（`mk/platform-riscv64-virt/`）が要る。WS333 の範囲の外なので、要るかは Q1 とユーザー。
