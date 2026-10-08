<!-- awesome-plan project=zedbsd record=ws113-p011 -->

# ws113-p011: i915 の 2 つ目の出力（Keiland の指示での scanout）

Parent: [WS113](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。抜きの head の release は p008 へ）（旧: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: 5330 のユーザーの UAT 2026-10-08「HDMIに出力されました。extendもmirrorも動いています。」（ws113-p014 の由来）で 2 出力の同時は実機で動いた。手順 (3) の HDMI の抜きの head の release・DBUF の log は未確認）（旧: in-progress（2026-10-07 q856-i01、P2: 設計・敵対的 review の反映・実装・build（warning 0）・host の規則の試験まで。実機 5330 の eDP + HDMI は未）））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue: q856（q856-i01、BUG-254 の後）
目安: 4〜6h（実機の比重が大きい）

## 目的

GOP の出力先とは別の接続済みの出力（初回の fixture は 5330 の eDP + HDMI、D-PORT）を、Keiland が libvulkan の経路で claim・present した時だけ、2 つ目の pipe で同時に scanout する（Guardrail の scanout の規則、[契約の確定](../phase001/contracts-beta2.md) D-GOP・D-RELEASE）。

## 範囲

- 単一の `rd`・lease・plane の状態を出力ごとに分ける（pipe・transcoder・DPLL・plane・watermark・帯域の割り当てを接続の集合で validate、他の出力が使う資源を奪わない）。2026-09-20 の診断（eDP pipe A/DPLL0 + HDMI pipe B/DPLL1、[report](../../ws031/handover/expert-reports/report-e123b-dual.md)）の手順を production の modeset に。
- `GPU_DISPLAY_CLAIM`（GOP の出力先でない display_id）で modeset の準備、最初の `PRESENT` で点灯、`RELEASE` で消灯（D-RELEASE）。GOP の出力先の RELEASE は console に戻る（今のまま）。
- 切断: その出力の lease と present を失効させ、新しい submit を拒み、scanout が読む buffer を退役するまで保持。残る出力は続ける（H07・H08）。
- mode は列挙した native の mode だけ（retiming は範囲外、偽の成功を返さない）。

## 受け入れ

実機（5330、eDP + HDMI、i915 の lock）: 小さい native の probe（`plan/ws113/tests/`）で eDP と HDMI に別々の lease で同時に present し、両方の画面に別の絵（ユーザーの目視か写真）、HDMI を抜いても eDP が続き、差し直して claim し直すと HDMI が戻る。RELEASE で HDMI が消える。host の試験（資源の割り当て・lease の寿命）、build warning 0（vmunix の kernel include check まで）、規約。

## 依存と衝突

依存: p002（inventory・規則）。衝突: WS051（DP-alt は同じ modeset の経路を使う）、WS075・WS084 の i915 の Phase。実機の時間はユーザーと（p008 とまとめてもよい）。

### D-LIMIT の反映（2026-10-05）

資源（pipe・transcoder・DPLL・帯域）が足りなくて 2 つ目（以降）を点けられない時の claim は `ENOSPC`（p002 の暫定の「一度に 1 つ」の `ENOSPC` を、本当の資源の検査に置き換える）。実機の受け入れに「3 つ目の出力（DP-alt があれば）で ENOSPC、他の出力は続く」を可能なら足す。

## 設計と実装（2026-10-07、q856-i01、P2）

- 設計: [design.md](design.md)（design-reviewer の敵対的 review の F1〜F19 を §9 に反映）。ユーザーの DBUF の決定「2 つ目の画面を足す時に、1 つ目の画面を点け直す」と P1 の口の要件 1〜6（§2）を入れた。依存に ws113-p004b（compositor の head、main に入った）を足す: 受け入れの実機の確かめは p004b の compositor で行う（native の probe は作らない）。
- 実装（UAPI・HAL・libvulkan・compositor は不変、worker.c・worker.h・platform/amd64/vmunix.mk は Q1 の許可で範囲に足した）:

| 部分 | file |
| --- | --- |
| 規則（純粋な関数、host 試験） | `display/head-rules.c`・`head-rules.h`: claim が move・head・limit のどれか（resident の lease、head の有無、broken、latch、resident が panel、head が HDMI か DP） |
| head の本体 | `display/head.c`・`head.h`: claim（output の準備、pipe の衝突、lease は resident と同じ番号の列）、release（常に 0）、lease の close、query の状態、DBUF の予約の pipe、latch（connector と generation）、点け直しの判断、frame（点灯、CPU か GPU の copy、screen 1 の flip）、停止（keep で dormant）、resume |
| 状態 | `display/internal.h`: `struct i915_display_head`、`window.run_pipes`・`relight`・`keep_buffers`、`struct i915_lcd_kernel` の `display`、`i915_lcd_run_params` を modeset.h から移した、status の `dbuf_active_pipes_now` |
| resident の run | `display/modeset.c`: run の `also_active_pipes` に head の pipe、buffer の保持（点け直し）、B→A の copy、kind の params・cfg・preflight・DP の fallback を output を引数に（`drv_i915_lcd_output_params`・`_cfg`・`_link_fallback`）、`flip_wait_screen`、commit_disable の keep_pipes |
| DBUF | `display/watermark.c`: `drv_i915_lcd_ms_wm_compute_off` の old を device の今の状態に、active_pipes を keep_pipes との積に（review F2） |
| present・worker | `display/present.c`: present・wait・release・close の lease の振り分け、head の present（resident の lease が要る、EIO→ENXIO）、latch を screen 0 に、window の serve の初めの resume と終わりの停止、点け直しと 2 pipe の失敗の retry（spared）。`worker.c`・`worker.h`: item の `head`、window の中の head の frame の点け直しの判断、head の release は hold しない |
| claim・query・power | `display/display.c`: 他の connector の claim を head か move に、query の ACTIVE・LIMITED を head の状態から、head の power は EOPNOTSUPP、`output_back` は head が claim されている間は戻さない。`diagnostics.c`: abandoned に head の broken |

## 確認（2026-10-07）

- build（warning 0）: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/ws113-p011 vmunix`（kernel include check・amd64 vmunix check PASS）、`I915_TEST_CAPTURE=y`（`BUILD=build/ws113-p011-capture`）PASS。
- host: `sh plan/ws113/tests/host-head-rules.sh`（16 checks、`-std=c89 -pedantic`）。DBUF の遷移と点け直しの状態機械の host 試験は無い（display の host 試験の runner が今の tree に無い）。
- 未実施: QEMU（Venus はこの driver を通らない、対象外）、実機 5330（eDP + HDMI、p004b の compositor）。実機の手順（T1 の passthrough か UAT）: (1) 拡張で HDMI を挿す → anchor（eDP）が 1 秒前後消えて戻り（`resident display: lit again for two pipes`）、HDMI に wallpaper（`display head: lit`、`DBUF lit: pipes 0x3 ...`）。(2) Settings で mirror（p006 の後）、または p004b の mirror の設定で HDMI に desktop の複製。(3) HDMI を抜く → eDP は続き、head は release（`display head: stopped ... dark`、`DBUF stopped: pipes 0x3`（予約のまま、slices・joined は (1) と同じ））。挿し直す → 再び点く（同じ window、点け直し無し）。(4) logout・login（greeter と session の head の閉じと開き）で eDP が点け直されない・HDMI が戻る。(5) sleep・resume → HDMI が最後の絵で戻る（`stopped with the window` → resume）。(6) HDMI 無しの起動・1 出力の時の DBUF が今と同じ（`LCD-B input: ... DBUF slices now`）。各段で `resident display: ended PASS`・underrun 無し。
- 既知の制限: head は 1 つ（pipe B）、resident が panel の時だけ（HDMI・DP の resident の横の panel は ENOSPC）。head の power・refresh の境界は無い。点け直しの間 anchor は 1 秒前後消える（review F15）。F12・F13・F18 は残り（design §9）。
