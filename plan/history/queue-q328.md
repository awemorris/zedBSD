<!-- awesome-plan project=zedbsd record=queue -->

# Queue q328: USB 列挙 UAPI と lsusb（ws034-p004）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q327 finished（履歴 `plan/history/queue-q327.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23、`plan/master.md`「夜間の自律実行」）。
UAPI の形は WS034 の決定（2026-09-23 ユーザー承認）に従う。
Start UTC: 2026-09-23T13:00:00+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q328-i01 | [ws034-p004](../ws034/phase004/phase.md) | cleared | `KERN_SYSTEM_GET_USB_DEVICE` と `userland/base/lsusb` |

## 範囲外

HALの変更。aggregate `make check`。commitは `git commit -m WIP` のみでpushしない。

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q328-i01 | ws034-p004 | **cleared**。`lsusb [-tv]`。QEMU の `info usb` と一致、hot-plug 中の300回で失敗0 |
