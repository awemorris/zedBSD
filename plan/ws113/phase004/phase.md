<!-- awesome-plan project=zedbsd record=ws113-p004 -->

# ws113-p004: compositorの出力・表示モード

Parent: [WS113](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2））（旧: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: p004a は cleared（T1-357b）。p004b は q855 P1 の実装、T1-367 1) 新しい guest で displays-p004b: PASS（下の「T1-367 の判定」）。5330 のユーザーの UAT 2026-10-08「HDMIに出力されました。extendもmirrorも動いています。」（ws113-p014 の由来））（旧: planned））
Disposition: normal
Primary Milestone: MG006（WSから継承）
Queue / attempts: none / 実装未承認
Purpose / goal: 全拡張または全mirrorで複数outputを描画
Prerequisites: p003 cleared/Vulkanの実複数出力
Investigation bound: 120分の有限1 Phase Queue案。選定時にscope/時間を再照合する。

## Procedure / affected components

出力ごとのcompose/surface/swapchain/present、論理座標、mirror複製、disconnect再構成を実装。GPU操作はlibvulkanのみ。
[設計](../design.md)の対応段と[全文方針](../../standards/ws113-display.md)を契約とする。材料が変わればWSと影響する他Phaseへ同時反映し、既存Queueの実装scopeを拡張しない。

## Clearance / verification

1/2 displayの拡張とmirrorが安定、異解像度mirrorも全内容、接続/切断後も残るdisplayで継続。Linux/FreeBSD単一表示を保つ。
結果は対象環境・source revision・commands・artifactsと結びつける。既存の実機/QEMUを混同しない。未解決の前提で調査上限に達したらattemptをunclearedとし、証拠と再開条件を記録する。

## Standards / limits / evidence

