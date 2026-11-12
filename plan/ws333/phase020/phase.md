<!-- awesome-plan project=zedbsd record=ws333-p020 -->

# ws333-p020: Pi 5 の provider（fan の level・rpm、受け手、boost）

Status: cleared（2026-11-12 Q1 判定: Pi 5 の実機で fan の rpm・level、strategy で trip の組が変わる、boost で level が上がる、を確かめた。host 試験 19838 check。QEMU raspi4b は rpi4 の firmware の notice の欠けで未実施）
Parent: [WS333](../ws.md)

## 目的

design.md §4.7・§9 の p020（WS303 design §4.9）: `src/drivers/platform/rpi5/rpi5-fan.c` を熱の core の fan（level と rpm）・strategy の受け手（trip の組、DT より遅くしない）・boost（level の下限、実際の level = max(trip, boost)）として登録する。lock の範囲を整理する。p005 前半で sensor の push（ZONE、CPU、critical は DT の値）は済み。

## 依存

- p005（熱の core）、p016（非常の停止、Pi 5 の critical 110 °C と poweroff）。

## 受け入れ

- host 試験（模擬の fan の register と DT の trip）。
- arm64（rpi5）の vmunix の warning 0、QEMU raspi4b の起動の回帰（rpi4 の kernel）。
- Pi 5 の実機（Q1 の割当）: fan の rpm と level が読め、strategy で trip の組が変わり、boost で level が上がる（WS303 §6.3 の Pi 5）。

## 実装（2026-11-12 P1、q2017-i01、branch agent/p1-ws333g）

`src/drivers/platform/rpi5/rpi5-fan.c`・`rpi5-fan.h`（bfff4fdee9）:

- **fan**: `kern_thermal_fan_register("rpi5-fan", 0, …)`。read は HAS_LEVEL（と rpm-offset のある時 HAS_RPM）、rpm は PWM の block の tachometer の register、level は trip の level（UAPI の「自動の level」）、level_count は DT の cooling-levels の数（Pi 5 は 5）、boost_count は最高の level（4）。
- **受け手 "rpi5-fan"**: MANUAL で noise ≥ 67 の時だけ active な trip を全部 5 °C 下げる（早く回す）。PERFORMANCE・BALANCED・POWERSAVE・MANUAL の noise < 67 は DT の trip。FIRMWARE は何も書かない。critical の trip は動かさない（DT が critical を fan に map していても）。trip を DT より上げる道は無いので fan は DT より遅くならない。apply は最後の温度で level を直ちに選び直す。`firmware_name` に今の trip の範囲（`trips 50.0-75.0` の形、active な trip の最低と最高、°C の 0.1 まで）を出し、`/sbin/thermal` の `firmware` の行で strategy の効きが見える。
- **boost**: level の下限。実際の level = max(trip の level, boost)。boost は strategy と独立に残る。level_count 以上は EINVAL。温度が読めない時は今の通り全開。
- **lock**: `rpi5_fan.lock`（spinlock、LOCK_RANK_DEVICE）の下に trip の crossed・trip の shift・最後の温度・trip の level・boost・channel の level・settle と PWM の block への全ての書き込み。fan の thread・core の strategy の thread（apply・boost）・poll の thread（read）が共有する。温度の push（`kern_thermal_sensor_update`・`_levels`）と log は lock の外。
- 登録は fan のある時だけ（fan の無い板は sensor だけ）。登録は thread の作成の後、`thread_start` の前。

## 試験

