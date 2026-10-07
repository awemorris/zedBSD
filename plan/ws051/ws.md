<!-- awesome-plan project=zedbsd record=ws051 -->

# WS051: USB-C の DisplayPort Alternate Mode

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-10-07 P1: p005a を実装（HPD の長い pulse → detect → 事象、IRQ_HPD の sink の確かめ、TC の link reset の 2 秒の猶予）、実機は T1 への依頼。p004b は設計済み（code は ws113-p011a の後）。p004a は merge 済み（c7ead0ec2）、実機は Q1 とユーザー。それ以前: p004a を実装（dp-ext.c・dp-ext-kern.c、host-dpext 73/0、build warning 0）、実機は T1 への依頼（Q1 経由）。次は p004b。それ以前: 2026-10-07 Q1: p002b cleared（T1-350）。次は p003（P1、q847、正解値は q848 で 5330 から採取）。それ以前: P2: p002b を実装（host 101/0、build warning 0、ktest・実機は 5330 の後）。次は p003（正解値の後）。それ以前: p001 の design.md 第 4 版（§14、[レビュー](design-review-2026-10-07.md) を反映）。p002 を実装（host PASS、ktest は 5330 の後）。次は p002b（TC の核）。§14.5 の正解値の採取の手順はユーザーの判断待ち（Q1 経由）。それ以前: 2026-10-04 の第 1〜3 版、§10・§13 の決定
<!-- awesome-plan-current:end -->

## 目標

USB-C の port につないだ DisplayPort の display（USB-C の monitor、USB-C から DP・HDMI への変換）を i915 が DisplayPort Alternate Mode で駆動でき、
抜き差しを Vulkan の Display の拡張の経路で Keiland に知らせ、Keiland の指示で画面を出す（2026-10-04 ユーザーの決定。mirror・拡張・出力 off は
Keiland が決める）。i915 は GOP の出力先以外に自分の判断で scanout を始めない（Guardrail「GPU の driver の scanout の規則」）。

## きっかけ

2026-09-24 ユーザー指示: 「USB-C DisplayPort Alternative Modeの実装。」

## 前提と今あるもの

- DP の mode に入るのは PD controller・EC の firmware と IOM（PMC の mux）で、i915 は TCSS・FIA の register で状態（live status、PHY の ready、
  lane と pin の割り当て）を読み、HPD は i915 の Type-C の hotplug の割り込みで受ける（Linux の i915 の `intel_tc.c` と同じ分担）。
  **UCSI（WS050）は必須の依存でない**（2026-10-04 ユーザーの決定 5: WS051 は UCSI を待たずに進める）。UCSI 2.0 以上で HPD・pin が取れる時は
  WS050 がそれも取り、i915 の値と統合する（決定 5 の補足）。firmware が自分で DP mode に入らない時は WS050 の SET_NEW_CAM を使う。
  plug の向きは取れれば表示し、受け入れの条件にしない（2026-10-05 の §13 (2) の決定、design §14.1）。
- 画面を出すのは GPU の display engine: 対象機（Alder Lake-P）では i915 の Type-C の subsystem（TCSS: FIA の lane の割り当て、Type-C PHY、
  DP の link training、HPD の割り込み）。i915 の driver は WS029・WS031 にある（render と Vulkan）。**display の modeset（pipe・transcoder・DDI）の
  範囲と、内蔵 panel 以外の出力がどこまであるかは p001 で調べる。**
- zdesktop（WS035）の複数 display の扱いは、出力が出た後の話。

## 範囲

- TCSS・FIA で DP-alt の状態を読み（live status、PHY の ownership、TC cold、lane と pin の割り当て）、HPD を受け取る。読んだ HPD・pin・向きを
  WS050 の Type-C の層に報告する。
- i915 の TCSS: lane を DP に割り当て、Type-C PHY を DP で使い、DDI・transcoder・pipe を立てて link training、EDID の読み出し。
- GOP の出力先の引き継ぎと、今の外部 display の優先の挙動の廃止（Guardrail の規則、決定 2。WS113 p002 part A が実装、WS051 は USB-C の分、design §14.2）。GOP の出力先なら eDP・HDMI・DP・
  USB-C のどれでも初期化を試み、非対応なら firmware の画面を保つ。
- 抜き差しと HPD の IRQ の扱い。抜き差しは display の UAPI の事象（`GPU_DISPLAY_EVENT_CHANGE`）から libvulkan の Display の拡張の通知へ
  （WS113 と同じ経路）。TC の output を display の UAPI の列挙に出し、Keiland の claim・present の時だけ出力する。

## 受け入れ

- 対象機の USB-C port につないだ DP の monitor に、Keiland の指示（display の UAPI の claim・present）で画面が出る。抜き差しが Vulkan の Display の
  拡張の通知に届き、差し直すと Keiland の指示で戻る。起動時に USB-C の display があっても GOP の出力先がそのまま出る（driver が切り替えない）。
