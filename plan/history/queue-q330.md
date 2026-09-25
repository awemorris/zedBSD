<!-- awesome-plan project=zedbsd record=queue -->

# Queue q330: ca-certificates（ws034-p019）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q329 finished（履歴 `plan/history/queue-q329.md`）
<!-- awesome-plan-current:end -->

Approval: ユーザーの夜間の自律実行の指示（2026-09-23、`plan/master.md`「夜間の自律実行」）。
入手元と置き場所は inventory §7 の既定案 (a)。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q330-i01 | [ws034-p019](../ws034/phase019/phase.md) | cleared | curl の PEM を `/etc/ssl/cert.pem` へ。`external.mk` に1ファイルの取得 |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q330-i01 | ws034-p019 | **cleared**。Debian の certdata と121件一致。ゲストの OpenSSL が既定の CA path で実 chain を検証 |