- host 試験 `plan/ws333/tests/rpi5-fan-host/run.sh <out>`（dtc で 4 つの木 full・critmap・nomap・nofan を作り、kernel の `fdt.c` で読み、`rpi5-fan.c` を test に取り込む。AVS・PWM・clock の register は配列、GPIO・thread・熱の core は記録する模型。ASan・UBSan）: **19838 check、0 failure、PASS**。場面: 起動（sensor の登録・critical・clock・pin・channel・登録の名前）、trip の上り下りと hysteresis、strategy（trip の組、noise 66/67 の境、FIRMWARE は書かない、critical は不変）、「DT より遅くならない」（8 つの strategy で 20〜90 °C を 0.5 °C 刻みで上下し、独立の模型と比べる）、boost（下限・strategy を越えて残る・4 は可で 5 は EINVAL・0 で戻る）、読めない温度（全開、valid=false、strategy でも全開のまま）、map された critical の trip、DT の map が fan を指さない時の Pi 5 の既定の trip、fan の無い板・RP1 の無い板、lock の外の PWM の書き込みが 0 と lock の中の push が 0。
- 突然変異 7 つ（critical の shift、apply の選び直しの欠け、boost の無視、noise の境、read の level、boost の範囲、lock の外の書き込み）を全部検出（scratch で compile、source は変えていない）。
- build: `make PLATFORM=rpi5 CONFIGMK=config/config.rpi5.mk build/rpi5/vmunix` rc=0 warning 0（rpi5 の既定 `CONFIG_KERN_THERMAL := y`）。`make PLATFORM=rpi4 CONFIGMK=config/config.rpi4.mk build/rpi4/vmunix` rc=0 warning 0（rpi4 は `rpi5-fan.c` を build しない）。`/sbin/thermal`（rpi5、`make … userland/base/thermal`）rc=0 warning 0。
- QEMU raspi4b の起動: **未実施**（Q1 の判断で未実施でよい）。rpi4 の disk image（`make PLATFORM=rpi4 CONFIGMK=config/config.rpi4.mk CONFIG_GRAPHICAL_LOGIN=n CONFIG_USERLAND_PROGRAMS="cat chown echo login ls mkdir sh uname" disk`）が firmware の license の段で止まる: `cat: mk/platform-rpi4/rpi4-boot-firmware.notice: No such file or directory`（`mk/platform-rpi4/firmware.mk:91`）。

## mk の案（Q1 へ、当てていない）

build system の作り直しの commit 9d216cfd16 が `platform/rpi4/rpi4-boot-firmware.notice`（13 行）と `platform/rpi5/rpi5-boot-firmware.notice`（11 行）を消し、`mk/platform-rpi4/firmware.mk`・`mk/platform-rpi5/firmware.mk` が読む `mk/platform-rpi4/rpi4-boot-firmware.notice`・`mk/platform-rpi5/rpi5-boot-firmware.notice` に移していない。案: 2 つを 9d216cfd16^ から移す。

```
git show 9d216cfd16^:platform/rpi4/rpi4-boot-firmware.notice > mk/platform-rpi4/rpi4-boot-firmware.notice
git show 9d216cfd16^:platform/rpi5/rpi5-boot-firmware.notice > mk/platform-rpi5/rpi5-boot-firmware.notice
```

（odroid-c5 の `odroid-c5-bootloader.notice` も同じ commit で消えている。`mk/platform-*` が読むかは未確認。）

## Pi 5 の実機（1 本の script）

`plan/ws333/tests/pi5/pi5-thermal.sh`（板の user は user/user、sudo）:

- `PI5=<addr> sh plan/ws333/tests/pi5/pi5-thermal.sh check`: uname・kernel の cksum・`/etc/thermal.conf`・`/dev/thermal` の mode。
- `PI5=<addr> KERNEL=build/rpi5/vmunix sh … deploy`: **元の kernel を一度だけ `/boot/boot0/vmunix.p020old` に残し**、`/boot/boot0/vmunix` を入れ替え、reboot して cksum を照合。
- `PI5=<addr> THERMAL=build/rpi5/sbin/thermal sh … run`: `/tmp/p020-thermal` を root で: 状態・sensors・fans・receivers、6 つの strategy（balanced、manual 50 100 50、performance、manual 50 66 50、manual 50 67 50、powersave）ごとに状態と fans、boost を all に 3・4・0（各 12 秒の後に fans と sensors）、終わりに `/etc/thermal.conf` を元に戻して `restore`。要約は `build/tmp/pi5-thermal/run-summary.txt`。
- **戻し方**: `PI5=<addr> sh … restore`（`vmunix.p020old` を `vmunix` に写し、reboot して cksum を出す）。手で戻すなら板で `sudo cp /boot/boot0/vmunix.p020old /boot/boot0/vmunix && sync` の後に reboot。
- 遠隔の電源は無い。戻らなければ Q1 に知らせて待つ。

