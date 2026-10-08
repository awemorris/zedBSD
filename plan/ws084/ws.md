<!-- awesome-plan project=zedbsd record=ws084 -->

# WS084: i915 の firmware の画面の引き継ぎ（素の実機の UEFI の起動でデスクトップを出す）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p003（再起動の 10 回）は 5330 の実機、p004 は乖離 1・3 を直すかの Q1 の判断待ち）
Primary Milestone: MG006
Related Milestones: —
Objectives: O1
Parent: [Master](../master.md)
Queue: なし（main が実装、2026-09-29 ユーザーの指示）
Resume point: 2026-10-07 P2: p003 の reboot-loop.sh（host の dry run まで）、p004 の乖離の表・案の patch 2 つ（未適用、build warning 0）・native-decide の host 試験 14/0。乖離 1・3 を直すかは Q1 の判断待ち、実機は 5330 が届いてから。それ以前: 2026-09-29 素の 5330（demo-lcd3）で takeover → LCD の desktop が動き、ユーザー「完璧です」。残り: parity との乖離 1〜3 の整理（今は実害なし）、demo の既定の image への反映、RPS の割込み（F-054）
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-29 ユーザー）

素の 5330 を UEFI で起動すると、GOP が内蔵の LCD（pipe A）を点けたまま kernel に渡す。i915 の N0 の判定は active な pipe を
「takeover（N1）が未移植」として止め、GPU の node は display を持たず、compositor は `ENOTSUP` で終わっていた（ユーザーの実機の写真、
`i915: N0 decision: STOP ... a pipe is active (firmware display)`）。これまでの実機の試験は QEMU の passthrough で、firmware の画面が無いので
この経路を通らなかった。ユーザー:「ではWSを立ち上げて実装してください。display takeoverは以前に実験して動いた実績があり、難しくないと思います。
メインエージェントで実装してください。」

完了の条件: 素の 5330 を USB の image から UEFI で起動し、firmware の画面を引き継いで greeter（または自動の login の desktop）が内蔵の LCD に出る。
（2026-09-29 ユーザー「HDMIはいったんやめて、LCDのみの構成にします」: HDMI の LCD は WS075 の範囲に戻し、この WS の条件から外した）

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| ws084-p002 | bare metal の log（ユーザーが ssh で dmesg、下）で見つかった組み込みの不足を直す | cleared（2026-09-29。素の 5330 で takeover → LCD の desktop、操作中 24.5 present/s。下の記録） | p001 |
| [ws084-p003](phase003/phase.md) | L2: 素の 5330 で demo の既定の image（logo あり）の起動が 10 回中 10 回 desktop まで届く（`plan/ws084/tests/reboot-loop.sh`） | in-progress（2026-10-07 P2: reboot-loop.sh まで、実機は未） | p002 |
| [ws084-p004](phase004/phase.md) | parity の N1 との乖離 1〜3 を調べて記録し、直しの案を patch に（適用は main の判断）、host-native-decide-test を戻す | in-progress（2026-10-07 P2: 表・案・host 試験、判断待ち） | p003 |
| ws084-p001 | N0 が active な pipe で止まらず `takeover` の印を付け、resident の display の開始が最初の書き込みの前に N1（readout + sanitize、`intel_crtc_disable_noatomic`、release）を走らせる。以前の parity の N1 の実機の手順（`4ab09939` の `parity_lcd_kernel.c`: 画面の object を仮の framebuffer で prepare → PLL の pool を空に → readout → takeover → release）に合わせる | cleared（2026-09-29。p002 の修正と合わせて素の 5330 で動作） | — |

## p001 の記録（2026-09-29 main）

- `takeover.c` `drv_i915_native_decide`: active な pipe は止めない。他の条件（pipe の読み取りの誤り、GGTT の重なり、VT-d）は今までどおり止める。
  proceed で active な pipe があるとき `report.takeover = 1`。`internal.h` の `struct i915_native_report` に `takeover`。
