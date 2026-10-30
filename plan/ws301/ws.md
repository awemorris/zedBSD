<!-- awesome-plan project=zedbsd record=ws301 -->

# WS301 CPU の省電力の driver の枠組み（DVFS と P-core/E-core の性能）

- Master: [plan/master.md](../master.md)（2026-10-27 作成、q1615 P1 の計画）。Primary Milestone: MG004（既定構成の性能と電力）。Related: MG003、MG008（HAL の契約）、
  [WS302](../ws302/ws.md)（この WS の口を使う）、[WS052](../ws052/ws.md)（電源の UAPI と文書）、[WS303](../ws303/ws.md)（`/dev/thermal`・strategy・passive、この WS の UAPI の置き場所）、[WS310](../ws310/ws.md)（`_OSC`）。
- Status: canceled（2026-10-31 ユーザーの指示で WS333 に統合。/dev/thermal で統一して設計し直すため。この WS の設計は WS333 の入力）
- 出典: 2026-10-27 ユーザー
  「CPUの省電力機能のドライバフレームワークを追加（WSを立てて検討）
  - コアごとのDVFSのステップ(設定可能な値)を取得する機能
  - どのコア群が電圧や周波数のを共有しているかのグルーピングも取得できる機能
  - コアごとのDVFSの状態を設定する機能
  - コアごとのDVFSの状態を取得する機能
  - 一番速いコアを100としたときのコアごとの性能を取得するP-core/E-core機能」
  補足（2026-10-27 ユーザー、Q1 経由の原文）「DVFSとP-core/E-coreは、/dev/systemに入れるのが適切ならそれでもいいですとお伝えください。ファン制御もあるといいですね。」
- 全体の依存と着手の順: [plan/agents/P1/pm-plan.md](../agents/P1/pm-plan.md)。

- 2026-10-27 09:39 ユーザー（クリック）: UAPI は /dev/system、設定は domain ごとの min・max・desired・EPP、設定できるのは root だけ、HAL の差分の案を作る（承認は差分ごと）。対象はまず amd64（5330 の HWP）、arm64・riscv64 は枠だけ。fan は別の WS（[WS303](../ws303/ws.md)）。

## 単一の到達目標

kernel に CPU の性能と電力の枠組み（provider の登録の口）を作り、5330（Alder Lake-P、i5-1245U、P-core 2 と E-core 8 の hybrid）で
(1) core ごとの DVFS のステップ（設定できる値）、(2) 電圧・周波数を共有する core の群、(3) core ごとの DVFS の状態の設定、(4) 状態の取得、
(5) 一番速い core を 100 とした core ごとの相対の性能、を kernel の中（WS302 などの利用者）と user（UAPI）から使えるようにする。
他の機種（Pi 4・Pi 5・VisionFive 2・AMD）は provider を足す後の Phase で、枠組みはそれらを入れられる形にする。


## 設計の要点（写し。本体は [design.md](design.md) 第 3 版の改訂 3）

- UAPI は WS303 の `/dev/thermal`（ioctl の範囲 32〜63、`include/uapi/thermal.h`）。`/dev/system` には足さない（下の 2026-10-29 の決定が 2026-10-27 の「UAPI は /dev/system」を置き換えた）。
- HAL は変えない（Q301-1）。x86 の driver（`src/drivers/platform/pcat/`）が CPUID・MSR を自分で扱い、CPU 間は既存の `hal_cpu_notify` の道。関門は CPUID の bit・hypervisor では動かない・model の allow list・`CONFIG_CPU_PERF` 既定 n。
- WS301 は自分の policy を持たず、WS303 の strategy の受け手（登録は p004）。root の直の設定（domain ごとの min・max・desired・EPP）は診断の口。熱の上限（WS303 の passive）は千分率の口。
- 対象は 5330（HWP と、`cpuperf.hwp=off` の試験の legacy の P-state）。arm64・riscv64 は枠だけ（Q301-5）。`_OSC` は呼ばない（Q301-3、WS310）。fan は WS303。
- 相対の性能は起動の時に測る Highest の MHz の比が既定、基準は root が替えられる（Q301-2）。
- 2026-10-27 の P1 の案（`/dev/system` の推し、HAL の差分、p001〜p009 の表）は git の履歴にあり、今は使わない。

## Phase（写し。本体は design.md §7）