[Guardrail](../../guardrail.md)、[C全文](../../coding-style.md)、[scoped full rule](../../standards/ws113-display.md)、[automation](../../standards/automation.md#ws113-multi-display-coverage-2026-10-02)を実装前に読む。formatter/style-checkは補助、意味/所有/イベント順はfull/manual review。HAL API変更は差分ごとの事前承認。compositorのGPU UAPI直接ioctl禁止。無関係なtoolchain変更、aggregate make check、既存WS089/WS099のPhase改変は含めない。

Commands/results/commit/environment/artifacts/skipped checks: 未実施（計画のみ）。Findings: [現状調査](../design.md)。Resume: prerequisiteの実出力を確認し、このPhaseだけを新Queueへ選定・承認後に開始。

## p001契約調査による詳細化（2026-10-02）

[origin p001](../phase001/phase.md)、[契約](../phase001/contracts.md)、[ID/完了比較](../phase001/identity-completion.md)、[fixture](../phase001/fixtures.md)、[WS summary](../ws.md)を入力とする。

Procedure: outputごとのcompose/swapchain/render状態とdynamic wl_outputを作る。0接続でもserver/device watcherを保持。extendedはsigned global originとhalf-open境界、mirrorは1論理desktopを各native modeへGPU aspect-fitし黒帯をopaqueに塗る。全接続集合のvalidate/stage/applyとrollback/degradedの実状態を保持する。起動/保存anchorは採択されたpolicyに従う。

Verification / resume: D01–D03/D06、H08を適用。異解像度で四隅全contentを確認、common mode/driver retiming/refresh同期を仮定しない。配置overflow、資源不足、切断中applyのtruthful snapshotを確認。p007へroot ownerを使える描画/input/output契約を渡す。

Status/dependenciesは上記のまま。未採択architecture/製品判断とactual prerequisiteを確認し、新QueueでこのPhaseだけを有限選定・承認後に実装する。q586はp001文書のみで後続sourceを許可しない。

## 採択済local port ID / native capability入力

[main採択A2とsource](../phase001/identity-completion.md)、[native capability結線](../phase001/native-contract.md)を使う。自Phaseへの影響: 保存keyはlocal port scope。connected集合を旧bootpreferredで隠さず全参加。unknown/invalid keyでもworking状態を保ち、mode/capability validate。
後続の実装権限/依存は不変。actual API番号/layout/共有callback差分は選定前にowner/main review、HAL変更なら事前承認。

## Event

2026-10-02 / ws113-multidisplay-plan-20261002-ws113-p004-created: current userの5条件・3つの追加判断をこのPhaseへ投影。planned/Queue none。GitHub body/comment/Projectへの公開は保留。

2026-10-02 / ws113-contract-design-20261002-a3-ws113-p004: p001のsource/一次仕様で明らかになった不足に合わせ、上記の自Phase procedureと検証/resumeを詳細化。D01–D03/D06、H08を適用。異解像度で四隅全contentを確認、common mode/driver retiming/refresh同期を仮定しない。配置overflow、資源不足、切断中applyのtruthful snapshotを確認。p007へroot ownerを使える描画/input/output契約を渡す。 origin/WSリンクは上記。planned/Queue noneを保持。GitHub body/comment/Projectはmainへdelivery依頼pending。

2026-10-02 / ws113-technical-choice-20261002-a3-ws113-p004: mainのdelegated technical decision messageからD-BOOT/LAYOUT/REC/AUTH/PORT通常案を採択記録。自Phase影響: 初回全extended/internal anchor、非重複/辺で連結/edge snapをoutput state/input契約へ。 [origin](../phase001/phase.md)/[詳細](../phase001/identity-completion.md)/[WS](../ws.md)。依存/Queue権限不変、main remote delivery pending。

2026-10-02 / ws113-local-port-id-20261002-a3-ws113-p004: mainのD-ID A2/旧bootpreferred技術採択messageを受領。保存keyはlocal port scope。connected集合を旧bootpreferredで隠さず全参加。unknown/invalid keyでもworking状態を保ち、mode/capability validate。 [origin](../phase001/phase.md)/[sourceと範囲](../phase001/identity-completion.md)/[WS](../ws.md)。既往eventを保存し、該当current designを更新。p001 in-progress、他Phase planned/Queue none。main remote delivery pending。

## 2026-10-05 計画（q702、ベータ2）

入力: [契約の確定](../phase001/contracts-beta2.md) D-BOOT2・D-HOTPLUG・D-RELEASE・D-STORE・D-QEMU、contracts.md §5・§6。

範囲（compositor `userland/desktop/wayland/`、GPU は libvulkan だけ）: 出力の表（token・persistent key・generation・状態 detected → validated → claimed → active → retiring → gone）、出力ごとの display の surface・swapchain・合成、global の論理座標（signed、half-open、辺で連結）、全拡張と全 mirror（mirror は各出力の native の mode へ aspect-fit、黒い帯、独立の swapchain）、device event の fence で再列挙して出力を足す・外す、0 台で server を保つ、出力ごとの `wl_output` の global（add・remove）、displays.conf の読み書き（D-STORE、明るさは p005）、設定の transaction（validate → 準備 → 再照合 → 適用 → 実 present の確認 → snapshot の publish、失敗は rollback か実状態を degraded で公開）。input の clamp を出力の集合の境界に（contracts.md §6 の共有の辺の処理）。

手順: 1) `compose.c` の 1 出力の state を出力の配列へ（swapchain・frame・damage）。2) `display.c`・`shell.c` の画面の大きさの参照を「窓の owner の出力」「論理の desktop」に分ける（窓の所属そのものは p007、p004 は全ての窓を anchor の出力に置く）。3) hotplug（fence → 再列挙 → transaction）。4) mirror の aspect-fit。5) host の試験（座標・辺の判定・transaction の validate・aspect-fit の計算、`plan/ws113/tests/host-layout.c`）。

試験: host（上の 5）。QEMU（T1、Venus の `max_outputs=2`、zdesktop `--glass`）: 拡張で 2 つの出力の PNG（`zdesktop-check.py` を出力ごとに撮れるか確かめる、QMP の screendump は head を選べる）、mirror で同じ絵、1 出力の guest で今までの回帰（boot-test、files-regress）。guest の起動に `max_outputs=2` の選択肢を足す（`plan/tools/guest/`、Q1 の許可）。実機は p008。
受け入れ: QEMU で拡張・mirror の 2 出力の PNG、1 出力の回帰 PASS、出力が 0 になっても落ちない（QEMU で起こせれば）、Linux・FreeBSD の単一 display の build を壊さない（KMS の複数出力は p010）、warning 0、規約。目安 4〜5h。依存: p003。衝突: WS099 の compositor の Phase（`compose.c`・`shell.c`・`display.c`）と同時に流さない。

### D-LIMIT の反映（2026-10-05）

