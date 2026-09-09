# WS019-p024 results

Date: 2026-09-09
Status: completed (q158)

The canonical Noct package adds patch 0003-terminal-partial-input-progress.patch
and advances local patch level from zedbsd3 to zedbsd4, retaining upstream
bb239816e20294c073702dc127142f6767375072. The sole input-fill caller is the
terminal decoder. It now polls for genuinely new bytes while retaining partial
events, limits reads to available ring space, distinguishes lone Escape timeout
from incomplete sequences, and terminates partial input at EOF. Full incomplete
rings and excessive numeric CSI parameters no longer grow/spin indefinitely.

The installer confirmation uses actual stdin/stdout terminal checks, exact
destination text, bounded editing, cancellation and protected terminal cleanup.
Native console Enter emits LF, represented by Noct as Ctrl-J; confirmation
accepts this as well as CR. Neither a mismatch nor excess input can approve.
Root authority and public installer orchestration remain p004 responsibilities.

## Evidence

- `/tmp/zedbsd-q158-noct-old-verify.log`: original host/target manifests passed
  before preserving extracts under `../temp/q158-noct-old`.
- `/tmp/zedbsd-q158-noct-new-verify.log`: new host/target identities and source
  manifests pass canonical verification.
- `/tmp/zedbsd-q158-noct-host.log`: host Noct build passed.
- `/tmp/zedbsd-q158-terminal-host2.log`: 18 real-PTY confirmations, two
  fragmented CSI/UTF-8/SS3 sequences with timeout/resume, terminal restoration,
  two noninteractive refusals and eight EOF cases, across JIT/interpreter.
- `/tmp/zedbsd-q158-{amd64,pcat,pc98}.log`: maintained make -j16 builds passed.
- `/tmp/zedbsd-q158-fixture2.log`: updated private fixture build passed.
- `../temp/q158-confirmation3/result.json`: eight native JIT/interpreter
  exact/escape/control/mismatch cases passed, followed by normal `id -u`.
  GPT, FAT boot sector, sentinel and production image hashes are unchanged.
  Runner session 60939 was polled to exit 0.

Historical failures remain: confirmation's original LF rejection in
`q158-confirmation`, and the harness's attempted exec of shell builtin echo in
`q158-confirmation2`. The latter run passed all eight confirmation cases but
failed the final harness check. It was corrected to existing `id -u` and rerun.

No public installer command is shipped by these component tests. Continue
p004 mount ownership, source/configuration uniqueness, capacity, publication
and recovery integration; p005 installed boot remains unexecuted.
