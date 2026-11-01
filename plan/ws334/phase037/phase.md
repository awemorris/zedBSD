<!-- awesome-plan project=zedbsd record=ws334-p037 -->

# ws334-p037 本物の Windows が要る口: TSF・DirectManipulation・modal の間の描き直し・題の帯の透過

- 親: [WS334](../ws.md)。p033 から分けた（2026-11-02 02:44 Q1）。
- 状態: planned（担当は未割当。本物の Windows の host が要る）。
- 依存: p033。

## 範囲

- a: TSF（Text Services Framework）の text store。今の IMM32 の入力を壊さないよう、TSF が無い・失敗した時は IMM32 に戻る。
- c: DirectManipulation による touchpad の慣性の scroll と AXIS_STOP。今は wheel の message で scroll する。
- d: modal な resize・move の loop の間の描き直し。公開の口 `kl_app_set_modal_draw`（libkeiland の header に足す。WM_TIMER から呼ぶ）を足す（Q1 の判断、下）。
- d: 題の帯の透過（Mica の上の題の帯の DIB と Vulkan の swapchain の alpha）。
- 本物の Windows 10・11（amd64 と arm64）で p032 の M1〜M8 と p033 の未試験の項（暗い mode と accent の追従、Mica、touch と pen、頁の画像の Explorer・Word・Paint への drag、DPAPI が他の利用者・他の機械で読めない事）を確かめる。

## Q1 の判断（2026-11-02 02:44）

- p033 は実装した所で cleared にし、Wine で確かめられない口をこの Phase に分けた。Wine には TSF・DirectManipulation・DWM が無く、未試験の text store を入れると今動いている IMM32 の入力を壊す恐れがあるため。
- drop の受け（b の残り）はこの WS に入れない。browser の shell が drop を受けないので今は益が無く、libkeiland の全体の移植（WS234）の側の物。
- modal の間の描き直しの口 `kl_app_set_modal_draw` は足す。libkeiland の Windows の backend の中だけでは、modal な loop の間に app の描画を呼べないため（HAL の変更ではない）。
