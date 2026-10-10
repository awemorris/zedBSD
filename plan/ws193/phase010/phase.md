<!-- awesome-plan project=zedbsd record=ws193-p010 -->
# WS193 p010: ユーザーランド配置定義のCPU共通化

Status: cleared
Disposition: normal
Parent: [WS193](../ws.md)
Queue: [Codex runtime](../codex-rpi4-sshd-queue.md), runtime-manifest-20261011-i01

## 承認・範囲・受入

2026-10-11 userのRPi4 sshd失敗写真はld.so: cannot open dependency。現在のssh-keygenは動的AArch64 ELFでlibutil.so/libcrypto.so/libc.soを要求し、現在UFSにlibutil.soが無い。user「アーキテクチャ非依存な定義に変えてほしいです。」により、全6platformの配置一覧を共通化し、libutilを動的ABIの共通runtimeに含める。CPU固有のcompile/linkは保持。静的ABIのみのX68kに動的linkerの移植は含めない。共通配置を変更した際の増分再配置も受入に含める。

専用worktree codex/rpi4-sshd、main基点3ba73d2c8。AGENTS/Guardrailの全文規則、Make既存recipe/registryを適用。共有LLVMはreadonly、現在sysrootは複写、toolchainを変更/buildしない。main統合は具体的commitを確認、push/実機/QEMUは実施しない。共有Master/Queue/history/cache/GitHubの投影はQ1 pending。

受入: 全CPUが共通manifestを使い同一選択の配置が一致（静的ABI runtime例外を明記）、libutilのAArch64 build/ELF検査、OpenSSHの実DT_NEEDEDと配置の照合、既存rootfsの増分無効化、最終変更の全規則review/diff-check。全optional packageの全CPU移植/起動成功は主張しない。

## Final outcome / 2026-10-11

runtime-manifest-20261011-i01 cleared for the bounded common rootfs definition / AArch64 build / runtime-layout criteria. All six root-tree rules use one manifest; five dynamic selections agree, X68k static exception is explicit. AArch64 and amd64 named libc/libutil builds have zero warnings; full current amd64 installation map preserved and RPi4 gains only libutil. Actual Make root staging / OpenSSH DT_NEEDED closure / cached-file incremental invalidation PASS. Final changed Make rules reviewed in full; diff-check PASS.

[Evidence and remaining port limitations](../tests/runtime-manifest-20261011.md) records unrelated i386/SPARC/X68k libc build failures and native bootstrap image limitations. These are not runtime success claims or hidden universal-image criteria. Physical RPi4 host key creation, whole-WS acceptance, and shared projections remain pending. Main exact commit approval will be requested; no push.

## Main integration follow-up / 2026-10-11

User explicitly approved main integration of `1b3d6cd03` (answer「mainへ統合する」). Clean main at 3ba73d2c8 fast-forwarded to the exact verified source commit; read-back confirms all 16 integrated files match the tested worktree. Source integration complete; preceding approval-pending text is historical. Documentation follow-up records this integration within the same approved scope. RPi4 physical host-key/SSH execution and whole-WS acceptance remain pending. No push; shared Master/Queue/history/cache/GitHub reconciliation stays Q1 pending.