anchor でない出力の swapchain の作成が `VK_ERROR_INITIALIZATION_FAILED` なら、その出力を limited にして使わずに続ける（server・他の出力は保つ、窓をその出力に置かない）。topology の変化と、他の出力の swapchain の解放の後に再び試す。QEMU の試験に「Venus の出力を上限より多くつないで、limited の出力があっても落ちない」を足すか p004 で確かめる。

## p004a: 1 出力の切り替え（設計の案、2026-10-07 q850、P2）

2026-10-07 ユーザーの N8（[ws052-p007](../../ws052/phase007/phase.md) §11、ベータ2）:「外部画面のみに切り替えて通常の利用を継続する」、蓋を開けたら内蔵へ戻す。そのための部分集合をこの Phase から分ける案（行は Q1 が ws.md に置く）。同時に 2 つ以上を出す・拡張・mirror・displays.conf・transaction は p004b に残す。

### 今の事実（main 4e0e65d1f を読んだ）

- compositor は起動時に `compose_display()`（`compose.c:766-855`）で最初の恒等変換の display を選び、その native の解像度を `server->width/height` に入れる。surface・swapchain は vkdemo の `vkdemo_display_open`（最初の display を選ぶ）で作り、`kwl_compose_output_open`・`_close` が swapchain と OS の acquire・release を持つ（handoff の時に閉じて開く道は在る）。動いている間に `server->width/height` を変える道は無い（代入は `compose.c:835` と `main.c` の option だけ）。
- `server->width/height` の参照は 27 file・約 250 箇所。大半は毎 frame・毎入力に読むだけ。起動時に大きさで作る物: glass の wallpaper と blur の image（`glass.c:596・961`）、backdrop の形（`backdrop.c:529`）、wl_output の mode と description（`protocol.c:537・594`）、xdg の configure（最大化・全画面・docked、`protocol.c:1399・1528・1636`）、画面の中央に置く surface（`display.c:278-294・422-443`）、pointer の clamp（`input.c:750-767・1004`）。
- i915 は p002 で GOP の出力（resident）だけを点け、他の出力の claim は `ENOSPC`（`display.c:2884`）。eDP を release すると console の絵が eDP に戻る（D-RELEASE）。HDMI だけに切り替えるには i915 の「点ける 1 出力の付け替え」が要る（案 p011a、下）。Venus は scanout ごとに独立に claim できる見込み（D-LIMIT の上限まで、T1 の p003 の 2 出力の試験で確かめる）。

### 設計

1. **出力の選択の口**（`compose.c`）: `compose_display` を「display を選ぶ」と「その display の size と refresh を読む」に分け、`compose->display` を任意の display にできるようにする。surface は vkdemo の関数ではなく compositor の中の `compose_surface_open(server, display)`（指定の display の mode と plane を選ぶ。plane の選び方は vkdemo と同じ）。
2. **切り替え**（新しい `output-switch.c`、`kwl_output_switch(server, VkDisplayKHR target)`）: (a) 今の出力を `kwl_compose_output_close`（device の idle → targets・swapchain・surface → release）。(b) target の size・refresh を読み、`compose->display = target`、`kwl_compose_output_open`。(c) 失敗（`VK_ERROR_INITIALIZATION_FAILED`＝D-LIMIT の limited、OUT_OF_DATE、SURFACE_LOST）なら元の display で (b) をやり直し、`KWL OUTPUT switch failed target=NAME result=R` を出す。元にも戻れなければ出力の無い状態（server は動き続け、hotplug を待つ）。(d) 成功なら `kwl_server_resize(server, width, height, refresh)`。
3. **大きさの変更**（`kwl_server_resize`、shell と各部の通知）: glass の wallpaper・blur・backdrop を作り直す、wl_output を bind した全 client に mode・geometry・description・done（`protocol.c`）、最大化・全画面・docked の窓に新しい size の configure、floating の窓は画面の内へ詰める（左上の点を新しい範囲に clamp、大きさは変えない）、pointer の位置を clamp、bar・App Home・画面の keyboard の layout の cache を捨てる、全体を damage。`KWL OUTPUT resized width=W height=H refresh_mhz=R`。
4. **数え直しと口**（`output-switch.c`）: device の hotplug の fence（p003 の `vkRegisterDeviceEventEXT`）を compositor の tick（`kwl_glass_tick` か main loop の 250 ms の周期）で `vkGetFenceStatus` で見て、signal なら新しい fence を登録 → 列挙し直す → 古い fence を壊す。結果を「内蔵（name が eDP・LVDS・DSI で始まる。A2 の displayName は connector の kind を含む）」と「外部」の一覧で持つ。ws052-p012 への口: `kwl_output_external_available(server)`（外部が 1 つ以上で、limited と覚えた物を除く）、`kwl_output_use_external(server)`・`kwl_output_use_internal(server)`。今の出力が抜かれた（SURFACE_LOST・OUT_OF_DATE、または列挙から消えた）時は残る出力へ切り替える（内蔵を先に）。
5. **起動時**: 今のまま（GOP の出力＝最初の display。保存の設定・D-BOOT2 の全拡張は p004b）。
6. **試験**: host（`plan/ws113/tests/host-output-switch.c`: 内蔵と外部の分類、切り替えの順と失敗の戻し、floating の窓の clamp の計算、resize の通知の順を stub で）。QEMU（T1、Venus の 2 出力、`VENUS_OUTPUTS=2`）: `kwl-output` の試験の口（compositor の debug の command か `keiland-system` の probe）で scanout 0 → 1 → 0 に切り替え、各 PNG（QMP の screendump の head 1）で desktop が出る、窓の大きさが新しい size、`KWL OUTPUT resized` の行。1 出力の回帰（boot-test）。実機（5330 の HDMI、p011a の後）は ws052-p012 の UAT にまとめる。

