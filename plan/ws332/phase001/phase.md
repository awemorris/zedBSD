<!-- awesome-plan project=zedbsd record=ws332-p001 -->

# ws332-p001: 設計（時間の API と可変の tick）

Status: in-progress
Parent: [WS332](../ws.md)
Queue: q1722（P3）

## 範囲

2026-10-30 ユーザーから、可変の tick は取得の口だけで実際に変える実装は今回は不要、という趣旨の指示を受けた。周期を変える口・mode・電源管理は design.md に将来の拡張として短く書くだけ。


- 今の KERN_CLOCK_HZ の 175 か所を分類する（期限・待ち時間・周期・計測・UAPI への値）。それぞれの置き換え先の API（既存の kern_* の時間の関数があればそれ、無ければ足す案）。
- 可変の tick: 周期を変える口、誰が変えるか（mode、電源管理）、変わる時の timer・期限の正しさ。各 arch の HAL の timer で周期を変えられるか（hal.h の口で足りるか。足りないと主張する時は理由を必ず書く）。
- 成果: design.md と p002 以降の Phase の分け方の案。design-reviewer を 1 回。
