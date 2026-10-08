<!-- awesome-plan project=zedbsd record=ws051-p004a -->

# ws051-p004a: TC の AUX・DPCD・EDID、外部 DP の object

Phase ID: `ws051-p004a`
Parent: [WS051](../ws.md)
Status: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: 5330 の UAT 2026-10-08 午後（BUG-256 の直しの後）でユーザー「USB-C DPでディスプレイ出力ができました！」、TC2。log は ../../bugs/BUG-256/。TC1 は未確認）（旧: in-progress（2026-10-07 P1: 実装、host 試験 PASS、build warning 0。実機（5330 の素の起動、TC2 の DP の monitor）は T1 への依頼を Q1 へ））
Phase disposition: normal
Queue: q847（P1）の続き。承認: ユーザー 2026-10-07「UCSIとDP alt modeってもう動いてるんですか？シェーダコンパイラより優先してほしいです」（Q1 経由、p003 → p004a → p004b）

## 範囲（[design.md](../design.md) §9・§12（M2・M6・H7）・§14.7）

1. TC の AUX: DP の AUX の転送（aux.c、Linux の intel_dp_aux_xfer）の Type-C の分岐を本物に（port の lock、mode、connected_locked、TBT の判定）。
2. 外部 DP の object（M6）: TC の port ごとの digital port・DP output・AUX channel・connector と、sink の cache（DPCD、LTTPR、識別、rate と lane、sink count、
   downstream、EDID、HDMI の有無、downstream の上限）。
3. sink の probe（Linux の intel_dp_detect の非 eDP: init_lttpr_and_dprx_caps・get_dpcd・detect_dpcd・set_edid・update_dfp）。branch device（H7）: sink count 0 は
   disconnected、HPD の付いた port は sink count、無ければ DDC、VGA・EDID 無しは unknown、他は無視。protocol converter の最小の設定（HDMI の mode の選択だけ）と
   downstream の TMDS・dot clock の検査は関数として置く（呼ぶのは p004b の modeset）。
4. 調べた後の同期の disconnect（M2）: probe の間 port に link を 1 つ持ち、終わりに返す（最後の link は PHY を即座に返す）。
5. 診断: hotplug の DP の detect（Type-C の DP connector だけ）と起動時の display の inventory で probe し、log に出す。EDID は connector の slot に入り、
   topology（QUERY の connected・preferred mode）に届く。

範囲の外（決めたこと）: LTTPR を non-transparent にする書き込み（link training と一緒に p004b）、MST（SST 固定）、DPCD の quirk、HPD の長い pulse からの
detect と事象（p005。今は起動時の inventory だけで detect する）、eDP の connector の detect（unknown のまま）、modeset での converter の設定の呼び出し（p004b）。

## 実装（2026-10-07 P1）

- `dp-ext.c`・`dp-ext.h`（新、kernel に依存しない核、`struct i915_dp_ext_env` で AUX と log を受ける）: `drv_i915_dp_ext_detect`（上の 3）、`drv_i915_dp_ext_forget`、
  `drv_i915_dp_ext_configure_converter`（DPCD 1.3 以上の branch: 0x3050 に HDMI/DVI、0x3051 = 0、0x3052 の RGB→YCbCr を消す）、`drv_i915_dp_ext_mode_valid`
  （downstream の dot clock・TMDS の範囲、8 bpc RGB）、`drv_i915_dp_ext_log`。source の rate は display version 11〜13 の 8b/10b の表を platform（HBR3）と VBT で
  制限、sink は DPCD の max と LTTPR、lane は sink・LTTPR・source（VBT）・FIA の最小。
- `dp-ext-kern.c`・`dp-ext-kern.h`（新、DP の environment）: `drv_i915_dp_ext_start`（TC の port を外部 DP の port に bind。環境は eDP device の kernel の
  backend（MMIO・power・mutex・thread）に bookkeeping を別に、eDP が始めていなければ backend を始める）、`drv_i915_dp_ext_probe`（port の lock の下で probe し EDID を
  写す）、`drv_i915_dp_ext_stop`（eDP device の fini から、backend より前）。probe は `drv_i915_tc_get_link(1 lane)` → mode と FIA の lane → DP-alt・legacy なら
  detect、他（TBT・取れない）は step PHY → `drv_i915_tc_put_link`（M2）→ log（probe の間の mode と後の mode）。
