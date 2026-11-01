<!-- awesome-plan project=zedbsd record=ws287 -->

# WS287 EHCI の full-speed・low-speed の device（split transaction）

- Master: [plan/master.md](../master.md)（2026-10-23 作成、着手は後日）。Primary Milestone: MG008。Related: WS286（ZYBO の ChipIdea の USB）。
- Status: planning・保留（後日）（2026-10-27 P3 の精査 q1608、ユーザー「…masterとWSを修正して、completedやcanceledにしてください。」: 2026-10-23 ユーザー「USBは後回しにして、full speed, low speedはWSを作ってあとで対処し、今はFPGAブロックを動作させるということをゴールにして最短で進みましょう。」）。旧: planning
- 出典: 2026-10-23 ユーザー「USBは後回しにして、full speed, low speedはWSを作ってあとで対処し、今はFPGAブロックを動作させるということをゴールにして最短で進みましょう。」

## 背景と目的（案）

- 今の EHCI の driver（src/drivers/pci/pci-ehci.c）は high-speed の device だけを扱い、transaction translator を通す split transaction が無い。ZYBO（ChipIdea、EHCI 互換、TT を内蔵）などで keyboard・mouse の LS/FS の device が動かない（ws286 p001 の調べ）。
- 完了: EHCI の split transaction（interrupt・control・bulk）で LS/FS の HID が動く。ZYBO の ChipIdea の host の driver（WS286 から外した物）もここか別の WS で扱う。

## Phase

| Phase | 内容 | 依存 | 状態 |
| --- | --- | --- | --- |
| p001 | 調べと設計 | — | planning（着手は後日） |

## 現在の状態・再開点

- 2026-11-01 09:25: VF2 でも同じ制限が見えた（[BUG-388](../bugs/BUG-388.md): EHCI 側の port に挿した Logitech の Bolt の受信機（full-speed）が使えない、ユーザーが原因を特定）。BUG-388 はこの WS へ移管して閉じた。対象の機械に VF2 を足す。

- 2026-10-23: 作成だけ。