- `modeset.c` `i915_resident_takeover`: resident の run の開始（preflight の前）で `display->n0.takeover` のとき、panel の cfg を
  `drv_i915_lcd_kernel_fill_cfg` で作り、仮の 640x480 の framebuffer で `drv_i915_lcd_modeset_prepare` → `drv_i915_lcd_dplls_reset` →
  `drv_i915_n1_readout` → `drv_i915_n1_takeover` → `drv_i915_n1_release` → PLL の pool と DBUF の状態を空に。readout と takeover の結果を log に出す。
- host 試験 `host-native-decide-test.c` の GOP の場面の期待値を「PROCEED、takeover」に変えた。**host 試験は未実施**: その build の道具
  （`plan/ws031/tests/display-host-lib.sh`、WS031 の片付けで削除済み）を git の履歴から出して流すと、`present.c` の `drv_i915_perf_*` が
  解決できず link できない（この変更と無関係の古さ）。
- kernel の build（main の config）: warning 0。USB の image `build/demo-takeover/hdd-image.img`。
- 実機: 未実施（ユーザーが試す）。QEMU の passthrough は firmware の画面が無いので、この経路の確認にならない。

## 危険と残り

- preflight は「pipe A の power well が切れている、vblank・underrun の割り込みが mask」を見る。takeover の後にそれが満たされないと、
  run は `LCD-B preflight` の log で止まる（そのときは takeover の後の power の扱いを直す）。
- display の core の初期化（CDCLK 等）は N0 の後、takeover の前に走る。parity の N1 も同じ順で実機で動いた。
- HDMI の主出力（display=auto で HDMI が見つかる）では、firmware の pipe A（eDP）を止めてから pipe B の HDMI を点ける。

## p002 の記録（2026-09-29 main）

ユーザーの実機（bare metal、`build/demo-hdmi3`）の dmesg:
- `N0 decision: PROCEED`、takeover の readout は `active pipes 0x1 ... DPLL-1 ... 0 kHz`、stop で `pipe_off wait timed out`・`Timeout waiting for DDI BUF to get idle`、
  preflight で `TRANSCONF 0x40000000`（enable は落ちたが state が active）→ `resident display: not started`。
- その前の P5 で `BIOS left unused DDI_IO_A power well enabled, disabling it`。parity の N1 の実機の記録（`4ab09939` の
  `plan/ws031/handover/notes/n1-implementation-state.md` の停止要因 1・4）と同じ: nogem の readout は動いている pipe の電源ドメインに参照を取らないので、
  firmware の画面の DDI IO・AUX の well が未使用に見えて落ち、pipe が止まれなくなる。P7 の `intel_power_domains_enable` の INIT の返却も同じ well を落とし得る。
- `display output: eDP panel (display=hdmi, but no HDMI sink is connected at boot: rc=13)`: HDMI の probe（EDID）が起動時の 1 回で未接続。

修正:
- `takeover.c` `drv_i915_modeset_sanitize_hw_state`: active な pipe があるとき well の sanitize をしない（takeover の readout が参照を取る）。
- `display.c` `i915_driver_register`: `n0.takeover` のとき `power_domains_enable` を遅らせる（`dprobe.power_domains_enable_deferred`、以前から宣言だけあった）。
- `modeset.c` resident の開始: takeover の成功の後に遅らせた `power_domains_enable` を行い、`n0.takeover` を消す（2 回目の lease で takeover をやり直さない）。
- `output.c`: `display=hdmi` のとき、未接続（EAGAIN）なら 250 ms ごとに最大 6 秒 probe をやり直す（USB 給電の LCD の controller が EDID に答えるまで）。`display=auto` は待たない。
- 検証: kernel と image の build（warning 0）、`plan/ws075/tests/hdmi/host-output-test.sh` 80 checks 0 failures（host の `<time.h>` を先に読む flag を足した）、
  QEMU の boot test PASS（`build/ws084-boot-test/login.png`、sshd 起動）。実機: 未実施。

### p002 の 2 回目（2026-09-29、demo-hdmi4 の実機の dmesg）

- well は残った（P7 で `DDI_IO_A hw_enabled=1`）が、takeover の stop はまだ `pipe_off wait timed out`、readout は `DPLL-1 ... 0 kHz`。
  原因: takeover の readout が encoder に `intel_ddi_get_config` を結び、combo PHY の `icl_ddi_combo_get_config`（PLL を読む）を結んでいなかった
  （`ddi.c` の「XXX: never bound」）。crtc の state に PLL が無いので sanitize が firmware の DPLL1 を止め、clock を失った pipe A が止まれない。
  parity の実機の run は preflight が RUNNING を通していたので表に出なかったと見る（推測）。
