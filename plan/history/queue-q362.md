<!-- awesome-plan project=zedbsd record=queue-q362 -->

# Queue q362: Wayland を POSIX で（ws034-p050）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「POSIX の範疇で wayland を構築できないか検討してみてください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q362-i01 | [ws034-p050](../ws034/phase050/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q362-i01 | ws034-p050 | **cleared**。GLib・GTK3/4・Qt5/6・upstream wayland 等を読んだ結果、client の toolkit は `poll` で動き、epoll・timerfd・signalfd は upstream wayland の server の event loop だけが使う。eventfd・memfd は pipe・shm_open 等に落ちる。libc に `mkostemp`・`posix_fallocate` が足りない（新 p053） |