### p011a（i915 の 1 出力の付け替え、案）

Keiland の lease がどの出力にも無い時に、GOP の出力でない接続済みの出力の `GPU_DISPLAY_CLAIM` を許し、resident の pipe をその出力へ modeset し直す（eDP は pipe を止め panel の電源と backlight を落とす。HDMI の modeset は起動時の `display=hdmi` の道を動いている間に使う）。その出力の release で GOP の出力へ戻し console を出す。lease が 1 つでもある時の他の claim は今のまま `ENOSPC`（同時の 2 出力は p011）。実機（5330 の eDP と HDMI）の確認が要る。

目安: p004a 4〜6 h（host と QEMU）、p011a 3〜5 h（実機）。

### 設計の詳細（2026-10-07、source を読んで）

- **surface の display の指定**: compositor は `userland/tests/vkdemo/display.c` を link している（`wayland/Makefile:28`・`Makefile.linux:73`）。`vkdemo_display_open` の loop の本体（mode・plane・surface）を新しい public の `vkdemo_display_open_on(instance, physical, VkDisplayKHR, w, h, out)` に出し、`vkdemo_display_open` はそれを呼ぶ（振る舞いは不変）。下書き: `plan/ws113/temp/p004a-vkdemo-open-on.patch`（git に入れない、sha256 99f47872…）。
- **拡張の有効化**（`compose.c` の `compose_device`）: instance の拡張の一覧に `VK_EXT_display_surface_counter` があれば足し、device に `VK_EXT_display_control` があれば足して `vkRegisterDeviceEventEXT` を `vkGetDeviceProcAddr` で引く（Linux の libvulkan-compat など無い所では hotplug の通知なしで今の動き）。
- **glass**（`glass.c` の新しい `kwl_glass_resize`）: device の idle → `glass->wallpaper`・`glass->blurred` を `kwl_host_image_release` → `wallpaper_create`（`server->wallpaper_path` は settings.c が今の選択に保つ）。atlas・tiles は大きさに依らない。
- **窓**（`shell.c` の新しい `kwl_glass_output_resized`、`keyboard.c` の `keyboard_work_area`（3375-3452）と同じ型）: 全 client の窓（live・mapped・toplevel・desktop の icon でない）について、全画面は configure（`protocol.c:1527` が `server->width/height` を送る）、docked（`maximized`）は `docked_rect` → x・y・window_width・height → `kwl_window_send_configure` と `window_resized`、floating は `kwl_glass_fit` で今の大きさのまま新しい space の内へ。glass でない look は全画面の configure と位置の clamp だけ。log `KWL OUTPUT window surface=N state=docked|floating|fullscreen x= y= w= h=`。
- **wl_output**（`protocol.c` の新しい `kwl_output_changed`）: bind した全ての `KWL_OUTPUT` の object に `output_send` と同じ geometry・mode（current|preferred、新しい width・height・refresh）・scale・name・description・done を送り直す。
- **pointer**（`input.c`）: `server->pointer_x/y` を新しい範囲へ clamp。
- **その他の cache**: 画面の keyboard（`keyboard.c` の panel の座標は開く時に計算、開いていれば閉じる）、backdrop（`kwl_backdrop_destroy` は output の close で既に呼ばれる）、App Home・bar・stage は毎 frame に `server->width` から計算（確かめて、cache があれば捨てる）。