| Phase | 内容 | 依存 | LW | 状態 |
| --- | --- | --- | --- | --- |
| p001 | 調査と設計、design-reviewer、ユーザーの判断 | — | 残り 1 | in-progress（改訂 3、改訂 2・3 の review を待つ） |
| p002 | 枠組み（domain・電圧の群・相対の性能と基準・strategy の受け・直の設定・熱の上限・要求の箱と notify の道・provider の口）と host 試験 | p001 | 5 | planning |
| p003 | x86 の driver の基礎（CPUID・MSR の file、関門、topology、MADT の UID、notify の道） | p002 | 3 | planning |
| p004 | HWP の道（enable、能力と MHz の測り、strategy の対応と受け手の登録、HFI、PECI の flag） | p003、WS303 p003a | 4 | planning |
| p005 | legacy の P-state（`_PSS`・0xCE・AMD の HwPstate、governor） | p003 | 4 | planning |
| p006 | UAPI（`/dev/thermal` の CPU の ioctl）と道具 | p002、WS303 p003a | 3 | planning |
| p007 | 5330 の受け入れ（1 本の script） | p004、p005、p006 | 4 | planning |
| p008 | 規約の全文と文書（WS303 と順を Q1） | p007 | 2 | planning |

合計 約 25 LW（p001 を除く）。WS303 の p007（passive）は WS301 の p002・p004 の後。

## ユーザーの回答（2026-10-29、Q1 の中継、原文）

- 2026-10-29 ユーザー（Q1 の中継、原文）「CPUのスピードは/dev/systemと言ってしまいましたが、/dev/thermalに変更してください。」→ 解釈: UAPI は WS303 の `/dev/thermal`（上の 2026-10-27 の「UAPI は /dev/system」を置き換える）。policy は WS303 の strategy の受け手（design.md の「第 2 版への追記」）。

- Q301-3「最終的には呼ぶが、別なWSにして、システム全体の挙動が問題ないかを厳重にチェックする。今は呼ばない。」→ 解釈: この WS は `_OSC` を呼ばない。Q1 が WS310（`_OSC` を呼び全体の挙動を確かめる、WS301 の後）を作った。
- Q301-4「試験が通るまで無効 (推し)」→ `CONFIG_CPU_PERF` の既定は n、p006（今の番号は p007）の受け入れの後に既定を決め直す。
- Q301-5「枠だけ、実装は後 (推し)」→ arm64・riscv64 は provider の口と既定の答えまで。provider は後の WS。
- Q301-7「持たせない (推し)」（Q1 の注: 利用者が選ぶのは WS303 の strategy だけ、CPU ごとの直の値は root の診断用だけ）→ WS301 は自分の policy・起動の parameter を持たない。domain ごとの直の設定は root の診断の口。
- ユーザーの問い「HWP は必ずあるのか」→ design.md §4.9 と Q301-8（新）。

## ユーザーの回答の第 2 回（2026-10-29、Q1 の中継の原文）

- Q301-2（firmware の値が誤りだった事を伝えて聞き直した。前の答え「firmware の Nominal (推し)」を置き換える）「とりあえず推しにしますが、あとでマッピングを変えられるインタフェースにします。」→ 解釈: 既定は最大の周波数の比（5330 は E = 75）、基準は root が選べ driver が別の源を出せる口にする（design.md §4.7）。
- Q301-8「P-stateを操作しますが、HALではなくドライバでできるはずです。」→ 解釈: HWP の無い CPU も P-state を操作する（legacy の道、design.md §4.9）。HAL を増やさず x86 の driver が CPUID・MSR を扱う。同じ理由で HWP・HFI・topology も driver に移し、HAL を変えない案を推しにした（design.md §4.2、Q301-1 の問いを [hal-review.md](hal-review.md) で改めた）。

## ユーザーの回答（2026-10-29、Q301-1、Q1 が MSR の例外と Linux の rdmsr_safe を説明して聞き直した）

- 回答（クリック）「HAL を変えず確かめで守る (推し)」→ HAL は変えない。driver の確かめ（CPUID・許可の表・hypervisor では動かない・既定は無効）で想定の外れを避け、外れた時は kernel が止まる（有効にした機械だけ）。安全網は必要になった時に別の差分で。
- 途中のユーザーの問い（原文）「MSRを例外を期待してtryでアクセスする実装は、私は個人的には見たことがないのですが、Linuxではふつうなのですか？今の気持ちとしては、HALを変えないことに傾いていますが、再説明してください。」

## 再開点（2026-10-29 P5、q1666-i04）

- design.md 第 3 版の改訂 3: Q301-1 の答え（HAL を変えない）、第 3 版の review の minor m1〜m17、§4.10 の lock の順を WS303 第 5 版（worker から lock なし）に合わせた（行き先は design.md §12）。HAL の差分の案 [proposed/hal-cpu-perf.diff](proposed/hal-cpu-perf.diff) は選ばなかった案 C の記録（当てない）、ユーザー向けの説明は [hal-review.md](hal-review.md)（回答済み）。
- 次: 改訂 2・3 の design-reviewer（design-v3r3-review.md）と反映。ユーザーへの問いは無い（WS303 の Q303-17・18 の答えを待つ所は推し A で書いた）。