## Pi 5 の実機の結果（2026-11-12 22:50〜23:00 P1）

- 板: 10.0.30.5（MAC 98:fe:54:22:65:c3、`uname -m` aarch64、zedBSD、desktop の group あり）。`/sbin/thermal` は板の image に無いので、この tree の rpi5 の build（`build/rpi5/sbin/thermal`、動的 link）を `/tmp/p020-thermal` に置いて root で走らせた。
- kernel: 元の `/boot/boot0/vmunix`（cksum 2770477200、4202496 byte）を `/boot/boot0/vmunix.p020old` に残し、p020 の kernel を入れた。1 回目（cksum 3226216820）で `firmware` の名前が `trips 50000-750` に切れていた（UAPI の `KERN_THERMAL_FIRMWARE_NAME_MAX` が 16、host 試験は 32 の buffer で読んでいて見逃した）→ 名前を `trips 50.0-75.0`（15 字）に直し、host 試験の buffer を 16 にした（8fd1a957f3、host 試験 19838 check PASS、rpi5 vmunix rc=0 warning 0）。**今の板の kernel は 2 回目（cksum 3179780313）**。
- 結果（`build/tmp/pi5-thermal/run.txt`、git の外。写しは `build/review/p020-pi5-run.txt`）:
  - 温度の sensor: `sensor 0 bcm2712 cpu-thermal kind=zone cpu=yes temperature=41.900 passive=- critical=110.000 flags=valid,armed`。
  - fan: `fan 0 rpi5-fan rpm=0 rpm_max=0 level=0/5 boost=0/4 flags=known,boost`（idle の 40 °C 前後で level 0、fan は止まる）。45.75 °C で `rpm=2706 level=1/5`（直前の trip −5 °C で越えた trip が hysteresis 5 °C の内に残った物）。
  - 受け手: `receiver 1 rpi5-fan result=ok`（cpu の受け手は `unsupported`、Pi 5 の CPU の provider は無い）。
  - strategy と trip の組: firmware（起動の時）・balanced・performance・powersave・manual 50 66 50 は `firmware trips 50.0-75.0`、manual 50 100 50 と manual 50 67 50 は `firmware trips 45.0-70.0`。
  - boost: `boost all 3` の 12 秒後 `rpm=7083 level=0/5 boost=3/4`、`boost all 4` で `rpm=8886 boost=4/4`、`boost all 0` で `rpm=0 boost=0/4`（1 回目の kernel でも 6345・8588・0）。
- 画面の PNG: **未実施**。板の image に `keiland-shot`（試験の capture）が無く、host に HDMI の capture（/dev/video0）も無い。
- 板に残した物: p020 の kernel（戻し方は上の「Pi 5 の実機」の restore）、`/etc/thermal.conf`（試験の前は無く、試験が `strategy powersave` を書いた。消すのは Q1）、`/tmp/p020-thermal`・`/tmp/p020-vmunix`。

## cleared の材料（Q1 へ）

- 受け入れ: host 試験 PASS、rpi5 vmunix warning 0、Pi 5 の実機で rpm・level・trip の組・boost・温度を確かめた。
- 未達・未実施: QEMU raspi4b の起動（rpi4 の disk image が firmware の notice の欠けで作れない、mk の案）、画面の PNG。rpi4 vmunix は rc=0 warning 0（rpi4 は rpi5-fan.c を build しない）。