## p004a の実装（2026-10-07 q850、P2）

| 部分 | file |
| --- | --- |
| surface の display の指定 | `userland/tests/vkdemo/display.c`・`.h`: `vkdemo_display_open_on`（`vkdemo_display_open` はそれを呼ぶ、振る舞いは不変） |
| compositor の Vulkan | `compose.c`: instance の surface_counter と device の display_control を任意に有効化し `vkRegisterDeviceEventEXT` を引く、surface は `compose->display` に開く、`kwl_compose_display_read`（size・refresh・名前）、`compose_refresh` は size を引数に、起動の display を `boot_display` に、acquire・present の SURFACE_LOST・OUT_OF_DATE は失敗でなく `output_lost`（frame を出さない）、close で hotplug の fence を壊す。`compose.h`: 出力の追跡の欄（hotplug・displays・名前・limited・output_lost）、`KWL_COMPOSE_DISPLAYS`・`KWL_COMPOSE_NAME` |
| 切り替え | 新しい `output-switch.c`: `kwl_output_tick`（`display.c` の `kwl_schedule`、250 ms ごと。fence の signal で新しい fence → 列挙 → 古い fence、limited を忘れる、失った出力を内蔵 → 他へ）、`kwl_output_switch`（close → 別の display の native の size で open → 失敗は limited にして元へ、元も開かなければ次の hotplug を待つ → size が変われば fit → wl_output を全 client へ）、`kwl_output_external_available`・`_use_external`・`_use_internal`（ws052-p012 の R4 の口）。内蔵は名前の `:edp:`、無ければ起動の display |
| 大きさの変更 | `glass.c` の `kwl_glass_resize`（wallpaper・blur を作り直す）、`shell.c` の `kwl_glass_output_resized`（全画面は configure、docked は docked_rect で configure、floating は大きさのまま space の内へ）、`protocol.c` の `kwl_outputs_changed`（bind した wl_output に geometry・mode・scale・name・description・done）、pointer の clamp |
| QEMU の道具 | `plan/ws035/tests/zdesktop-guest.sh` の `VENUS_DISPLAY=dbus`（選ぶ時だけ、runtime の私的な session bus に QEMU の D-Bus display、既定は不変、2026-10-07 Q1 の許可）、新しい `plan/tools/guest/venus-head.sh`（`SetUIInfo` で head を挿す・抜く）、`plan/ws113/tests/output-switch-p004a.sh`（T1） |
| 試験 | `plan/ws113/tests/host-output-switch.c`・`.sh` |

## 確認（2026-10-07）

- `sh plan/ws113/tests/host-output-switch.sh` PASS（plain・ASan/UBSan: 内蔵と外部の分類（名前、無ければ起動の display）、外部への切り替えで size・fit・client への通知・pointer の clamp、内蔵へ戻る、拒否で limited と元への戻り、hotplug で新しい fence を先に・limited を忘れる、250 ms の間引き、失った出力の内蔵への移動、display 0 台で待ち次の display に開く、仮想の display の内蔵の扱い、同じ display への切り替えは何もしない）。
- build（warning 0）: zedBSD の `wayland`・`vkdemo`（`config-amd64-zdesktop.mk`）、`keiland-linux`（rc 0）。style-check は新規の違反 0。
- 未実施: QEMU（T1: `VENUS_DISPLAY=dbus VENUS_OUTPUTS=2` で `output-switch-p004a.sh`。D-Bus display で VNC が絵を取れるかは未確認なので試験は guest の行で判定）、実機（p011a の後、ws052-p012 の R4 とまとめて 5330 の UAT）。
- 制限: 画面の keyboard が開いている時の panel の座標は開き直すまで古い大きさ（p004b で）。

## T1-357 の判定（2026-10-07 Q1）

FAIL（QEMU Venus、VENUS_DISPLAY=dbus、2 出力）。列挙・2 つ目の swapchain・refresh・hotplug の fence・compositor が 2 を数える・KWL FAILED 無しは ok。1 回目は head 0 を抜いても 20 秒以内に display 1 へ移らず、retry では移ったが display 0 への戻りが無く `KWL OUTPUT displays count=1` で終わる（失敗の所が回ごとに違う）。証拠は /home/awe/zedBSD-worktrees/t1/build/t1-357-out/。

