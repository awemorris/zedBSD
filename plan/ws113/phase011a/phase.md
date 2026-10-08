<!-- awesome-plan project=zedbsd record=ws113-p011a -->

# ws113-p011a: i915 の 1 出力の付け替え（Keiland の指示で点ける出力を替える）

Parent: [WS113](../ws.md)
Status: test-wait（実機 5330）。2026-10-08 q902 P1 の照合: 確認の (1)〜(3) の蓋の分は BUG-255 とともにベータ3（2026-10-07 ユーザー）。付け替えの口は ws113-p014（anchor の移し替え）・ws051-p004b が使い、TC への付け替えの失敗は BUG-266（P2）（旧: in-progress（2026-10-07 q850、P2: 実装・build（warning 0、vmunix の kernel include check PASS）まで。実機 5330 の確認は UAT））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue: q850（p004a の後）
目安: 3〜5 h（実機 5330 の HDMI が中心）

## 目的

2026-10-07 ユーザーの N8（[ws052-p007](../../ws052/phase007/phase.md) §11）: 蓋を閉じたら外部の画面だけで使い続け、開けたら内蔵へ戻す。i915 は今 GOP の出力（resident）だけを点け、他の出力の claim を `ENOSPC` で断る（p002）。同時に 2 つを点ける p011 より小さく、**点ける 1 つの出力を Keiland の明示の指示で付け替える**。compositor の側は [p004a](../phase004/phase.md)。

## 今の事実（main 61004284b を読んだ）

- resident の output は起動時に 1 度だけ選ぶ（`display/output.c` の `i915_output_choose`: GOP が eDP なら eDP、HDMI（DDI B）なら HDMI を pipe B・DVI で、他の interface は none）。`display->output.hdmi` と `display->output.state`（HDMI の mode・PLL）が resident の run の入力。
- 点けるのは最初の present の時（`present.c`: worker が display の window に入り `drv_i915_lcd_kernel_resident_run` で modeset）。release の後は最後の絵を `I915_PRESENT_HOLD_MS`（10 秒）保ち、次の lease が無ければ window を出て stop の道で消す。sleep の後に HDMI が点かなければ eDP に替えてもう 1 度 run する道が既に在る（`present.c:591-595`、ws052-p009）。つまり window の外（止まっている時）なら、`display->output` を替えて次の run で別の出力を点けられる。
- 表示の ID: resident は常に `I915_DISPLAY_ID`（1）・generation 1（`display.c:2735` 付近・`present.c` の `I915_PRESENT_DISPLAY_ID`）、他の connector は `I915_DISPLAY_OTHER_ID + connector`。名前（A2 の key）は resident が eDP なら `…:edp:A`、HDMI なら `…:hdmi:B`。
- libvulkan は display を (device, display_id) で覚え、名前は最初の列挙の値のまま（`wsi.c` の `wsi_display_get`）。resident の ID が出力に依らず 1 なら、付け替えの後に同じ VkDisplayKHR が別の connector を指し名前も古いまま → **ID を connector ごとに固定する必要がある**。

## 設計

1. **ID は connector ごと**: hotplug の道に connector がある時、resident も `I915_DISPLAY_OTHER_ID + connector` の ID で出す（今の 1 は hotplug の道が無い時だけ）。generation は connector ごとの値にし、付け替えで両方を 1 進める（旧 generation の mode・surface を libvulkan が断る）。`present.c`・`control.c`（power・refresh）の ID の照合も resident の今の ID に合わせる。
2. **claim**（`i915_display_claim`）: 対象が resident でない connector の時、
   - lease が在る（`rd->owner != NULL`）→ `ENOSPC`（今と同じ。同時の 2 つは p011）。
   - 切断・generation 違い → `ENXIO`・`ESTALE`。
   - この driver が点けられない interface（DP・USB-C の DP-alt は WS051 の後）→ `EOPNOTSUPP`。
   - 点けられる（eDP の DDI A、HDMI の DDI B）→ 付け替えの item を worker に出して待つ: window の中（hold の最中）なら hold を終えて window を出る（stop の道で今の出力を消す。eDP は panel の電源と backlight も落ちる）→ `display->output` を対象に（HDMI は `i915_output_hdmi` で EDID から mode と PLL を選び直す、eDP は `output.hdmi = 0`）→ generation を進め topology の sequence を 1 進める（ACTIVE が移るので、hotplug の fence を持つ client は数え直す）→ lease を渡す。点けるのは今と同じく最初の present。
   - 付け替えの失敗（HDMI の EDID が読めない、mode が無い）→ 元の resident に戻して `EIO`（libvulkan は SURFACE_LOST、compositor は元の出力へ戻る）。
3. **release と戻し**: release は今のまま（10 秒の hold）。GOP の出力でない resident の hold が lease 無しで終わったら（compositor が終わった・切り替えに失敗した）、resident を GOP の出力に戻す（点けない。次の claim・present で点く）。D-RELEASE の「GOP の出力先へ戻る」を保つ。
4. **切断**: resident の connector が抜かれたら、今の lease の present を `ENXIO` にし（libvulkan は SURFACE_LOST）、window を出て、resident を GOP の出力に戻す。compositor（p004a）は残る出力へ切り替える。
5. **起動時**: 今のまま（GOP の出力）。

## 試験

