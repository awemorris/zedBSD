<!-- awesome-plan project=zedbsd record=queue-q356 -->

# Queue q356: `/dev/dsp` の mmap interface（ws035-p049）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（`/dev/dsp0` の OSS mmap、コピーありで可、「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q356-i01 | [ws035-p049](../ws035/phase049/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q356-i01 | ws035-p049 | **cleared**。GET_CAPS・GET_OPTR/IPTR・SET_TRIGGER と mmap（shadow ring を fragment ごとに copy、HAL 変更なし）。`O_RDWR` は hardware にある方向だけを取る（write-only の device は mmap できないため）。QEMU で mmap 再生 bit 一致、mmap 録音は実時間どおり、write の経路の退行なし |
