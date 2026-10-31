<!-- awesome-plan project=zedbsd record=ws302 -->

# WS302 デスクトップスレッドバランサ（DTB）

- Master: [plan/master.md](../master.md)（2026-10-27 作成、q1615 P1 の計画）。Primary Milestone: MG006（グラフィカルな操作環境の応答）。Related: MG004、[WS333](../ws333/ws.md)（core の種類・相対の性能・topology の口、WS301 を統合。2026-10-31 に口は main に入った）。旧 [WS301](../ws301/ws.md) は WS333 に統合された。
- Status: planning（2026-10-27 新設。2026-10-31 P5 q1775-i01: p001 の設計 [design.md](design.md) 第 1 版と問い [user-questions.md](user-questions.md)、ユーザーの答え待ち）
- 出典: 2026-10-27 ユーザー
  「デスクトップスレッドバランサの追加（WSを立てて検討）
  - コンフィグで有効にできるデスクトップスレッドバランサ(DTB)をカーネルに追加
  - DTBはスレッドスケジューラ(sched)ではない
  - プロセスやスレッドを、適切なコアに移動するように、schedに依頼を行う
  - DTBはプロセス/スレッドの動作傾向や、人間の操作傾向から、ヒューリスティックなバランシング、スケジューリングのヒントを行う
  - HIDサブシステムの統計、GPUサブシステムの統計、プロセス/スレッドの統計を使う
  - GPU使用アプリは応答の優先度を上げる方向で速いCPUに割り付け最適化する
  - 一定時間以上、syscallなしでビジーなスレッドは、遅くて消費電力の低いCPUに割り付け直す
  - ターミナルアプリ(GPU使用なしのttyあり)は、開発者向けとして、UIのレスポンスを落さない範囲で速いコアに割り付ける
  - nice値からも割り付ける
  - まずはどういった情報からどういったバランシングができるかを検討する」
- 全体の依存と着手の順: [plan/agents/P1/pm-plan.md](../agents/P1/pm-plan.md)。

- 2026-10-27 09:39 ユーザー（クリック）: CONFIG_DTB の既定は off、nice は core の選択に加えて sched の priority にも効かせる。

## 単一の到達目標

config（`CONFIG_DTB`）で有効にできる DTB を kernel に入れ、5330（P-core 2・E-core 8）で、HID・GPU・process/thread の統計と nice から thread の性質を見分けて
sched に core の種類の希望（hint）を出し、DTB を切った時に比べて desktop の応答（入力から画面まで、GPU を使う app の frame の時間）を落とさず、
長く忙しい計算の thread を E-core に寄せて電力を下げる事を数で示す。DTB は sched ではなく、移動と実行の順は sched が決める。

## 今の事実（2026-10-27 の調べ、根拠は file:行）

（2026-10-31 注: 下の行番号は 2026-10-27 の tree の物。main 3d820d9ca での引き直しは [design.md](design.md) §2・§2.8。）

- sched（`src/kern/sched.c` 2952 行、`include/kern/sched.h`）: CPU ごとの run queue、thread は CPU に固定、移動は `sched_set_cpu()`（:1665-1758）だけで、**走っている thread は移せず（EINVAL）、sched.c の外に呼び手は無い**。
  16 段の priority、全部の thread が既定の priority で作られる。affinity・cpuset は無い。負荷の分散は idle の CPU の盗み（:2528-2599）と、忙しい CPU が idle の CPU を起こす（:1092-1113）だけで、
  core の性能・topology を知らない。hint の口は無い。
- **nice は保存されるが sched は読まない**（`setpriority` は `process->nice_value` に入れるだけ、`src/kern/syscall.c:8863-8935`（main 61630bb0b）、sched.c に nice は無い）。
- 統計: process ごとの tick（cpu・user・system、`include/kern/process.h:119-124`）、`controlling_tty`（:145）、`command`。thread ごとは無い（`kern.perf=1` の時の診断だけ）。
  syscall の数・最後の syscall の時刻・自発／非自発の切り替え・wake の数は無い（`ru_nvcsw`・`ru_nivcsw` は宣言だけ）。足す所は `syscall_dispatch_body`（syscall.c:9796、main 61630bb0b）、`sched_wakeup`・`sched_yield`・`switch_without_enqueue`。
