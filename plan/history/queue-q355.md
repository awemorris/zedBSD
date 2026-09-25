<!-- awesome-plan project=zedbsd record=queue-q355 -->

# Queue q355: audiod の設計（ws035-p050）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（audiod の方針と「unix socket interface として設計してください」「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q355-i01 | [ws035-p050](../ws035/phase050/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q355-i01 | ws035-p050 | **cleared**。[audiod-design.md](../ws035/audiod-design.md): socket の message、共有メモリの作り方と配置、mix の 2 経路、`/dev/dsp` の mmap の UAPI（p049 コピーあり → 新 p056 ゼロコピー）、libpulse との対応 |