## T1-357 の解析と直し（2026-10-07 q852-i01、P2）

証拠（T1 の worktree の `build/t1-357-out/`、run/・run2/ の zdesktop.log）を読んで、原因は 2 つ（と 1 つの潜在の不具合）。

1. **試験（QEMU の仕様）**: QEMU 10.0 の `dpy_set_ui_info`（ui/console.c）は、console が前に受けた UIInfo と同じ値を無視する（`memcmp` → "nothing changed -- ignore"）。UI の client の無い D-Bus display では head 0 の console の UIInfo は 0x0 のまま（head 0 は QEMU が xres/yres で自分で有効にする）なので、1 回目の `venus-head.sh 0 off`（0x0）は何も起こさなかった。run/ では head 0 を抜いた後の hotplug が無く、20 秒後の挿し直し（1280x800、初めての変化）で初めて topology が動いた。その generation の変化で acquire が OUT_OF_DATE（`lost operation=acquire`）→ 一覧に display 0 が残る → `use_internal` で display 0 に開き直し、その行 `switch name=...display 0` が「display 0 へ戻る」に誤って一致し、大きさが変わらないので `resized width=1280` は出なかった。retry（run2/、同じ QEMU で前回の最後に head 0 が 1280x800 を受けていた）では抜く操作が効いた。
2. **compositor**: 見せている display が hotplug の後の一覧から消えても、出力を失ったと判定していなかった（判定は次の frame の acquire・present の SURFACE_LOST・OUT_OF_DATE だけ）。何も動かない desktop は frame を描かないので、run2/ の最後（head 1 を抜いた後 `displays count=1` に display 0 だけ）で移らずに終わった。run2/ の head 0 を抜いた時に移れたのは、時計などで偶然 frame が描かれたから（`PERF compose frames=2` の後の `lost operation=acquire`）。
3. **潜在（compositor）**: kernel（Venus の `display_refresh`）は topology の event ごとに全ての出力の generation を進めるので、他の display の抜き差しでも見せている display の swapchain は OUT_OF_DATE になる。その時に `output_recover` が `use_internal` を先に試すので、外部を選んでいても内蔵へ移ってしまう（run/ の display 0 への開き直しは同じ道）。

直し:

- `output-switch.c`: hotplug の fence の後の列挙で見せている display が一覧に無ければ `KWL OUTPUT lost operation=hotplug name=…` で失ったとする（frame を待たない）。`output_recover` は、見せている display がまだ一覧にあればまず同じ display に開き直す（`KWL OUTPUT reopen name=…`、移動の `switch` の行と分ける）、無い時だけ内蔵 → 他へ。列挙が読めない時（`VK_ERROR_UNKNOWN` など、列挙の途中の topology の変化）は前の一覧を保ち（`KWL OUTPUT displays unreadable result=R`）、失ったとはせず、recover の中なら次の hotplug を待つ（新しい fence は列挙より先に登録してあるので再び signal する）。`compose.h` の `output_lost` の注記を合わせた。
- `plan/ws113/tests/output-switch-p004a.sh`: 1. で head 0 にも大きさ（1280x800）を与える（後の抜く操作が変化になる）。head 0 を挿し直した後に出力が display 1 に留まる（`switch name=…display 0` がまだ無い）の確認を足した。`plan/tools/guest/venus-head.sh` の注記に QEMU の無視の規則を書いた。
- `plan/ws113/tests/host-output-switch.c`: 外部で OUT_OF_DATE（一覧に残る）→ 同じ display に開き直す・大きさは不変、読めない一覧で何も失わない、frame 無しで hotplug だけで一覧から消えた display から内蔵へ移る、を足した。

