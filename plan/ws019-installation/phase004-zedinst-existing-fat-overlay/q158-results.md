# q158 results

Date: 2026-09-09
Queue: finished; ws019-p004 uncleared, ws019-p024 completed

Integrated discovery, owned source/destination mounts, configuration uniqueness,
immutable inputs, existing-file/capacity admission, actual terminal confirmation,
post-pause revalidation, six-file publication and cleanup. The command's Noct
entry point is exercised unchanged in the private test image.

`../temp/q158-install2/result.json` passes cancel, install and exact rerun.
Session 41645 ended with exit 0. Cancellation preserves the whole destination;
GPT, FAT boot sectors, both sentinels, OVMF variables and the production image
compare unchanged after the complete run. All six files publish and cleanup
returns successfully. `published-input-comparison.json` independently compares
the copied loader/kernel/rootfs and selected-payload configuration during rerun.

Component evidence and historical failures are retained in [progress](q158-progress.md).
Noct's terminal-input fix is completed in [p024](../phase024-noct-terminal-input/results.md).
Host lifecycle, destination subset/conflict and terminal tests passed; native
source/destination preflight, capacity and cleanup passed before integration.
The last seed-mode correction normalizes its owned empty file to 0755 instead
of inheriting /bin/sh's observed 0775.

Full p004 remains uncleared: apply the privately tested complete-rerun fix,
register and verify the public package, then exercise its launcher and conflict
refusal. Complete-destination runs must not require a stage directory/seed or
extra disk capacity. Public installed boot remains p005. No process remains live.