- host: ID の対応（resident と other の ID が connector に固定、付け替えで generation が進む）と claim の判断の表（lease の有無・interface・切断・generation）を `plan/ws113/tests/` の host 試験で（display.c の判断を純粋な関数に出す）。
- 実機（5330、eDP と HDMI、`flock /tmp/i915-hw.lock`）: native の probe（`userland/tests/display-control` か新しい小さな probe）で、eDP の lease を release → HDMI を claim・present → HDMI に絵・eDP は消灯 → release して 10 秒 → GOP（eDP）に戻る。p004a の compositor の切り替え（Keiland の画面が HDMI に移り、戻る）は ws052-p012 の UAT にまとめる。
- build warning 0（vmunix の kernel include check まで）、規約。

## 依存と衝突

依存: p002（inventory・HPD、in-progress・T1 待ち）。衝突: P1 の WS051 p004a（`takeover.c`・`output.c`・`display.c` の DP-alt）。実装の前に Q1 へ知らせ、同じ file は小さく commit して早く merge を頼む。

## 実装（2026-10-07、P1 の依頼 R1〜R4 を入れた）

| 部分 | file |
| --- | --- |
| R1 出力の種類 | `display/internal.h`: `enum i915_output_kind`（PANEL・HDMI・DP_EXT）、`struct i915_display_output` の `hdmi` を `kind` に、connector（`has_connector`・`connector`）、`display->gop_output`（起動時の GOP の出力）、`window.hold_cut`。`output.hdmi` の全ての使い（output.c・display.c・present.c・control.c・backlight.c・modeset.c）を `kind` に |
| R2 準備の分岐 | `output.c` の `drv_i915_display_output_prepare`（hotplug の connector の種類ごと: eDP は panel、HDMI は DDI B だけ hotplug の道の EDID から mode と WRPLL（`i915_output_hdmi_mode`、検出し直さない）、DP は `EOPNOTSUPP`（ws051-p004b が埋める 1 か所）、他は `EOPNOTSUPP`）、`drv_i915_display_output_pipe`（種類ごとの pipe）、`drv_i915_display_output_panel` |
| R4 run の選び方 | `modeset.c` の `i915_resident_output_way`（種類ごとの params・cfg・PLL pool を 1 か所で、DP_EXT は `EOPNOTSUPP`）、`drv_i915_lcd_kernel_resident_run` はそれを使う。pipe は `drv_i915_display_output_pipe` |
| ID・generation | `display.c` の `drv_i915_display_resident_identity`（resident も connector の ID `0x100+connector` と hotplug の generation、hotplug の道が無ければ 1・1）、`i915_display_which`（resident か他か、ESTALE・ENOENT）で query・mode・claim・power・refresh を振り分け |
| 付け替え | `display.c` の `i915_display_move`（lease が在れば ENOSPC、準備 → `drv_i915_present_cut`（hold を 1 度だけ終わらせ worker が window を出て止めるのを 2 秒まで待つ）→ IRQ lock の下で `display->output` を入れ替え）。claim で他の connector なら move の後に lease を渡す（点けるのは最初の frame） |
| 戻し | `present.c` の window の終わり: sleep の後の panel への fallback は種類に依らず、`drv_i915_display_output_back`（lease が無く GOP の出力でなければ GOP の出力へ、hold が終わったと言う前に）、`hold_cut` を戻す。`drv_i915_present_hold_over` は `hold_cut` でも終わる |
| R3 切断 | `present.c` の present: resident の connector の hotplug の `connected` が 0 なら ENXIO（種類に依らない topology の判定）。generation は resident の今の値 |
| compositor（R4 の蓋） | `backend-host.c` の `kwl_lid_follow`: 内蔵に出していて外部が使えれば蓋を閉じた時に `kwl_output_use_external`（成功すれば lock も消灯もしない、蓋は閉じた扱い）、開けた時に `kwl_output_use_internal`。失敗（ENOSPC など）は今までどおり lock して眠る。`kwl.h` の `output_lid_moved` |

## 確認（2026-10-07）

- build（warning 0）: `make ZEDBSD_CONFIG=config/release/config-amd64-beta2.mk BUILD=build/ws113-p011a build/ws113-p011a/vmunix`（kernel include check PASS、amd64 vmunix check PASS）、compositor（zdesktop の config）・keiland-linux。style-check: 新しい違反 0。
- host 試験: 書いていない（判断は display.c・output.c の static で hotplug の道に依る。実機で確かめる）。
- 未実施: 実機（5330、eDP と HDMI、ユーザーの UAT）: (1) 内蔵で起動 → HDMI を挿す → 蓋を閉じる → 画面が HDMI に移り（`i915: resident display: the output moves to connector N (HDMI, Keiland's claim)`、`KWL LID external`、`KWL OUTPUT switch name=...:hdmi:B`）、内蔵は消え、HDMI で普段どおり使える。(2) 蓋を開ける → 内蔵に戻る（`KWL LID internal`）。(3) 蓋を閉じたまま HDMI を抜く → 内蔵に戻って lock して眠る。(4) compositor を終えて 10 秒後に GOP の出力に戻る（`the firmware's output (eDP panel) is the output again`）。
- 制限: 起動時の出力が HDMI だった時は backlight の device を登録しない（今までと同じ）ので、後で内蔵へ移っても明るさは変えられない。HDMI は DDI B だけ。resident の display の ID が 1 から connector の ID に変わった（libvulkan は identity で追うので影響しない）。
- 2026-10-07 追記（P1、ws051-p004b の続き、Q1 の許可）: 付け替えた出力の run の失敗で `display_failed`（恒久）を立てていた点を直した。今は GOP の出力に戻し、
  その lease の present だけ EIO、次の claim でまた点ける。`drv_i915_display_output_back` の worker の上の `mutex_lock` は `mutex_trylock` に（park・hold の
  終わりの deadlock を避ける）。詳細は [ws051-p004b](../../ws051/phase004b/phase.md) の記録。