- HID: `src/drivers/generic/input.c` に kernel の購読の口 `drv_input_subscribe`（:1517、最大 8、今は console だけ）。数・最後の入力の時刻は無い。
- GPU: `/dev/gpuN` の `struct gpu_session`（`src/drivers/gpu/gpu.c:219`）に process ごとの使用量は無い。device ごとの `hw.gputelemetry`（busy_ns など）だけ。
- core の性能・topology の口は無い → [WS333](../ws333/ws.md)（WS301 を統合した WS）が作る。（2026-10-31 追記: 口は main に入った。design.md §2.5）

## 範囲

- 含む: 「どの情報からどんな balancing ができるか」の検討（p001、ユーザーの「まず」）、nice を sched の priority に効かせる事（2026-10-27 09:39 ユーザーの決定、上の原文）、sched への依頼の口（hint、走っている thread の安全な点での移動）、
  統計の追加（thread・HID・GPU）、DTB の本体（周期の評価・分類・hint・履歴の幅）、config、観察の道具、5330 の評価。
- 含まない: sched の作り直し（hint を受ける最小の変更だけ）、DVFS の policy（WS301、今は WS333）、他の機種の評価（hybrid でない機械では「速い core」が無い。電力の寄せ方だけ後で）。

## 検討の骨組み（p001 で埋める表の案）

| 情報（源） | 分かる事 | hint（例） | 衝突と注意 |
| --- | --- | --- | --- |
| GPU の session と submit・busy（GPU、process ごと、新） | GPU を使う app（desktop の client・game・video） | 速い core、応答の priority を上げる | 背景の compute だけの GPU の利用と区別 |
| syscall の無い連続の実行の時間（thread、新） | 長く忙しい計算 | 遅い省電力の core へ移す | build の compiler も当たる。tty の規則と衝突 |
| controlling tty があり GPU を使わない（process、既存＋GPU） | terminal の app（開発者の道具） | 「UI の応答を落とさない範囲で」速い core | UI（compositor・GPU app）が先。P-core が 2 つしかない |
| nice（process、既存） | 利用者の意図（背景か） | nice > 0 は遅い core、nice < 0 は速い core | 他の規則より強いか弱いか |
| HID の入力の頻度・最後の時刻（HID、新） | 人が操作中か | 操作中は応答側に寄せ、無操作の時は省電力に寄せる | 入力の宛先の app は kernel に分からない（D4） |
| wake の頻度・待ちの長さ（thread、新） | 対話的か（短く走って眠る） | 速い core で待ちを短く | |
| compositor（Keiland）からの focus の hint（UAPI、新、任意） | 前面の app | 前面を速い core | UAPI の決定、backend だけが送る（Guardrail） |

評価の物差し（案）: 入力から frame までの遅れ（Keiland の計測）、GPU app の frame の時間の分布、build の時間、idle と負荷の時の電力（RAPL）、migration の数。
比較は DTB の on・off、同じ 5330、同じ作業の script。

## 制約

- DTB は sched の外の kernel の部品（`src/kern/dtb.c` など、`CONFIG_DTB`、既定は off、2026-10-27 09:39 ユーザーの決定）。sched の変更は hint を受ける最小の口だけ。
- HAL は使わない見込み（topology・性能は WS333 の kernel の API）。UAPI（focus の hint、観察の口）は明示の決定（`plan/master-design-policy.md:189`）。desktop の OS 固有の code は libkeiland-backend-zedbsd だけ（[Guardrail](../guardrail.md)）。
- 規約: 新しいコードに `plan/coding-style.md` の全文。tick は `kern_ms_to_ticks()` など（数の直書きをしない）。
- 試験: host 試験（分類と hint の決定の表、履歴の幅）、5330 の実機（1 本の script で on・off を比べる）。QEMU は回帰（DTB を on にして起動・desktop が動く、T）。

## 受け入れ（案、数は p001 で決める）

1. p001 の検討の文書（情報 → balancing の表、衝突の規則、物差し）がユーザーの review を通る。
2. 5330 で DTB on の時、GPU を使う app の frame の時間と入力の遅れが off の時より悪くならず（案: 中央値と 95 パーセンタイルが同等以上）、長い計算の thread が E-core に移り、電力が下がる（案: 背景の計算が走る時の package の電力）。
3. terminal の作業（build など）の時間が、off に対し決めた範囲（案: 10% 以内の悪化）に収まる。
4. `CONFIG_DTB=n` で今の動きと同じ（統計の追加の負担は測って小さい事を示す）。build warning 0（全 platform）、`boot-test.sh` の回帰（T）。
5. 規約の全文と文書。

