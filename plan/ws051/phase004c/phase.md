<!-- awesome-plan project=zedbsd record=ws051-p004c -->
# ws051-p004c: GOP が USB-C の DP-alt に出していた時の引き継ぎ（M5）

Parent: [WS051](../ws.md)
Status: in-progress（2026-10-08 q902 P1 の照合: BUG-256 は resolved。5330 で GOP が USB-C の起動（蓋を閉じ USB-C の monitor だけで電源を入れる）の確認が残り）（旧: in-progress（2026-10-08 P1 q877: 実装・host 試験・kernel の build warning 0 まで。確かめは 5330 の素の起動で、BUG-256 の後））
Disposition: normal
Queue: q877（P1、2026-10-08 Q1 の承認「USB-C・DP Alt、5330 なしで進められる host の分」の 1 番）
依存: p004b（外部 DP の claim・mode・present、N1 の readout の TC の encoder）。設計: [design.md](../design.md) §6・§12 の M5・§14.2 の 3

## 範囲

GOP（firmware）の出力が Type-C の port の DDI の DP SST（DP-alt）の時、今は `I915_GOP_OTHER`（点けられない interface）として display を absent にし firmware の画面を保っていた。p004b で外部 DP の resident run ができたので、判定を「引き継ぐ」に変える。Guardrail の scanout の規則（2026-10-04 の補い）: 引き継ぎの初期化を試み、できない段では log に出して firmware の画面をそのまま保つ。

範囲の外: Type-C の HDMI（legacy の TC）と DP MST（`I915_GOP_OTHER` のまま）、TBT-alt（範囲外の決定）、BUG-256（TC の AUX の timeout、5330 の診断の log 待ち）。

## 実装（2026-10-08 P1）

- `display/internal.h`: `I915_GOP_DP_TC`（4、Type-C の port の DDI の DP SST）。
- `display/output.c`:
  - `drv_i915_gop_output_read`: port が TC1〜TC4（`I915_OUTPUT_PORT_TC1`〜`+ I915_OUTPUT_PORT_TC_COUNT`）で mode が DP SST なら `I915_GOP_DP_TC`。clone（panel と TC）は今どおり低い番号の pipe（panel）。
  - `i915_output_choose`: `I915_GOP_DP_TC` は新しい `i915_output_dp_tc_wait`。その port の DP の connector を hotplug の表から探し、claim と同じ準備（`i915_output_dp_ext`: AUX で sink を調べ、mode・link・TC PLL を計算、hardware には書かない）をする。sink が答えない（ENXIO）間は HDMI と同じく 250 ms ごとに 6 秒まで聞き直す。
  - 準備ができない時（connector が無い・sink が答えない・mode を運ぶ link が無いなど）は `output.none`。resident の run が走らないので takeover（N1 の crtc の停止）も起きず、firmware の画面が残る。log: `display output: none: the firmware's DisplayPort output (...) cannot be driven (...); the firmware's picture is kept`。
  - 準備ができた時は output が外部 DP（`I915_OUTPUT_KIND_DP_EXT`）になり、最初の present で resident の run が N1 で firmware の crtc を止めて（TC の encoder は p004b の `i915_n1_add_tc_ports` で readout に入る）、外部 DP の run（`i915_resident_dp_ext_params`、training の fallback を含む）で点け直す。
- `display.c` の absent の判定（`I915_GOP_OTHER` だけ）は変えない。`I915_GOP_DP_TC` は absent にならない。
- Type-C の readout（`drv_i915_tc_readout`）は firmware が DDI の buffer を有効にしていた port の PHY を持ち、link を 1 つ数える。なので準備の probe（get_link・put_link）はその port の PHY を手放さない（firmware の画面の途中で PHY が切れない）。

## 判断の点（案を記録、Q1 へ）

- 準備は通ったが、takeover の後の link training が全ての fallback で失敗した時は、firmware の画面は既に止まっていて戻せない（resident の run が EIO）。HDMI の引き継ぎも同じ形（準備の後の失敗は戻せない）。案: このまま（準備で AUX・EDID・link の計算まで確かめているので、残るのは training だけ）。BUG-256 の直しの後の UAT で確かめる。
- 準備の待ち（6 秒）: PD と DP mode の突入は boot の前に firmware が済ませているので、HDMI と同じ値にした。

## 確認

- host: `sh plan/ws113/tests/host-gop.sh build/ws051-p004c/host-gop` → `host-gop: 15/15 passed`。WS113 の試験の期待値を直した: DP TC1 → `I915_GOP_DP_TC`。足した場面: DP TC2・DP TC4・panel と TC1 の clone（panel）・DP MST TC1（OTHER）・TC4 の先の port（OTHER）。
- build: `make -j16 BUILD=build/ws051-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/ws051-k/vmunix`（rc 0、warning 0）。
- QEMU: 意味が無い（i915 の TC の port が無い。QEMU の boot test は takeover.c・output.c の GOP の判定を通らない）。T1 には頼まない。
- 実機（5330、未実施）: GOP が USB-C に出す起動（蓋を閉じ USB-C の monitor だけで電源を入れる、か firmware の設定）で、(1) `display output: DP on the Type-C port, the firmware's output` の後に USB-C に driver の画面が出る、(2) BUG-256 のように AUX が答えない時は `none: ... the firmware's picture is kept` で firmware の画面が残る。BUG-256 の直しの後。

## 積み残し

- Type-C の HDMI（legacy の TC）と DP MST の firmware の出力は今どおり absent。
