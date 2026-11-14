<!-- awesome-plan project=zedbsd record=ws353-p016 -->

# ws353-p016: 試験（QEMU の回帰と 5330 の idle）

Status: in-progress（2026-11-14 21:3x Q1、P1 q2088 で再開。q2084 は host の不調で uncleared。5330 を割り当てた）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §11。

## 目的

WS353 の全部の変更の後の tip で、idle の起床と電力、入力・描画・animation・hotplug の応答を確かめる。

## 確かめ

- QEMU（Venus、P1 の名前・port、1 本の script）: 起動（`plan/tools/boot-test.sh`）、WS309 の guest 試験（Q1 の割当）、WS099 の C1・C2、hotplug（venus-head.sh の抜き差しと出力の移動）、key repeat・pointer・touch・slide・fade、sleep と lock、game mode の出入り（window mode の present の完了の待ち、T17、が切り替えを止めない）、layer cache の焼きの後の描画、Log Out、device lost の後の終了（hotplug・shown の thread が join される。起こせる時だけ）。idle 60 s で compositor の全 thread の timeout の起床が 0（時計が見える時の分の 1 回を除く。WS352 の poll の診断）、`KWL PERF` の `timeouts` 0。
- 5330（Q1 の割当、`5330-idle.sh` の形、Debian から 1 回の UEFI の起動）: idle の package の電力と PC2 を WS333 p029（100 Hz 固定で 4.37 W・PC2 87 %）と比べる。入力（touch pad の tap・key repeat）と描画の目視の代わりに `--log-frames` の時刻。display の抜き差しは実機の HDMI があれば（無ければ未実施）。
- arm64・riscv64 の build（compositor・libvulkan・kernel）warning 0。実機（Pi・VF2）は Q1 の割当があれば。

## 受け入れ

- QEMU の全項目 PASS。5330 の idle が WS333 p029 と同等以上（下回る時は thread ごとの起床の数を添えて Q1 に）。実施しなかった物は未実施と書く。

## 依存

- p002〜p015（p013 は未決 3 の決定しだいで除いてよい。設計の推しは除く）。

## 途中の結果（2026-11-14 P1、q2084-i01、branch `agent/p1-ws353m`、基点 main ac5b3993f7）

host の disk が埋まり（14:57 頃）、続いて NVMe の書き戻しが止まった（Dirty 3 GB が約 30 分 D state の sync）ので、Q1 の指示で 16:28 に sweep を止め、host の再起動のため中断した。未完。

### build

- arm64（`config/config.rpi4.mk`）・riscv64（`config/config.vf2.mk`）: sysroot、`packages/libvulkan`・`packages/wayland` の stamp、vmunix。rc 0・warning 0（libvulkan 29・compositor 134・kernel 344/293 file を compile）。
- x86_64: sweep の各 image の build で compositor・libvulkan を含む userland の warning 0（noct の外）。

### QEMU（Venus、`build/p016/sweep.sh` が各試験を順に、成果物は `build/p016/q/`）

| 試験 | 結果 |
| --- | --- |
| qemu-deadlines | PASS（idle-30 passes=2 timeouts=0、desktop・IME の再起動、monitor、shot の timeout、`--timeout`） |
| qemu-animation | PASS（idle-60 timeouts=0、timer は network-grace 1・clock 1。Home 298/236 ms、notify、時計の前後、OSK の slide） |
| qemu-printers | PASS |
| qemu-render-wait | PASS（draw-shm 600、wltest 300、回転、bake、game、stop の数） |
| qemu-shown | PASS（returns early=0、told median 37 ms、shown-stop waits=shown） |
| ws309 scanout・grave | PASS |
| qemu-main-wake | FAIL: idle-30 passes=11（起動の後の blur の map の作りが窓に入った）、idle-after-plug-20 passes=5（最後の窓は stop の pass を含む）。timeouts は全部 0。host の負荷の時間の境と読むが、再実行で確かめていない |
| qemu-ui-deadlines | FAIL: titlebar-repeat（Ctrl+F の focus が 10 s の内に見えない）・apps-hover（hover の preview が 10 s の内に見えない）。log では focus も preview も後で出ている。他の step は PASS（idle-30 timeouts=0、layer-quiet、band 501 ms、rotate、OSK）。全 log を残す写し `build/p016/ui-diag.sh` で再実行する予定 |
| ws309 resize・rotate | 無効（disk が満ちた時刻、copy が 789 MB で切れた・mkdir の失敗）。再実行が要る |
| ws309 task | FAIL: 6 窓の 1 つの wlshm が `setup errno=5`。disk の事故の直後の run。再実行が要る |
| qemu-hotplug-follow | FAIL: serial に shell が出ない（load 14〜20 の時）。再実行が要る |
| boot-test | 未実施（`sh` で呼んだ誤り。直に実行する） |
| display-fence・hotplug-wait・present-fence・refresh-fence・answer-wake・seat-hotplug・seat-owner | 未実施（中断） |
| WS099 C1・C2、lock・Log Out（restart-guest・zdesktop-p095）、device lost | 未実施 |

### 5330

- DMI の product name が Latitude 5330（host chaos、10.0.30.3）である事を確かめた。起動はまだしていない。
- 試験の script: `plan/ws353/tests/5330-p016.sh`（5330-idle.sh の配置・起動・戻しに、cursor・入力の段 `5330-p016-guest.sh` を root で）。idle の電力と PC2 は `plan/ws352/tests/5330-idle.sh` を変えずに使う（WS352 fix4 の 4.52 W・PC2 73 % と WS333 p029 の 4.37 W・87 % と比べる）。sleep は遠隔の電源が無いので行わない。
- static の道具は `build/p016/static/`（thermal e7b5ff39a396bfc6・sysctl 6925e9671c1a6b15・wifi d3aabcc4f6062332・laytool d0a409b11d2ded51）。tree と kernel の build（`build/p016/build-5330.sh`: firmware を除く既定の program に aat-input・wlshm・wltest、kernel は素と CONFIG_INPUT_TEST_INJECT の 2 本）はまだ。
