<!-- awesome-plan project=zedbsd record=ws333-p004 -->

# ws333-p004: `/dev/thermal` の UAPI の全文の案（ユーザーの review）

Status: in-progress（2026-11-12 P1、q2012-i01: 案を書いた。ユーザーの review 待ち）
Parent: [WS333](../ws.md)
Queue: q2012-i01（P1）

## 目的

design.md §4.10・§9 の p004: `include/uapi/thermal.h` の全文（ioctl の番号と struct、事象の record、`/dev/thermal-inject`）と、`/sbin/thermal` の使い方の案を書き、design-reviewer を通してユーザーの review に出す。実装（header を `include/uapi/` に置く・device・道具）はこの Phase ではしない（p005 後半・p013）。

## 依存

- p001（設計）。p003・p005 前半・p006・p011・p016 の kernel の口（`include/kern/thermal.h`・`thermal_private.h`・`cpuperf.h`）が main にあり、UAPI の struct はその写しの形に合わせる。
- 2026-11-01 のユーザーの決定（Q333-1〜7 を推しで、Q333-4 は 100 = その物が多い）。

## 受け入れ

- 案の全文（header の案と使い方の文書）が `plan/ws333/` にある。
- design-reviewer 1 回、指摘を反映（反映の表）。
- -m64・-m32 の layout の確かめ（host の cc で `_Static_assert` の付いた案の header を compile する）。
- ユーザーの答え（review）。答えで変わる所を直した版。

## 成果

- 案の header: [../proposed/include/uapi/thermal.h](../proposed/include/uapi/thermal.h)
- 案の説明と `/sbin/thermal` の使い方: [uapi-proposal.md](uapi-proposal.md)
- review: [uapi-review.md](uapi-review.md)
