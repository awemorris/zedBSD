# libedit

This is zedBSD's small, clean-room line editor.  The package is named
`libedit`, but it exposes the Readline-compatible headers
`<readline/readline.h>` and `<readline/history.h>` and builds as
`libreadline.a`.

It contains the editing and history operations that `/bin/sh` and the
network tool use.  It is not a complete GNU Readline or BSD libedit
implementation.

- emacs mode (the default, `rl_editing_mode = 1`): the arrows, Home, End,
  Delete, Ctrl-A/B/E/F/N/P, Ctrl-D, Ctrl-K, Ctrl-U, backspace.
- vi mode (`rl_editing_mode = 0`, which `/bin/sh` sets for `set -o vi`):
  the insert and command modes of XCU sh's vi-mode, with counts, the
  motions `h l 0 ^ $ w W b B e E`, `i a I A x X s S C D r ~`, the operators
  `d c y` with a motion (`dd cc yy`), `p P`, `u`, the history keys
  `k - j + G`, and `#`.  History search (`/ ? n N`), `f F t T ; ,`, `.`,
  `v` and completion are not implemented.

Redrawing uses the small ANSI/VT100 baseline provided by zedBSD's console
TTY (`CSI n C` and `CSI n D`, and spaces over a shortened line).  No termcap
or terminfo database is required.
