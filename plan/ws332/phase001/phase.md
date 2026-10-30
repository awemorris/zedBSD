<!-- awesome-plan project=zedbsd record=ws332-p001 -->

# ws332-p001: 設計（時間の単位の API と tick の率を取る口）

Status: cleared（2026-10-30 Q1）
Parent: [WS332](../ws.md)
Queue: q1722（P3）

## 範囲

- 今の KERN_CLOCK_HZ の利用を分類する（期限・待ち時間・周期・計測・UAPI への値）。それぞれの置き換え先の API（既存の kern_* の時間の関数があればそれ、無ければ足す案）。
- 2026-10-30 Q1 経由のユーザーの指示で範囲を絞った: 可変の tick は「今の率を走っている時に取る kernel の口」だけを用意し（値は今は固定、呼び手は定数を仮定しない）、
  周期を実際に変える実装・mode・電源管理との関係は将来の拡張として design.md に短く書く。HAL の timer は変えない。
- 成果: design.md と Phase の分け方の案。design-reviewer を 1 回。

## 結果（2026-10-30 P3）

- 設計: [design.md](../design.md)。§2 分類（KERN_CLOCK_HZ 170 行・65 file、KERN_MS_TO_TICKS 59 行、kernel の HAL_TIMER_FREQUENCY 2 行）と置き換えの規則、
  §3 時間の単位の API（`kern_ms/us/ns_to_ticks`・`kern_ticks_to_ms/us/ns`・`kern_ticks_to_rate`・`kern_ticks_to_timespec`、clock.c の out-of-line）、
  §4 今の率を取る口（`kern_clock_tick_rate()`・`kern_clock_resolution_ns()`）、§5 将来の拡張、§6 review の扱い、§7 Phase の案（p002 と規約の見直し）。
- design-reviewer: 1 回（範囲を絞る前の広い版に対して）。指摘は誤り 9・欠落 9・矛盾 4・改善 4。p002 に効く物の扱いは design.md §6 の表、
  可変の tick の実装に効く物は §5 に要点を残した。絞った後の版の再レビューはしていない。
- hal.h の差分の案: 無し（範囲を絞ったので周期を変える口は出さない）。hal.h の RTC の comment の語だけは、ユーザーの承認で p002 と別の commit で直した（phase002）。
