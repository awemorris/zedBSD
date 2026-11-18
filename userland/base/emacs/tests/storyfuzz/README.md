# storyfuzz — exploratory system testing for remacs

Coherent editor-operation **stories** (typing, isearch, SKK input and
conversion, window juggling, kill/yank, query-replace, minibuffer
prompts, paste bursts, ...) are synthesized from a random seed,
randomly interleaved, and randomly aborted mid-story with C-g. Many
seeded sessions run in parallel against a real pty; a session fails
when the editor

- **CRASH**: dies on a signal,
- **RT_ERROR**: trips `noct_error()` / `rt_error()` (the launcher
  prints `file:line: Error: ...` on stderr and exits nonzero),
- **EXIT(n)**: exits nonzero any other way,
- **SLOW(n s)**: exceeded the grace period after C-x C-c but was
  still burning CPU and finished within 90s — a throughput finding,
  not a livelock,
- **HANG**: idle past the grace period (a true livelock/deadlock).

## Usage

```
# explore: 200 sessions, 8 workers, random base seed
python3 storyfuzz.py --runs 200 --jobs 8

# fixed sweep (repeatable)
python3 storyfuzz.py --runs 400 --jobs 8 --base-seed 20000

# reproduce one failure, printing every key sent
python3 storyfuzz.py --seed 12345 --verbose
```

The byte stream is deterministic per seed. Failures land in
`failures/seed-<N>/` with `keylog.txt` (labeled steps + replay
command), `stderr.txt`, and `screen-raw.txt` (the last 8KB of
terminal output — the final frame shows where the session was).

Knobs: `--stories` (stories per session), `--abort-p` (per-step C-g
switch probability), `--time-limit`, `--remacs`.

## Debugging a failure

1. `--seed N --verbose` to watch the story; `keylog.txt` names each
   step.
2. `screen-raw.txt` tail = the last frame the editor drew; the frozen
   echo line usually names the active mode (e.g. `C-u-`).
3. For stalls, `tools/profile_lines.py` spawns remacs with
   `REMACS_NO_JIT=1` (interpreted frames keep `env->line` fresh),
   replays a scenario, and samples the executing Noct source line via
   gdb into a histogram. Edit `drive()` to the scenario under
   investigation.

## Story set

The base stories are small coherent scenarios; the **load-heavy**
group stresses throughput: `key_hammer` (one key ×50-200 in a single
burst), `hammer_pairs` (two keys alternated ×100), `scroll_hammer`,
`isearch_real` (search over freshly typed text with repeats and a
backward pass), `file_cycle` (open → edit → save → kill → reopen),
`window_walk` (split both ways, hop C-x o ×30), `huge_paste`
(100-300 lines or one multi-KB line in one chunk), `long_line_nav`
(a heavily wrapped line, then navigation hammering).

## Bugs found by this framework (2026-08-09, first two sweeps)

- **User errors killed the editor**: `error()` ("No further undo
  information", "End of buffer", "Kill ring is empty", ...) unwound
  out of `noct_enter_vm` and terminated the session. Fix: the command
  loop wraps `dispatchKey` in `System.pcall` (new core API — a
  protected call, the condition-case of the command loop); the message
  lands in the echo area and editing continues.
- **Quasi-hang: paste into a vertically split window** (regression
  test `term/case_split_paste.py`): the scroll-fit loop in
  `renderWindow` estimated one display row per logical line; with
  wrapped lines it recomputed the same start line up to the 500-pass
  guard on *every* frame (seconds per keystroke). Fix: force progress
  toward the point's line and stop when a single logical line
  overflows the window.
- **Busy spin on tty EOF** (found while debugging the above): when
  the terminal went away, `poll()` returned instantly forever and the
  main loop spun at 100% CPU. Fix: the POSIX backend tracks EOF,
  `Term.readKey` returns -2 ("gone for good", vs -1 timeout), and the
  main loop exits; the Win32 backend maps WAIT_FAILED the same way.
- **Modal loops redisplayed per hammered key** (found by
  `key_hammer` landing inside isearch / C-u / minibuffer prompts):
  those input loops drew a full frame per queued key, so a 200-key
  burst cost 200 redisplays. Fix: like Emacs, they skip the frame
  while `Term.pendingInput()` is nonzero.
- **Win32 console: large pastes silently truncated** (wine smoke
  test): the event queue held 64 events and `drain_input` dropped the
  overflow. Fix: 1024-slot queue plus flow control that leaves the
  rest in the console's own buffer.
- **Paste throughput ~560 bytes/s** (found by `huge_paste`, first
  classified as HANG): every pasted character paid full per-key
  dispatch bookkeeping plus its own undo entry — 9KB took 16s, 25KB
  over a minute. Fix: the main loop batches runs of plain unbound
  characters into one insert (`selfInsertRun`, one undo group per run
  like the amalgamation rule): 9KB now 0.25s, 25KB 1.25s.
- **Quadratic isearch pastes**: each character pasted into isearch
  re-searched the whole buffer. Fix: while input is pending only the
  search string extends; one search runs when the queue drains.
- **O(buffer) backward addressing in multibyte buffers**: the
  char→byte cache only worked scanning forward and was dropped on
  every modification. Fix: walk backward from the cache when closer,
  and re-anchor the cache at the modification point (whose char/byte
  pair the edit just computed) instead of invalidating.

A classifier bug in the framework itself was also caught: after the
harness killed a SLOW>90s session, the negative return code was
re-classified as CRASH(sig 9) — the framework was reporting its own
kill. The CRASH check now applies only when the harness did not kill.

Final sweep (seeds 200000-200999, all fixes applied): 968/1000 OK,
32 SLOW (13s-90s+), 0 CRASH, 0 HANG, 0 RT_ERROR. 30 of the 32 SLOW
sessions combine SKK mode with a load story.

Residual, known and accepted for now: with SKK enabled every pasted
character must run the kana composer individually, so a multi-KB
paste in SKK mode drains at per-key dispatch speed — sessions
combining SKK with `huge_paste` classify as SLOW (tens of seconds of
backlog, then a clean exit). No livelocks remain.

## Notes

- The pty transcript check tolerates timing jitter, but a failure
  seed is deterministic in the bytes it sends; a rare timing-dependent
  failure may need a few replays to fire again.
- Not part of `run-all.sh` (randomized); run sweeps manually or from
  CI with a fixed `--base-seed`.
