<!-- awesome-plan project=zedbsd record=ws333-p027 -->

# ws333-p027: 5330 の残り（package の C-state 0% の原因、i915 の qs40、S0 idle）

Status: planned（2026-11-13 Q1。p014 の残りを移した。5330 の遠隔の電源が無い中で起動しないかもしれない試験の扱いはユーザーの判断待ち）
Parent: [WS333](../ws.md)

## 目的

[p014](../phase014/phase.md) の受け入れのうち、[p025](../phase025/phase.md) の後に残った物を確かめる。

1. **package の C-state 0%**: zedBSD は全 CPU が core C6 に約 90% でも PC2 0%（idle 5.4 W）、Debian（graphics の driver 無し）は PC2 99%（3.3 W）。Debian で 1 ms の tick を真似ても PC2 64.6% なので tick は主因でない。残る差は zedBSD が i915 を動かしている事（GT の RC6、display の DC state、`DMC program not loaded`）。i915 の GT RC6 の滞在・forcewake の数を読めるようにし、`i915.start=manual` との A/B で確かめる。原因が i915 なら i915 の WS へ渡す。
2. **i915 の qs40 × 600**（WS291 p022 の mview と model を 5330 の image に置く）。
3. **S0 idle** の回帰と S0 idle の後の要求（`sleepctl s0idle`、起こす事象が要る）。
- tick を止めるか（design.md §4.3.3 段 4、[p023] は未立て）は 1 の数の後に決める。Debian の真似で tick は約 1.1 W。

## 実機の危険

5330 は遠隔の電源が無い。`i915.start=manual`・S0 idle・新しい image は、起動や復帰に失敗すると Q1 も担当も戻せない。
