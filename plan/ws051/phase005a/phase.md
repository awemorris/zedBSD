<!-- awesome-plan project=zedbsd record=ws051-p005a -->

# ws051-p005a: USB-C の DP の抜き差しの検出と事象

Phase ID: `ws051-p005a`
Parent: [WS051](../ws.md)
Status: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: 5330 の UAT 2026-10-08 午後、TC2 に挿した monitor が検出され点いた（BUG-256 の resolved）。抜き差しの繰り返しと TC1 は未）（旧: in-progress（2026-10-07 P1: 実装、host 試験 PASS、build warning 0。実機（5330 の素の起動で抜き差し）は T1 への依頼を Q1 へ））
Phase disposition: normal
Queue: q847（P1）の続き。Q1 の判断 2026-10-07: p005 を a・b に分け、p005a は p004b に依存しない（hotplug.c・dp-ext の範囲）。承認はユーザー 2026-10-07「UCSIとDP alt modeって
もう動いてるんですか？シェーダコンパイラより優先してほしいです」（Q1 経由）。

## 範囲（[design.md](../design.md) §12 の M4 の前半）

1. HPD の長い pulse → detect: `intel_dp_hpd_pulse` の外部 DP の長い pulse を hotplug の work へ（IRQ_NONE）。work が DP の detect（p004a の外部 DP の probe）を走らせ、
   connector の状態と EDID を topology へ。topology の変化（接続・切断、接続のままの EDID の変化）が `GPU_DISPLAY_EVENT_CHANGE`（display の事象の sequence、
   `poll_notify`）になる。
2. 5 回の retry: Type-C の connector の detect が UNCHANGED の時の 1 秒おきの 5 回（既にある `i915_ddi_hotplug` の retry、HPD_RETRY_DELAY）がこの detect で働く。
3. 2 秒の猶予: `intel_tc_port_link_reset` を本物に。output の link が DP-alt で port を持ち partner が居なくなった時、2 秒後に見直す（戻れば link を保つ）。
4. 短い pulse（IRQ_HPD）: Type-C の DP の sink を AUX で確かめ（capabilities、branch の sink count）、変わっていれば detect、service の割り込みの vector を
   書き戻して ack する。

範囲の外（p005b、p004b の後）: 2 秒後にまだ partner が居ない link を実際に止めること（scanout 中の抜けの停止）、IRQ_HPD の link status の確かめと retrain、
service の割り込みの処理（HDCP、HDMI の link status）、S0ix。eDP の pulse は今まで通り全て無視（Linux は panel の電源がある時の短い pulse を扱う）。

## 実装（2026-10-07 P1）

- `hotplug.c`: `i915_hpd_dp_pulse_step`（eDP は無視、外部 DP の長い pulse は IRQ_NONE で hotplug の work へ、Type-C の DP の短い pulse は `drv_i915_dp_ext_pulse`、
  変化があれば IRQ_NONE、他の DP の短い pulse は handled）。`i915_hpd_intel_tc_port_link_reset`（`drv_i915_tc_link_needs_reset` → 2 秒の delayed work を arm、
  true を返し retrain しない）と `i915_hpd_tc_link_reset_work`（戻れば log、戻らなければ「reset が要る、output は止めない」の log）。work は start で用意し stop で
  同期で cancel。`i915_hpd_hotplug_recorded` の topology の公開に「接続のままで epoch（EDID）が変わった」を足した。
- `hotplug-internal.h`: `struct i915_hpd_tc_reset`、world に `tc_reset[I915_TC_PORTS]`。
- `tc.c`・`tc.h`: `drv_i915_tc_link_needs_reset`（link があり DP-alt で、live status の target の mode が違う）。
- `dp-ext.c`・`dp-ext.h`: `drv_i915_dp_ext_short_pulse`（Linux の `intel_dp_short_pulse` の sink count まで、DEVICE_SERVICE_IRQ_VECTOR 0x201・
  LINK_SERVICE_IRQ_VECTOR_ESI0 0x2005 の ack）。`dp-ext-kern.c`・`.h`: `drv_i915_dp_ext_pulse`（port の lock の下で link を 1 つ持って確かめ、返す）。

## 確かめ（2026-10-07 P1）

- host: `sh plan/ws051/tests/host-dpext.sh` → 82 checks 0 failures（短い pulse 9 件を追加: 変化なし、vector の ack の 2 つの書き戻し、capabilities の変化、AUX の失敗、
  adapter の sink count の変化、probe で見つかっていない sink）。`sh plan/ws051/tests/host-tc.sh` → host-tc 90/0（link reset の 5 件）、host-tc-tables 34/0。
- kernel: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p1-ci vmunix` warning 0。`I915_TESTS=y` の execution・display・display_ktest warning 0
  （hpd-ktest の HPD-EDP（eDP の pulse は dig-port の work だけ）の期待は変わらない）。
- style-check: 短い対称の条件演算子と tc.c の critical section の本体だけ。
- 未実施: 実機。

## 再開の情報

- T1 への依頼（Q1 経由、p004a の確認と同じ起動でまとめてよい）: 5330 を zedBSD で素に起動し、起動の後に TC2 へ USB-C の DP の monitor を挿す・抜く・挿す。
  合否: 挿すと `hpd: long hpd on DDI E: detecting`（名前は encoder の名前）→ `DP-ext TC2: connected` → `HPD-EVENT DP-2 ... disconnected -> connected (CHANGED)`、
  display の事象の sequence が 1 進む（display の UAPI の QUERY で DP-2 が connected・1920x1280。道具は `userland/tests/display-control` などで、Q1 が選ぶ）。抜くと
  `HPD-EVENT DP-2 ... connected -> disconnected`、sequence がまた進む。PHY は毎回の probe の後に返る（`after it` が DP-alt でない）。HPD の storm の log が出ない
  （probe の PHY の取り返しが pulse を起こして detect が回り続けないこと）。
- 次: p004b の code（ws113-p011a の merge の後）、その後 p005b。
