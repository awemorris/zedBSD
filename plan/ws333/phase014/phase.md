<!-- awesome-plan project=zedbsd record=ws333-p014 -->

# ws333-p014: 5330 の CPU の受け入れ

Status: planned（2026-11-12 P1 が design.md §9 から立てた。実機の割当が要る）
Parent: [WS333](../ws.md)

## 目的

design.md §8.4 の 5330 の 1〜4 と §1 の受け入れの柱を、1 本の script（配置 → 起動 → 実行 → 回収 → 要約）で確かめる。§4.3.3 の段 4（idle の CPU の tick を止める、HAL の契約の外）の要否の数を取る。WS299（5330 の NVMe の速さ、CPU の idle と P-state が律速と見られる）の待ちを解く数にもなる。

## 依存

- p011（x86 の driver）、p012（tick の位相）、p013（`/sbin/thermal` の CPU と strategy。p004 の答えが遅れる時は p005 後半の注入の口か、試験だけの diag の patch で strategy を選ぶ）。
- Q1 の実機の割当（5330）。

## 受け入れ（design.md §8.4）

1. idle の電力: 同じ image で kernel を替え HLT（`IDLE_WAIT_DEEPEST_CSTATE=1`）・MWAIT C6（既定）・C10・tick の位相の有無（`LAPIC_TICK_TSC_DEADLINE`）・strategy BALANCED の各 60 秒、package の電力（RAPL の meter）と package・core の C-state の滞在。同じ日に Linux の値。
2. i915: WS291 p022 の qs40 × 600 の fps と GT の周波数。
3. HWP の 4 strategy と直の設定で 1 CPU の spin の MHz が要求に従う。E-core の 4 つ組の共有。
4. `cpuperf.hwp=off`・相対の性能の基準・S0 idle の回帰と S0 idle の後の要求。
- PASS: 1〜4 の数が全部取れ、MWAIT の idle の package の電力が HLT より低く、3 の MHz が要求に従う。数の目標（Linux との差）は design.md §11.2 の 9 で Q1 が決めた値。package の深い状態の滞在が 0 のままなら、妨げの候補（ASPM・PSR）を他の WS に渡す材料として記録する。

## 他の Phase から持ち越した 5330 の確かめ

- p002: MWAIT の路を通った事の直の観察（C-state の滞在で）。
- p011: 全 CPU の負荷で performance の P-core が 1.4 GHz に留まる理由（PLATFORM_LIMIT の印 0x8）、E-core の 4 つ組の clock の共有、0xE2 の lock の bit、MC6。
- p012: package の C-state（PC2〜PC7）が両方の tick の形で 0% の理由、全 CPU が同じ時刻に tick を処理する事の遅れ・lock の取り合い。
- p018: boost の (a)(b)（今は既定 n）。SMI の数の実機での数え。
- p021: passive と cpu の受け手の実機の結合（5330 では動く場面が無い見込み）。

## 既に取れた数（2026-11-01 q1788、試験だけの diag の patch、main 82c6ef325）

- p011 と p012 の phase.md の表（idle の package: periodic 10.08 W → TSC-deadline 6.44 W、Debian 3.75 W。HWP の strategy の request、HFI、meter 4 本、`cpuperf=off`）。受け入れの 1〜4 の全部ではない。
