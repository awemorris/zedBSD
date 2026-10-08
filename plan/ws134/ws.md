<!-- awesome-plan project=zedbsd record=ws134 -->

# WS134: システムモニターのアプリ（Analytic Spatial UI）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p007（T1-066 PASS）・p013（T1-070 PASS）は Q1 の判定、p003 は uncleared（fps・GPU の名前の 2 点）、p010 は規約（ベータ3）と実機の i915 の値）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q649（P2、p001）〜 q660（P2 generation8、p004、2026-10-04 実装済み・QEMU の試験待ち）
Resume point: (1) p003 の uncleared の直し（fps・GPU の名前 2 点、[phase003](phase003/phase.md) の「再開の条件」）と T1 の再試験。
(2) p004 は実装済み（a37e407、host 試験 PASS）、T1/T2 の `monitor-p004.sh` の結果待ち。p001・p002 は cleared。
<!-- awesome-plan-current:end -->

## 目標（2026-10-03 ユーザー）

「P2が空いているので、システムモニターのアプリを作ってほしいです。添付がイメージです。」2026-10-03 user「イメージの通りじゃなくてよくて、要素を採用してほしいです。」→ 参考の画像の再現ではなく、コンセプトと画像の要素を取り入れて設計する。デザインのコンセプトは [design/user-concept.md](design/user-concept.md)（原文の要約）と参考の画像 [design/reference-image.webp](design/reference-image.webp)。

CPU（全体と各 core）、GPU（Util・VRAM・温度・電力、複数）、RAM（Used・Cache・Available・Swap）、Network（RX/TX）、Disk I/O（読み・書き・Latency）と最近の出来事を、中央の「システム状態の立体コア」を顔にした層構造の画面で見せる Keiland の app。3D は階層を見せるため、動きは「呼吸」。タブレットの操作（タップで浮く、スワイプで時間軸、長押しで固定、2 本指で俯瞰）。

## Phase

| Phase | 内容 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: zedBSD（と Linux・FreeBSD）で取れる情報の出どころの調査（不足は kernel・libkeiland の追加の案）、画面の構成・3D の表現・動き・操作・描画の方式（libkeiui と Vulkan）、Phase の分け方 | planning（P2、q649） | — |
| [p002](phase002/phase.md) | M1 骨組み: package・3 OS の build・libkeiui の窓＋自前の Vulkan・title bar・data source の層（sim・replay）・履歴・plate と文字・ZMON の log・App Home | cleared（T1-041 PASS） | p001 |
| [p003](phase003/phase.md) | M2 3D と動き: 状態コア・CPU のタイル面・GPU のカード・Network/Disk の流れ・Memory の層・視差・数値の slide・段階的な警告・Events | cleared（実機の fps 21、UAT 2026-10-04） | p002 |
| [p004](phase004/phase.md) | M3a 操作: tap の展開・長押しの固定・swipe の時間軸・pinch の俯瞰・key/pointer・--calm | cleared（q660、T1-063 PASS） | p003 |
| [p005](phase005/phase.md) | K1 kernel: CPU ごとの時間 `hw.cputimes`（sysctl の CLI・top の CPU 行、design.md §1.2 の review の反映） | cleared（q661、T1-064 PASS） | — |
| [p006](phase006/phase.md) | K2 kernel: disk ごとの統計 `hw.diskstats`（物理の whole disk） | cleared（q661、T1-065 PASS） | — |
| [p007](phase007/phase.md) | K3 kernel: GPU の telemetry の sysctl `hw.gputelemetry`（Guardrail により ioctl にしない。実機の確認は 5330） | in-progress（q661: 実装・build 済み、QEMU の試験待ち。i915 は実機） | — |
| [p008](phase008/phase.md) | M3a 本物の値（zedBSD の backend の monitor の領域） | cleared（q662、T1-067 PASS） | WS131 p010 までの統合、p005・p006 |
| [p011](phase011/phase.md) | M3b Linux・FreeBSD の backend の monitor の領域 | cleared（q662、T1-068 PASS） | p008 |
| [p012](phase012/phase.md) | M3c compositor の `kl_system_monitor_v1`（manager v2、専用の thread、ack と間引き）と libkeiland の `kl_system_monitor_*` | cleared（q662、T1-069 PASS） | p008 |
| [p013](phase013/phase.md) | M3d app の system の source | in-progress（q662: 実装・host 試験済み、QEMU の試験待ち） | p012 |
| p009 | K4 kernel: ACPI の thermal・電池（実機、WS131 p005 と調整） | planned（**[BUG-165](../bugs/BUG-165.md) の後**: 5330 の DSDT が読めず、電池・AC の情報が無い。UAT 2026-10-04） | 実機、BUG-165 |
| [p010](phase010/phase.md) | M4 全文規約・回帰・デモの通し | in-progress（q663: 全文規約の見直しと直し済み、QEMU の回帰を依頼。実機の p003・p007・p009 とデモは実機待ち） | p002〜p009 |

2026-10-03 user「P2はコードを書いてOKだと思います。衝突しないです。」 → P2 は設計（p001）を書いたら、user のレビューを待たずに実装の Phase へ進んでよい（Phase の ID は Q1 が割り当て）。

2026-10-03 user「P2はとりあえずOSから取れない情報はスタブデータでそれっぽいアニメーションを表示しましょう。」→ OS から今取れない値（GPU の温度・電力など）は、stub のデータ（もっともらしく動く値）で画面と動きを先に作る。stub であることは code と画面の上で分かるように（後で本物に差し替える所を一覧に）。
2026-10-03 user「simという表記はつけなくていいです。私がどの項目がスタブか把握できていればいいです。」→ 画面に stub の印は付けない。stub の項目の一覧を design.md（と完了の報告）に書き、user が把握できるようにする。

2026-10-03 user「システムモニターは私に確認しなくていいので、どんどん実装して動かしてください。」 → P2 は WS134 の Phase を user の確認なしに順に進める（Q1 は報告だけ受ける）。