- 修正: `ddi.c` `drv_i915_lcd_ms_bind_readout` が combo PHY の port に `i915_icl_ddi_combo_get_config` を結ぶ（Linux の intel_ddi_init と同じ）。
- HDMI: 6 秒の再試験でも未接続。hotplug の割込みは DDI A だけで DDI B は無い。`hotplug.c` `drv_i915_hpd_probe_connector` が毎回 SDEISR と pin の bit を log に出す
  （live status が立たないのか、EDID が読めないのかを分ける）。
- 検証: build（warning 0）、QEMU の boot test PASS（`build/ws084-boot-test5/login.png`）。kernel 内の hotplug の試験（`tests/display/hpd-ktest.c` 等）は build の道具が無く未実施。実機: 未実施。

### p002 の 3 回目（2026-09-29、LCD のみ）

- ユーザー:「HDMIはいったんやめて、LCDのみの構成にします。その上で、過去にLCDのtakeoverに成功しているはずです。修正を続けてください。」
  → `plan/ws075/demo/config-demo-hdmi.mk` の既定を `display=edp`。
- 2 回目の修正の根拠を参照で確認: Linux の `intel_ddi_init`（履歴 `6d8ca152` の `plan/ws031/linux-parity/linux-reference/i915-src/display/intel_ddi.c` 5030 行）は
  display 11 以上の combo PHY に `icl_ddi_combo_get_config` を結ぶ。移植の `i915_sanitize_dpll_state`（`clock.c`）は `active_mask` の無い PLL を止め、
  `active_mask` は readout の `crtc_state->shared_dpll` から決まる。parity の N1（`8022d26f` の `parity_ddi_emit_glue.inc`）も `intel_ddi_get_config` を結んでおり、
  preflight を readout の前に 1 回だけ行って takeover の後は preflight 無しで再点灯していたので、止まり切らない pipe が表に出なかったと見る（推測、parity の takeover の後の register の記録は無い）。
- 検証: image の build（warning 0、`display=edp`）、QEMU の boot test PASS（`build/ws084-boot-test-lcd1/login.png`）。実機: 未実施。
- 実機の確認点: `takeover: readout` の `DPLL1` と clock が 0 でないこと、`pipe_off wait timed out` が無いこと、preflight が通り `resident display` が frame を出すこと。

### p002 の 4 回目（2026-09-29、demo-lcd1 のフリーズ）

- ユーザー:「フリーズしていて、ネットワークも届きません」「LCDはGOPのまま有効でフリーズしてます。グラフィックブートを無効にするのがいいかもね。」
- 原因（コードから）: 3 回目で結んだ `i915_icl_ddi_combo_get_config` → `i915_ddi_get_clock` の `icl_set_active_port_dpll` が移植されていない step
  （`I915_TAKEOVER_ICL_SET_ACTIVE_PORT_DPLL`、名前の記録だけ）で、`crtc_state->shared_dpll` が NULL のまま `drv_i915_dpll_get_freq(i915, NULL, ...)` が
  NULL を参照して fault。readout の中なので takeover の前、GOP の画面のまま止まる。QEMU は firmware の画面が無く active な encoder が無いのでこの経路を通らない。
- 修正: `ddi.c` `i915_ddi_get_clock` に参照の `icl_set_active_port_dpll` の 2 行（`shared_dpll`・`dpll_hw_state` の代入）。同じ経路の残り
  （`drv_i915_disable_shared_dpll` の lock は N1 の emit、combo の PLL の funcs）は確かめた。
- image: `plan/ws075/demo/build-demo-image.sh build/demo-lcd2 ZEDBSD_GRAPHICAL_BOOT=n "ZEDBSD_BOOT_EXTRA_LINES=display=edp login=graphical"`
  （logo と kmsg=quiet を外し、kernel の message を GOP の画面に出したまま。graphical boot を丸ごと切ると greeter が lease を取らず takeover が走らないので login=graphical は残す）。