確認（2026-10-07）: `sh plan/ws113/tests/host-output-switch.sh` PASS（plain・ASan/UBSan）。build（warning 0）: `make -j16 BUILD=build/ws113-p004a ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p003.mk build/ws113-p004a/bin/wayland`（rc 0、warning 0）、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws113-p004a-linux all`（rc 0、warning 0）。`plan/tools/style-check.py` の output-switch.c・host-output-switch.c は 0 件（前後とも）、`git diff --check` ok。未実施: QEMU（T1-357b に依頼）、実機。

## T1-357b の判定（2026-10-07 Q1）

PASS（QEMU Venus、VENUS_DISPLAY=dbus・2 出力、6e8e0bb94）: head 0 1280x800・head 1 1024x768、hotplug の fence、head 0 を抜くと display 1 へ 1024x768、挿し直しても display 1 に残る、head 1 を抜くと display 0 へ 1280x800、KWL FAILED 無し。p004a の QEMU の受け入れを満たす（実機は p011a の 5330 の UAT と一緒）。

## p004b の実装（2026-10-07 q855、P1）

範囲（Q1 への 3〜8 行の送付のとおり）: anchor（今の 1 出力、compose->output）を desktop の display に保ち、他の接続済みの display を head にして同時に出す。窓・bar・入力・`server->width/height` は anchor のまま（窓の画面間の移動は p007）。

| 部分 | file |
| --- | --- |
| 配置・file の計算（Vulkan に依らない） | 新しい `displays.c`・`displays.h`: mirror の aspect-fit、拡張の wallpaper の cover（中央、比率保持）、拡張の位置の検査（大きさ・範囲 ±2^20、重なり無し EINVAL、辺の共有で全部が繋がる ENOTCONN、範囲外 ERANGE）、後から繋いだ display は一番右の右（D-HOTPLUG）、displays.conf の読み書き（`version=1`・`mode=`・`anchor=`・`place=KEY X Y`、key は空白を含めてよい（Venus の名前）、他の version は読まない、未知の key は飛ばす） |
| head | 新しい `heads.c`: head ごとに surface・swapchain（COLOR_ATTACHMENT＋TRANSFER_DST、format は anchor と同じ）・view・framebuffer・semaphore・論理の位置・wl_output の global（1000 から）。`kwl_heads_sync`（列挙の後に足す・外す、anchor・kept_off・limited を除く、拒否は limited にして次の hotplug まで）、frame（anchor の acquire の後に head の acquire（timeout 0）、anchor の render pass と shot の後に head ごとに mirror は anchor の image を vkCmdBlitImage で aspect-fit＋黒帯の clear、拡張は anchor の pass で背景色＋wallpaper の cover、submit は全ての acquire を待ち全ての rendered を signal、present は anchor の後に head ごと。拡張の head は開いた時・wallpaper が変わった時・hotplug の後だけ描く）、OUT_OF_DATE・SURFACE_LOST は head を lost にして次の look で閉じる、`kwl_displays_apply`（mode と位置を検査してから適用、拒否なら何も変えない、適用した選択を displays.conf に tmp→fsync→rename、保存の失敗は適用と別に返す）、`kwl_displays_describe`、`kwl_output_view`（wl_output の geometry の x,y と mode を display ごとに） |
| compose | `compose.c`: anchor の swapchain を常に TRANSFER_SRC で試す（mirror の元、無理なら今どおり無しで）、open で displays.conf を一度読み head を次の look で揃える、close で head を全て先に閉じる、`kwl_compose_image_quad`。`compose.h`: `struct kwl_head`、mode・anchor の位置・kept_off・config |
| 追従 | `output-switch.c`: 最初の look・hotplug の後・anchor を開き直した後・head の lost の後に `kwl_heads_sync`。蓋で外部へ移す時は内蔵（だった anchor）を kept_off（head にしない）、内蔵へ戻す時は kept_off を外す（外部は拡張の head に戻る、N8） |
| wl_output | `protocol.c`: head ごとの動的な global（registry への global・global_remove、後から bind する registry にも、閉じた head の binding は以後何も送らない、閉じた後の bind は不活性の object）、geometry に論理の位置、名前 DISPLAY-n、mode は display ごと。`kwl.h`: `output_head`、`struct kwl_output_view` |
| その他 | `glass.c` の `kwl_glass_wallpaper`、`userland/tests/vkdemo/display.c` の `vkdemo_display_create_swapchain_usage`（`vkdemo_display_create_swapchain` はそれを呼ぶ、振る舞いは不変）、Makefile 3 つ |
| 試験の口（test image だけ） | `shot.c`（ZEDBSD_TEST_SCREEN_CAPTURE=y の時だけ build）に `DISPLAYS`・`MODE extended|mirror`・`PLACE KEY X Y`、`userland/tests/keiland-shot` に `--request LINE`。製品の口は p005 の `kl_system_displays_v1` |
| 試験 | 新しい `plan/ws113/tests/host-displays.c`・`.sh`、`host-output-switch.c` に head の stub と kept_off の確認、T1 用の `displays-p004b.sh` と `config-amd64-p004b.mk` |

確認（2026-10-07）:
- `sh plan/ws113/tests/host-displays.sh` PASS（plain・ASan/UBSan: fit・cover・validate（横並び・下・角だけ・隙間・1 列の重なり・3 つ・大きさ無し・範囲外）・place_right・conf の読み書き（CRLF、未知の key、壊れた place、空白入りの key、version 2・無しの拒否、16 の上限、長すぎる key））、`sh plan/ws113/tests/host-output-switch.sh` PASS。
- build（warning 0）: `make -j16 BUILD=build/p1-wl ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p003.mk [ZEDBSD_TEST_SCREEN_CAPTURE=y] build/p1-wl/bin/wayland`（shot.c と shot-none.c の両方）、`keiland-shot`、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-wl-linux all`。style-check: 新しい file 0 件、変えた file は増えない。`git diff --check` ok。
- 未実施: QEMU（T1: `displays-p004b.sh` と p004a の回帰 `output-switch-p004a.sh`、1 出力の boot-test）、FreeBSD の build（guest の中、T1）、実機（i915 は p011 の前は head の swapchain が ENOSPC → limited で 1 出力のまま、p011 の後に 5330）。
- 制限: 拡張の head には窓が出ない（p007）。mirror は anchor の大きさの desktop を各 head へ縮小・拡大（GPU の blit、linear）。head の format が anchor と違う display は使わない（ENOTSUP → limited）。画面の keyboard の panel は anchor だけ。

