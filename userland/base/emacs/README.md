REmacs: Re-implemented Editing Macros
=====================================

A re-implementation of GNU Emacs on the Noct JIT VM.

The editor core is written entirely in Noct. Every OS primitive it
needs is a standard or non-standard Noct API (Term, Process, Regex,
File, System).

```
REmacs 0.1

Copyright (C) 2026 Awe Morris

REmacs comes with ABSOLUTELY NO WARRANTY.
It is distributed under the zlib license; see the file named LICENSE.
```

## Running

The whole editor ships as a single bytecode file that an **unmodified**
`noct` runs. `make` builds two configurations:

```
make
noct build/remacs.nap [file]      # full: everything
noct build/remacs-s.nap [file]    # small: for embedded use
```

The **full** bundle carries every module; the **small** bundle keeps
the core editor plus isearch and query-replace, and leaves out SKK,
shell, gud and compilation mode. The rosters live in
`tools/build-nap.sh`; a module is a self-registering unit (its
`<name>Install(ed)` adds its commands, key bindings and hooks), so the
core never names a module and a bundle simply omits the sources it
does not want.

## Install

```
sudo make install     # -> /usr/bin/remacs + /usr/share/remacs/noct/remacs.nap (+ remacs-s.nap)
remacs [file]
```

`make install` detects the OS family (Debian, RedHat, FreeBSD, macOS)
to choose the resource prefix; `make showconfig` prints the chosen
paths. `/usr/bin/remacs` is a small shell script that runs
`noct /usr/share/remacs/noct/remacs.nap`.

## Startup files

- `~/.remacs.noct` — Noct source; must define `remacsInit(ed)`.
- `~/.remacs.el`   — Emacs Lisp, evaluated by the built-in interpreter.

## Development

A C launcher builds against the parent NoctLang tree for the test
suite:

```
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug .
cmake --build build-debug -j
make test        # or: cd tests && sh run-all.sh
```

## Layout

- `editor/*.noct`  — the editor, in Noct
- `src/napi.def`   — the Editor.* API table (single source of truth)
- `tools/`         — gen-napi.py (bridge/table generator), build-nap.sh
- `tests/`         — unit, pty, GNU Emacs oracle, and Lisp oracle tests

## Elisp compatibility

To find out what an Emacs Lisp file needs that remacs does not have:

```
tools/compat-report.sh path/to/package.el
```

## In zedBSD

This tree is REmacs as of commit 1a72439 (github.com/awemorris/remacs),
taken into zedBSD as userland/base/emacs (2026-10-04, ws129).  Its author
licenses it under the zlib license (SPDX-License-Identifier: Zlib).  The
zedBSD package (`emacs`) builds `remacs.nap` from it with the host's Noct and
installs it as the command `/bin/emacs` (2026-10-04, ws129-p012), with the
bundled dictionaries at `/usr/share/remacs/skkjisyo.dic` (compact) and
`/usr/share/remacs/skkjisyo-x.dic` (`SKK-JISYO.X`, looked in after the
compact one; 2026-10-14, BUG-302).  The application
starts with `#!/usr/bin/noct`, so the kernel runs it with noct; the package
links `/usr/bin/noct` to `/bin/noct` (`Makefile`; REmacs's own build is
`Makefile.remacs`, which `make -f Makefile.remacs` runs as before).
