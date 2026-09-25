<!-- awesome-plan project=zedbsd record=queue -->

# Queue q325: networkd購読の検証、libzdesktopの枠、lspci

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q326 finished（履歴 `plan/history/queue-q326.md`）。その前は q324 finished
<!-- awesome-plan-current:end -->

Approval: current user「queueを承認します。投入してください。」（2026-09-23、候補の提示後）
Start UTC: 2026-09-23T06:00:00+00:00

## 選定

3件とも前提が解けており、**項目同士に依存が無い**。
実行はメインセッションが上から1つずつ行う。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q325-i01 | [ws035-p018](../ws035/phase018/phase.md) | cleared | networkdの状態push通知（購読）の**検証**。実装はq323でtreeに入り、buildは通っている |
| 2 | q325-i02 | [ws035-p042](../ws035/phase042/phase.md) | cleared | libzdesktopの**空の枠**（`/lib/libzdesktop.so`） |
| 3 | q325-i03 | [ws034-p003](../ws034/phase003/phase.md) | cleared | PCI列挙UAPI（`/dev/system`）とlspci |

## 順序の理由

1が最初なのは、ユーザーの指示「元のnetコマンドのテストのphaseを再度投入しましょう」（2026-09-23）による。
q324 で入れたシリアルコンソールの受信を使って検証する。**キー入力は使わない。**

## 入れなかったもの

- **ws035-p039**（USB CDC-ECM の実機確認）: ユーザー指示で後回し。
  p018 の検証には ECM を使わず、実績のある **NE2000（`ne0`）** を使う
  （`plan/ws011/tests/` に前例があり、`CONFIG_DRIVER_NE2000=1`）。
- **ws035-p038**（SSHハーネス）: p039 の後。
- **ws035-p040・p041**（互換 libz・libpng）: zdesktop が要るようになったときに入れる（D6）。

## 範囲外

上記の入れなかったもの、WS036、WS031。HALの変更は差分ごとに事前承認。
aggregate `make check` は使わない。commitは `git commit -m WIP` のみでpushしない。

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q325-i01 | ws035-p018 | **cleared**。上限がちょうど8（9つ目は `EBUSY`）、切れた購読者の枠が空くこと（kill 後に新しい購読が入る）を amd64 KVM で確認。最初のフレーム・変化の通知・`EBUSY` は一時停止前に確認済み |
| q325-i02 | ws035-p042 | **cleared**。`/lib/libzdesktop.so`（`zdesktop_version()` だけ）、`include/libc/zdesktop.h`。SONAME・依存・export を確認、boot-test PASS |
| q325-i03 | ws034-p003 | **cleared**。`KERN_SYSTEM_GET_PCI_DEVICE`（72 byte の固定 layout）と `lspci [-Dkv]`。QEMU の `query-pci` と一致 |

## 一時停止（2026-09-23）

USB storage の timeout が 10分の1 になっていた件（[WS040](../ws040/ws.md)）を先に直すため、
ユーザーの「実行してください」（WS040 を先にする提案に対して）により一時停止した。

- **q325-i01（ws035-p018）**: 検証の大半は済んだ（最初のフレーム、`net down`→offline、
  `net up`→online、上限超過で `EBUSY`）。残りは「購読者が切れたら枠が空く」「上限が正確に8」の2点。
  途中で daemon と `net watch` のバグを3つ直した（通知の id 0 が encoder に拒否される、
  半閉じ要求と POLLHUP の衝突、拒否を黙って捨てる）。**cleared にはしていない。**
- q325-i02（p042）・q325-i03（ws034-p003）: 未着手。

WS040 の後に再開する。
