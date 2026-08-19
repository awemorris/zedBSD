# libedit

This is zedBSD's small, clean-room line editor.  The package is named
`libedit`, but it exposes the Readline-compatible headers
`<readline/readline.h>` and `<readline/history.h>` and builds as
`libreadline.a`.

The initial implementation intentionally contains only the editing and
history operations used by `/bin/sh`.  It is not yet a complete GNU Readline
or BSD libedit implementation.

Redrawing uses the small ANSI/VT100 baseline provided by zedBSD's console
TTY (`CSI 2 K` and `CSI n D`).  No termcap or terminfo database is required.
