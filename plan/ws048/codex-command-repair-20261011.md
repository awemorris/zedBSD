# Codex Queue: rpi4-command-repair-20261011

Status: finished
Owner: Codex / codex/rpi4-sshd
Approval: current user「USBはだめ、Ethernetは動きました。」とEnable Slot timeoutの実機log、SSH接続先「ssh kei@10.0.30.2」。既存USB修正指示の継続。
Finite scope: VL805 command completionを妨げるPCIe DMA bus/physical address不整合とdoorbell posted write処理を修正、失敗時の診断を追加し、短いhost確認と現在configのwarning0 kernel build、最終C全文reviewまで。実機boot/reboot、HAL API、MSI追加、toolchain/pushは含まない。GPU追加logは今回は診断記録。

| Attempt | Phase | Status | Dependencies |
| --- | --- | --- | --- |
| rpi4-command-repair-20261011-i01 | [ws048-p011](phase011/phase.md) | cleared | main402598d27、SSH実機Enable Slot timeout |

Graph: main402598d27 (context) → i01 → user physical USB acceptance (context)。1Phaseの限定Queue。main統合は具体的commitの承認後。共有投影/GitHubはQ1 pending。

## Terminal result

The selected source/build item cleared. [Evidence](tests/command-repair-20261011.md), [Phase](phase011/phase.md). Original constraints/identity API and HAL/UAPI retained. Actual USB input acceptance remains uncleared, GPU log investigation remains read-only. Concrete-commit main approval and Q1 shared/remote projections pending. No next Queue automatically started.
