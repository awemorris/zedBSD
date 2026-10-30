<!-- awesome-plan project=zedbsd record=ws333 -->

# WS333: /dev/thermal で統一する電力・熱の枠組み（CPU の idle・DVFS・P/E core、熱・fan）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG004。
- Status: planning
- 目標: CPU の idle（C-state、MWAIT）・P-state（HWP と、HWP の無い CPU の driver の P-state）・P/E core の相対性能・熱の監視・fan・非常の停止を、1 つの device `/dev/thermal`（と `/sbin/thermal`）の下で統一して設計し、実装する。
- Resume point: p001（統一の設計）から。担当は未定。

## ユーザーの決定（要点）

- 2026-10-31 ユーザーから、WS301（CPU の DVFS と P/E core）と WS303（熱と fan）を廃止して統合し、新しい WS にする、という趣旨の指示を受けた。理由は、2 つの検討を始めた時には /dev/thermal という統一の device を想定しておらず、検討が進んでこの形になったので、統一して設計しないといけない、という事。
- 前の 2 つの WS のユーザーの決定（UAPI は /dev/thermal、HAL は変えない、HWP の無い CPU も P-state を driver で、strategy は performance・balanced・powersave・manual、fan は上げるだけ、非常の停止）は、この WS の設計の入力として引き継ぐ（[ws301/ws.md](../ws301/ws.md)・[ws303/ws.md](../ws303/ws.md) とその design.md・user-questions.md）。
- 関係: 2026-10-31 WS291 p022（i915、5330）で、zedBSD の idle の package の電力が 12.99 W（Linux 3.2〜3.5 W）で、CPU の idle が HLT（C1）だけ・P-state の制御が無い見込みのため、GPU が電力の上限で 800 MHz に抑えられていると分かった（WS299 の結果の「amd64 の idle が C1 だけ・HWP」と同じ件）。CPU の idle（MWAIT の C-state）はこの WS の範囲に入れる。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | 統一の設計: /dev/thermal の UAPI（CPU の idle・P-state・P/E core・熱の zone・fan・strategy・非常の停止）、kernel の枠組みと driver の分け方、HAL の口で足りるか（足りないと主張する時は理由）、WS301・WS303 の設計からの引き継ぎと食い違いの整理、Phase の分け方。design-reviewer を通す | planned | — |