- `dp-internal.h`: `intel_digital_port` に `tc`・`tc_port`・`tc_mode`、`intel_tc_port_lock`・`unlock`・`connected_locked` を tc.c に（DP-alt・legacy だけ AUX を通す）、
  `intel_tc_port_in_tbt_alt_mode` を lock の mode から、`struct i915_dp_ext_port`・`i915_dp_ext_world` を DP world に。`dp-sink.c`: DPCD の helper の公開の口
  （`drv_i915_drm_dp_dpcd_read`・`write`・`probe`・`read_dpcd_caps`）、`drv_i915_edp_device_start`（static から公開、started なら何もしない）、
  `drv_i915_dp_env_current` は eDP が live でなければ外部 port の環境、fini で外部 port を止める。`edid-read.c`: `drv_i915_drm_probe_ddc`。
- `tc.c`・`tc.h`: `drv_i915_tc_connected_locked`（AUX の転送が持つ lock の下）。
- `hotplug.c`: DP の detect の step を Type-C の DP connector で本物に（live status → probe → EDID を slot へ、`drm_edid_connector_update`）、world に `dp_display`。
  `display.c`: TC の port の start の後に `drv_i915_dp_ext_start`。`output.c`: 起動時の inventory で DP の connector も detect（scanout はしない）。
  `platform/amd64/vmunix.mk` に dp-ext.c・dp-ext-kern.c（別の行）。

## 確かめ（2026-10-07 P1）

- host: `sh plan/ws051/tests/host-dpext.sh` → host-dpext 73 checks 0 failures（ASan・UBSan。5330 の DP-2 の monitor（DPCD 1.4、HBR2 x4、m3-5330-20261007 の EDID
  256 bytes）で connected・rate 540000（debugfs の max_link_rate と同じ）・lane 4・EDID 2 block・最初の access が 0xf0000 の wake・書き込み 0、FIA 2 lane・VBT の lane・
  不正な lane 数、LTTPR（block と DPCD 1.2 の byte 毎、rev < 1.4 は捨てる、count の bit 2 つは 0）、AUX の timeout、USB-C→HDMI の adapter（sink count 0 は
  disconnected で EDID も DDC も読まない、sink count 1・HDMI の VSDB・TMDS 25〜300 MHz・12 bpc・mode の検査、converter の 3 つの書き込み）、HPD の無い branch
  （DDC、VGA は unknown、DP は無視、DPCD 1.0）、rate（VBT、HBR3、rate の無い sink は RBR）、converter を書かない場合と失敗）。
  `sh plan/ws051/tests/host-tc.sh` → host-tc 85/0（probe の link の間 PHY が残り、最後の put で即座に返る、connected_locked が lock を取らない、の 6 件を追加）、
  host-tc-tables 34/0。
- kernel: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p1-ci vmunix` warning 0。`I915_TESTS=y I915_TEST_SET=execution`・`display`・`display_ktest`
  warning 0。
- style-check: 新しい file は `branch ? "branch" : "sink"`（短い対称の選択）だけ。変えた所は tc.c の critical section の本体（p002b と同じ形）だけ。
- 未実施: 実機（5330 の素の起動で TC2 に DP の monitor、T1 への依頼は Q1 へ）。HPD の後の detect は p005 まで無い（起動時の inventory だけ）。

## 再開の情報

- T1 への依頼（Q1 経由）: 5330 を zedBSD で素に起動（USB の image、`ZEDBSD_GRAPHICAL_BOOT=n` で kernel の message を残す）、TC2 に USB-C の DP の monitor を挿したまま。
  合否: dmesg に `i915: DP-ext TC2: connected (step done, rc 0)`、DPCD 000 の行、`rate max 540000 ... lanes 4`、`EDID: 2 block(s) ... DTD1 1920x1280`、
  `probe 1 held the port in DP-alt mode (FIA lanes 4), <DP-alt 以外> after it`、`hpd DP detect DP-2: connected`、DP-1 は disconnected。GOP の出力（eDP）がそのまま
  で DP-2 に scanout しない。monitor を抜いた起動で DP-2 は disconnected、AUX の timeout の log が出ない。
- 次: p004b（display の UAPI での出力: claim・mode・present での link training（LTTPR の mode の設定、fallback、M3）・modeset・scanout、TC PLL と TBT PLL を lcd の
  world の DPLL の pool へ、TC の encoder を N1 の registry へ、converter の設定の呼び出し）。