## Phase（本体は [design.md](design.md) §7、±50%、2026-10-31 P5 で書き直した）

| Phase | 内容 | 状態 | 依存 | LW |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | 検討と設計: 情報 → 性質 → hint の表、規則の強さの順、sched の口、統計の場所と負担、config、試験と受け入れ、design-reviewer、ユーザーの問い | in-progress（第 2 版で review（blocker 2・major 11・minor 13）を反映済み、ユーザーの答え待ち） | — | — |
| p002 | config（`CONFIG_DTB`）、`dtb.h`・`sched-hint.h` の型、統計（thread の計数・syscall の入口・tick・RUNNABLE の所、process の GPU と表示の時刻、gpu.c・input.c の通知）、`base_priority` の初め | planned | p001、Q302-5 | 3 |
| p003 | sched の口: base の priority と `sched_hint_set`、kernel の中の優先、CPU の種類の表、行き先の選び、置き場所・盗み・tick の助け、`sched_hint_place`、`sched_set_cpu` の確かめ直し、走っている thread の手渡し、飢えの防ぎ、計数、`CONFIG_DTB_TEST` の偽の種類と stress（QEMU 30 分） | planned | p002、Q302-2・3 | 7 |
| p004 | DTB の本体（dtb.c・dtb-policy.c、分類・履歴・nice の段・HID の状態・周期・予算、`dtb=off`、`kern.dtb`） | planned | p003、Q302-1・6 | 6 |
| p005 | （条件つき）compositor からの前面の app の hint（Q302-4 で作る時か、p006 の数で要る時だけ） | — | p004、UAPI の決定 | 3 |
| p006 | 5330 の評価（on・off の比べの 1 本の script）と閾値の調整 | planned | p004、WS333 p011・p013（電力の meter と相対の性能）、5330 の割当 | 4 |
| p007 | 規約の全文の見直しと文書 | planned | p006 | 2 |

合計 約 22 LW（p005 を立てれば 25）。旧い表（約 32 LW、WS301 の Phase に依存）は git の履歴にある。

## 判断が要る点

- D1: 決定済み（2026-10-27 09:39 ユーザー（クリック）「CONFIG_DTB の既定は off」、上の原文）。
- D2: → Q302-1（推し: nice → 忙しい計算 → 画面を作る process → GPU の app → terminal、terminal は兄弟も空いた P がある時だけ）と Q302-2（忙しい計算の空いた P への溢れ）。
- D3: 決定済み（2026-10-27 09:39 ユーザー（クリック）「nice は core の選択に加えて sched の priority にも効かせる」、上の原文）。今は sched が nice を読まない（survey-source.md §2）ので、飢えを防ぐ対応（段か重みか上限か）は p001 の設計で決める。→ user の thread を kernel の worker（8）より上に置かない段（DTB が on の時の user の既定 10、上げは 8・9）と 100 ms の飢えの防ぎ（design.md §4.7）。段と on の時だけにするかは Q302-3。
- D4: → Q302-4（推し: v1 では作らず、p006 の数で要れば条件つきの p005）。
- D5: → Q302-5（推し: `CONFIG_DTB=y` の build の時だけ取る）。
- 新: Q302-6（観察の読みだけの UAPI `kern.dtb`）、Q302-7（受け入れの数、design.md §6.4）。

## 再開点（2026-10-31 P5、q1775-i01）

- p001 の設計 [design.md](design.md) 第 2 版（main 3d820d9ca の code で書いた。第 1 版 f2d6ca7b4 に review を反映）、header の案 [proposed/include/kern/](proposed/include/kern/)（`sched-hint.h`・`dtb.h`、UAPI の案 `proposed/include/uapi/dtb-sysctl.h`）、問い [user-questions.md](user-questions.md)（Q302-1〜7）、review [design-review.md](design-review.md)。HAL は変えない（design.md §5）。
- 次: ユーザーの答え。p002 は Q302-5、p003 は Q302-2・3、p004 は Q302-1・6 の答えの後（推しで先に始めるかは Q1、design.md §10.2 の 5）。
- 前の再開点（2026-10-29）の source の棚卸し [survey-source.md](survey-source.md) は design.md §2.8 で引き直した。§0 の ws.md の食い違いは既に直っていた。