- 検証: build（warning 0）、QEMU の boot test PASS（`build/ws084-boot-test-lcd2/login.png`）。実機: 未実施。

### parity の N1 との照合（2026-09-29、ユーザーの依頼「過去に動作したN1と、現在のコードを、レビューで解離がないか確認」）

比べたもの: parity の tree（`8022d26f`: `parity/lcd/parity_modeset_setup_glue.inc`・`parity_lcd_kernel.c` の `parity_lcd_kernel_n1_run`・
`display_nogem.c`・`driver_probe.c`・`probe.c` の `PARITY_N1_TEST` の分岐）と、今の `takeover.c`・`modeset.c`・`display.c`・`ddi.c`。

一致: registry と device の組み立て（`i915_n1_build_device`）、readout・takeover・release の glue、encoder の readout の hook、atomic state の crtc、
vblank の配列（停止要因 5〜7）、N0 が active な pipe で止まらないこと、P7 の initial_commit で止まらないこと（停止要因 3）。

乖離:
1. **takeover の後の preflight**: parity は preflight を readout の前に 1 回（RUNNING を正常とする）行い、takeover の後は preflight 無しで自前の modeset で再点灯。
   今は takeover の後に preflight を行い、`TRANSCONF` の state の bit が残ると拒む（実機の 1・2 回目の失敗の直接の理由）。
2. **probe 時の sanitize**: parity は firmware の画面があるとき、encoder の PLL の対応付け（DDI の clock の gate）・crtc・DPLL・未使用の well の 4 つを全て外した。
   今は p002 で well だけを外した。
3. **INIT の参照の返却**: parity は N1 の run（再点灯と保持を含む）が終わってから返した。今は takeover の直後、再点灯の前に返す
   （実機の 2 回目: `wells_on 8 -> 3`、`power well DC_off state mismatch`）。今の takeover は lease の時で DMC の読み込みの後なので、DC state の影響も受け得る。
4. **readout の PLL**: parity も `intel_ddi_get_config` を結び、`icl_set_active_port_dpll` は空の step（`n1_compat.h`）で、readout は PLL を持たなかった。
   今は p002 で Linux どおり PLL を読む（parity より参照に近いが、実機で未確認の経路。demo-lcd1 のフリーズはこの経路の空の step）。
5. 再点灯の中身: parity は firmware の framebuffer を import して readout の pipe に点けた。今は resident の run（自前の buffer、pipe A）。readout の pipe は 0 で同じ。

### 実機の結果（2026-09-29、demo-lcd2）

- ユーザー:「起動しました！ですが、1fpsくらいしか出ないです。」→ takeover と再点灯は実機で動いた（firmware の画面から LCD の表示へ）。性能は約 1 fps。
- 原因は未特定。flip の完了の待ちは 100 ms で打ち切る（`I915_LCD_FLIP_EVENT_MS`）ので、毎回 timeout しても約 10 fps のはずで合わない。GPU の実行の時間か present の経路の待ちを疑う。
  main の端末から 10.0.30.3 に届かない（No route to host）ので、dmesg の `perf:` の行をユーザーに依頼した。

### passthrough での入力の遅れの計測（2026-09-29）

ユーザー:「マウスの移動をしても描画が1秒後ですね。…QEMUでパススルーして、マウス移動から描画までの時間を計ったりできますか？」（5330 は Linux、10.0.10.25。
`~/.ssh/config` の `solaris10-man` を 10.0.10.25 に書き直した）。
- 道具: `plan/ws075/tests/hdmi/h4-ctl.py latency PIPE COUNT`（足した。tablet を 40 px 動かし、pipe の `PLANE_SURFLIVE` を passthrough の BAR 越しに xp で読んで
  次の flip までの時間。続けて入力なしの 3 秒の flip の数）。image `plan/ws075/demo/build-demo-image.sh build/demo-lcd-pt passthrough`（display=edp）、
  `plan/ws075/tests/hdmi-h4-hw.sh start|ctl|stop`、結果 `build/ws084-pt1/`。
- QEMU（passthrough、firmware の画面なし＝takeover なし）: 10/10 flip、中央値 49.1 ms（40.6〜49.5）、入力なしは 3 秒で 0 flip。
  perf の行は present 1 回の GPU 約 15 ms、flip 1〜7 ms。
