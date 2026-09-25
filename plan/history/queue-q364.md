<!-- awesome-plan project=zedbsd record=queue-q364 -->

# Queue q364: audiod（ws035-p009）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（audiod の方針）と「作業を継続してください」。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q364-i01 | [ws035-p009](../ws035/phase009/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q364-i01 | ws035-p009 | **cleared**。`/sbin/audiod`（共有メモリの ring、SCM_RIGHTS、mmap と write の 2 経路、mix・変換・録音）。QEMU で bit 一致・2 stream の和・音量・44.1 kHz・録音・縮めた client に耐える・device 無し。libc の `<sys/socket.h>` の `struct iovec` も直した |