## T1-365 の判定（2026-10-07 Q1、p004b）

FAIL（QEMU Venus 2 出力、T1-366 と同じ guest で p005 の後）: `displays-p004b.sh` の PLACE が `OK saved=6`（期待 saved=0）、重なる place が `ERROR errno=3`（期待 22）、displays.conf が位置を保たない、mirror の apply も saved=6、再起動で mode=extended。head 1 の抜き差し・生存は ok。回帰の output-switch-p004a・boot-test は PASS。p005 の試験の後の guest の状態の影響は切り分けていない。log: /home/awe/zedBSD-worktrees/t1/build/t1-366/out/displays-p004b.log・-retry.log。FreeBSD の backend-test の host-session・host-power が link で FAIL（`kl_backend_power_parse_outcome` が無い、ws052-p011 の後）。

## T1-365 の FAIL の原因と修正（2026-10-07 P1）

- `saved=6`: zedBSD の errno の 6 は ENOENT（`include/uapi/errno.h`、Linux の番号ではない）。`heads_save` は `$HOME/.config/keiland` を 1 段の `mkdir` で作っていたので、`.config` の無い新しい home（試験の `/tmp/p004b-home`）では folder が作れず `mkstemp` が ENOENT。displays.conf が書かれないので、place が保たれない・再起動で mode=extended に戻るのも同じ原因。p005 の試験は saved を見ないので PASS していた（p005 の変更の影響ではない）。修正: `heads.c` に `heads_mkdir`（`mkdir -p` と同じく上の段から作る、settings-store.c の store_mkdir と同じ形）。
- `ERROR errno=3`: zedBSD の EINVAL は 3。重なりは正しく EINVAL で拒否されている。試験の期待（22、Linux の番号）が誤り。`displays-p004b.sh` の期待を `errno=3` に直した（注記つき）。
- 同じ依頼の FreeBSD の backend-test: `plan/ws131/tests/host-session.sh`・`host-power.sh` の source の一覧に `libkeiland-backend-zedbsd/power-outcome.c`（ws052-p011 で session-zedbsd.c が呼ぶ）を足した（Q1 の許可）。Linux の host で `host-session: 63/63 passed`・`host-power: 6/6 passed`。
- build（warning 0）: `make -j16 BUILD=build/p1-wl ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p005.mk build/p1-wl/bin/wayland`、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-wl-linux all`。style-check: heads.c 指摘 0。
- 未実施: QEMU の再試験（T1: `displays-p004b.sh`、FreeBSD guest の backend-test の host-session・host-power）。

## T1-367 の判定（2026-10-08 Q1）

p004b: 新しく起動した guest で `displays-p004b: PASS`（QEMU Venus 2 出力）→ p004b の QEMU の受け入れを満たす。FreeBSD の backend-test 9 step PASS（host-session・host-power を含む）。
