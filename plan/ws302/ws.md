<!-- awesome-plan project=zedbsd record=ws302 -->

# WS302 デスクトップスレッドバランサ（DTB）

- Master: [plan/master.md](../master.md)（2026-10-27 作成、q1615 P1 の計画）。Primary Milestone: MG006（グラフィカルな操作環境の応答）。Related: MG004、[WS301](../ws301/ws.md)（core の相対の性能・domain・topology、前提）。
- Status: planning（2026-10-27 新設。計画だけ）
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

- sched（`src/kern/sched.c` 2952 行、`include/kern/sched.h`）: CPU ごとの run queue、thread は CPU に固定、移動は `sched_set_cpu()`（:1665-1758）だけで、**走っている thread は移せず（EINVAL）、sched.c の外に呼び手は無い**。
  16 段の priority、全部の thread が既定の priority で作られる。affinity・cpuset は無い。負荷の分散は idle の CPU の盗み（:2528-2599）と、忙しい CPU が idle の CPU を起こす（:1092-1113）だけで、
  core の性能・topology を知らない。hint の口は無い。
- **nice は保存されるが sched は読まない**（`setpriority` は `process->nice_value` に入れるだけ、`src/kern/syscall.c:8863-8935`（main 61630bb0b）、sched.c に nice は無い）。
- 統計: process ごとの tick（cpu・user・system、`include/kern/process.h:119-124`）、`controlling_tty`（:145）、`command`。thread ごとは無い（`kern.perf=1` の時の診断だけ）。
  syscall の数・最後の syscall の時刻・自発／非自発の切り替え・wake の数は無い（`ru_nvcsw`・`ru_nivcsw` は宣言だけ）。足す所は `syscall_dispatch_body`（syscall.c:9796、main 61630bb0b）、`sched_wakeup`・`sched_yield`・`switch_without_enqueue`。
- HID: `src/drivers/generic/input.c` に kernel の購読の口 `drv_input_subscribe`（:1517、最大 8、今は console だけ）。数・最後の入力の時刻は無い。
- GPU: `/dev/gpuN` の `struct gpu_session`（`src/drivers/gpu/gpu.c:219`）に process ごとの使用量は無い。device ごとの `hw.gputelemetry`（busy_ns など）だけ。
- core の性能・topology の口は無い → [WS333](../ws333/ws.md)（WS301 を統合した WS）が作る。

## 範囲

- 含む: 「どの情報からどんな balancing ができるか」の検討（p001、ユーザーの「まず」）、nice を sched の priority に効かせる事（2026-10-27 09:39 ユーザーの決定、上の原文）、sched への依頼の口（hint、走っている thread の安全な点での移動）、
  統計の追加（thread・HID・GPU）、DTB の本体（周期の評価・分類・hint・履歴の幅）、config、観察の道具、5330 の評価。
- 含まない: sched の作り直し（hint を受ける最小の変更だけ）、DVFS の policy（WS301）、他の機種の評価（hybrid でない機械では「速い core」が無い。電力の寄せ方だけ後で）。

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

## Phase の案と見積もり（LW、±50%）

| Phase | 内容 | 依存 | LW |
| --- | --- | --- | --- |
| p001 | 検討と設計: 上の表を 5330 の実測（今の sched での P/E への散り方、各情報が取れるか）で埋め、衝突の規則・物差し・sched と DTB の口・統計の追加の場所と負担・config、design-reviewer、ユーザーの review | WS301 p001（API の形） | 6 |
| p002 | sched の口: thread ごとの core の種類の希望（強さ付き）、走っている thread の安全な点での移動、配置と盗みで希望と core の性能を見る | p001、WS301 p002 | 6 |
| p003 | 統計: thread ごと（実行の時間、syscall の数と最後の時刻、自発／非自発の切り替え、wake）、GPU の session ごとの submit と busy、HID の最後の入力と頻度 | p001 | 5 |
| p004 | DTB の本体（`CONFIG_DTB`、周期の評価、分類、hint、履歴の幅、観察の道具） | p002、p003、WS301 p004 | 6 |
| p005 | compositor の focus の hint（D4 で採る時だけ、UAPI の決定の後） | p004 | 3 |
| p006 | 5330 の評価（on・off の比較の script）と閾値の調整 | p004 | 4 |
| p007 | 規約の全文の見直しと文書 | p006 | 2 |

合計 約 32 LW（p005 を採らなければ 29）。

## 判断が要る点

- D1: 決定済み（2026-10-27 09:39 ユーザー（クリック）「CONFIG_DTB の既定は off」、上の原文）。
- D2: 規則の強さの順（nice・GPU・tty・長い計算）。P-core が 2 つ（4 thread）しかない 5330 で、GPU の app と terminal の app が競う時。
- D3: 決定済み（2026-10-27 09:39 ユーザー（クリック）「nice は core の選択に加えて sched の priority にも効かせる」、上の原文）。今は sched が nice を読まない（survey-source.md §2）ので、飢えを防ぐ対応（段か重みか上限か）は p001 の設計で決める。
- D4: 前面の app を kernel が知る口（compositor からの focus の hint の UAPI）を作るか。作らなければ HID は「人が操作中か」だけに使う。
- D5: 統計の追加（syscall の数と時刻）を DTB が off の時も常に取るか（負担の数を p001 で示す）。

## 再開点（2026-10-29 P5、q1666）



- source の棚卸し [survey-source.md](survey-source.md)（Opus の調べ、Q1 の指示で止めた。未調査は §7）。§0 の ws.md の食い違い（:21 の決定と :45・:64・:93・:95）は Q1 の確かめの後に直す。再開は p001 の設計（WS301 p001 の後、WS309 の thread の種類と合わせる）。