- 結論: 約 1 秒の遅れは bare metal（takeover の後）だけのもの。GPU の描画や compositor の経路一般の問題ではない。疑い: takeover の後に残る状態
  （INIT の参照を再点灯の前に返す＝DC state、probe 時の sanitize の差、`kmsg` を画面に出す console）。bare metal の `perf:` の行で確かめる。

### bare metal の遅れの切り分け（2026-09-29、demo-lcd2、10.0.30.5）

- 入力: `plan/ws084/tests/evlat.c`（足した。evdev の event ごとに kernel の時刻と読めた時刻）を 90 秒、ユーザーがタッチパッドとキーボードを操作。
  event0（タッチパッド、PS/2）10443 event・約 7 ms ごと、event1（キーボード）50 event。どちらも kernel の時刻と読めた時刻の差は 0〜1 ms → **入力は遅れていない**。
  結果 `build/ws084-evlat/`。
- 表示（同じ時間の `perf:` の行）: present 2〜10/s（多くは 5〜8）、present ごとに submit 2 回、submit ごとの GPU 25〜75 ms（多くは約 30）。present（copy 7・flip 3〜7 ms）は passthrough と同程度。
  入力なしの frame の submit の GPU は bare metal 約 30 ms、passthrough 約 15 ms（約 2 倍）。
- 見立て（未確認）: (1) bare metal で GT の周波数（RPS）が上がっていない、(2) 約 140 Hz の入力に約 8 fps の描画が追いつかず frame が積もって約 1 秒遅れて見える。
- 原因（コード）: `gt-power.c` `drv_i915_rps_enable` が Linux の `rps_reset()` どおり最低の周波数（min=6、RP0=72）を要求し、周波数を上げる RPS の割込みは移植されていない
  （RPNSWREQ を書くのはここだけ）。GT は RP0 の 1/12 のまま。
- 修正: RP0 を要求する（Linux との差。RPS の割込みを移植するまで。PCODE が下げることはある）。`tests/execution/ktest-gt.c` の期待値を RPNSWREQ=RP0 に。
  image `build/demo-lcd3/hdd-image.img`（demo-lcd2 と同じ引数）、build（warning 0）、QEMU の boot test PASS。ktest は build の道具が無く未実施。実機: 未実施。

### 実機の結果（2026-09-29、demo-lcd3）

- ユーザー:「完璧です。」 dmesg（10.0.30.5）: takeover rc=0、preflight 通過、picture up。操作中の窓 5105 ms で 125 presents（24.48/s）、submit ごとの GPU 6.33 ms、
  present ごと 11.34 ms（copy 1.15、flip 5.08）。入力なしの submit の GPU 2.9 ms（demo-lcd2 は約 30 ms）。
- p001・p002 を cleared。HDMI の LCD はユーザーの判断でこの WS の外（WS075）。RPS の割込み（負荷に応じた上げ下げ）は F-054。

## 段の計画（2026-09-30 Q1）

段に分けるほどの残りは無い（takeover は 9/29 に素の 5330 で動いた）。残りは確かめだけ。
- L1（済み）: 素の 5330 で firmware の画面から Kei の LCD へ引き継ぐ。
- L2: 素の 5330 で、demo の image（最新）の起動が 10 回中 10 回、黒い画面や固まりなく greeter まで届く。ユーザーの実機の試験（demo-lcd9 以降）。

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **L2 の 10 回の起動は自動にできる見込み**: demo の image は sshd を持つ。ユーザーが USB から 1 回起動した後、エージェントが ssh で
  `reboot` を 10 回繰り返し、毎回 (1) ssh が戻るまでの時間、(2) greeter の process が居ること、(3) i915 の pipe の状態（kernel の log の
  takeover の行と、`/dev/gpu` の display の情報）を読む script（`plan/ws084/tests/reboot-loop.sh`）。画面そのものは見えないので、
  最初と最後の 1 回だけユーザーが目で確かめる。
- 注意: firmware の boot の順が USB を先に選ぶ設定であること（ユーザーの機械の設定）。reboot が NVMe の別の OS に戻るなら、この自動化はできない。
