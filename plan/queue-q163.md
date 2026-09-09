# Queue q163: whole-disk GPT initialization codec

Date: 2026-09-09
Status: finished
Authorization: standing autonomous Priority instruction; queue presented before implementation
Timebox: 60 active minutes
Previous: [q162](queue-q162.md)

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p030](ws019-installation/phase030-gpt-initialization-codec/phase.md) | completed | Prepare complete GPT on blank/old media; bounded writes, preimages, fault verification |

This prerequisite does not expose destructive CLI initialization before
exclusive block-device admission is implemented. p006 owns admission, command
integration, formatters and automatic layout; p007 owns native installation.
BeUI remains mandatory p029 after text installation.
[q163 results](ws019-installation/phase030-gpt-initialization-codec/results.md):
ordinary/sanitized parser/editor/CLI and independent GPT inspection pass;
amd64/pcat/pc98 disk-image builds pass. No destructive live CLI was added.
Capture and display the installer framebuffer on the next installer execution.
WS004-p050 remains mandatory in this goal after the full installer is finished.