- 規約の全文、build、boot test。実機の証拠が中心（QEMU には無い）。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| [ws051-p001](phase001/phase.md) | 調査と設計: i915 の TCSS・Type-C PHY（DKL）・TC PLL・DDI の手順、今の i915 の display の範囲、画面の構成、WS050 との連携 | in-progress（2026-10-07 P2: [design.md](design.md) 第 4 版（§14: §13 の決定、WS113 p002 との分担、WS050 の口、WS084）、design-reviewer に掛ける） | — | 設計文書 |
| [ws051-p002](phase002/phase.md) | TC PLL の enable の番地（H8、sanitize の対象から TC を外す）、VBT の DVO の code の値（L1）、clone の log、新しい host の試験、ktest の期待値（GOP の引き継ぎと外部優先の廃止は WS113 p002 part A、design §14.2） | in-progress（2026-10-07 P2: 実装、host PASS、build warning 0。ktest は 5330 の後の T1、ws113-p002 の clearance 待ち） | p001、ws113-p002 の clearance | `src/drivers/gpu/i915/display/`（takeover.c・diagnostics.c）、i915 の ktest |
| [ws051-p002b](phase002b/phase.md) | TC の port の核（`tc.c`）、TC の AUX の power domain（H1）、AUX_USBC の well の TC の分岐（H2）、DE の HPD の配送（H5）、診断（向きは記録だけ） | cleared（2026-10-07 T1-350 5330 ktest PASS） | p002 | `src/drivers/gpu/i915/display/` |
| [ws051-p003](phase003/phase.md) | DKL PHY と TC PLL（ADL-P の enable の番地の分岐）、DDI の TC の clock、ADL-P の DKL の buffer translation、DP_MODE、FIA の lane 数、TC PLL・init_mode・sanitize の readout（判定は変えない） | cleared（2026-10-07 T1-354） | p002b、正解値（design §14.5、ユーザーの判断） | 同上 |
| [ws051-p004a](phase004a/phase.md) | TC の AUX・DPCD・EDID の診断、外部 DP の object（M6）、調べた後の同期の disconnect（M2）、branch device と sink count（H7） | in-progress（2026-10-07 P1: 実装、host PASS、build warning 0。実機は T1 への依頼） | p003 | 同上 |
| [ws051-p004b](phase004b/phase.md) | display の UAPI での出力（claim・mode・present での link training（fallback、M3）・modeset・scanout、TC PLL・TBT PLL を pool へ、TC の encoder を N1 の registry へ） | planning（2026-10-07 P1: 設計だけ。code は ws113-p011a の merge の後、Q1） | p004a、ws113-p011a（付け替えの口、R1〜R4） | 同上 |
| [ws051-p004c](phase004c/phase.md) | GOP が USB-C の時の引き継ぎ（M5: 判定を引き継ぐに、host-gop.c の期待値） | in-progress（2026-10-08 P1 q877: 実装・host・build、実機は BUG-256 の後） | p004b | 同上 |
| [ws051-p005a](phase005a/phase.md) | 抜き差しの検出と事象: HPD の長い pulse → detect（外部 DP の probe）→ `GPU_DISPLAY_EVENT_CHANGE`、2 秒の猶予と 5 回の retry（M4 の前半） | in-progress（2026-10-07 P1: 実装、host PASS、build warning 0。実機は T1 への依頼） | p004a（2026-10-07 Q1 の判断: p004b の依存を外し p005 を a・b に分けた） | hotplug.c・dp-ext |
| [ws051-p005b](phase005b/phase.md) | scanout 中の抜けの停止、IRQ_HPD の retrain（M4 の後半）、S0ix の口（M10） | in-progress（2026-10-08 P1 q877: retrain を実装・host・build。抜けの停止は案（判断待ち）、S0ix はベータ3） | p004b、p005a | 同上 |
| ws051-p006 | 規約の全文の確認と最終の確認 | planned | p002、p002b、p003、p004a、p004b、p004c、p005a、p005b | WS の全 source |

## 2026-10-04 予定（Q1）

ユーザー「次の新規実装項目は、USB-C の DisplayPort Alternate Modeの実現を目標にします。その次が電源管理です。これらは併走できると思います。共通のpredecessorがAMLですね。」→ [queue.md](../queue.md) の q679（WS050 p001）・q680（WS051 p001）・q681（WS052 p001）。設計は WS049 の q677（BUG-165、DSDT）と並走、実装は q677・q678（ws049-p007）の後。

## 2026-10-04 ユーザーの決定（WS050 §10、Q1 経由）と WS051 への影響

決定 5（HPD・pin は i915 の TCSS・FIA から、UCSI 2.0 以上で取れる時は UCSI からも）により、WS051 は UCSI を待たずに進める。決定 4（向きは 1.x でも
i915 から）は 2026-10-05 の §13 (2) で受け入れから外した（取れれば表示、design §14.1）。i915 の値は WS050 の Type-C の層へ報告する（WS050 p005）。

## 2026-10-04 WS051 §10 のユーザーの決定（Q1 経由、design.md の末尾に記録）

TBT-alt は範囲外。driver は GOP の出力先を引き継ぎ自分で出力先を変えない（今の外部 display の優先はやめる。WS113 p002 part A で直した）、USB-C の DP の接続は
Keiland が Vulkan の Display の拡張で通知を受け、mirror・拡張・出力 off を決める。Guardrail に「GPU の driver の scanout の規則」（GOP の出力先
以外に scanout を始めない、GOP の出力先ならどのインタフェースでも初期化を試みる）。
