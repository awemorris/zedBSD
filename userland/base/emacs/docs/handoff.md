# Handoff: the Emacs-package compatibility push

Written 2026-08-10, at the end of the session that built the
compatibility instrumentation and the first three layers of the Elisp
runtime. This is the state of things and what the next person should do
with it — the reasoning behind each piece lives in `docs/design.md`,
which was kept current as the work went; this file is the map.

**Nothing in either repository is committed.** That is deliberate: the
owner commits by hand. `git status` in `~/remacs` shows the working set
described below. The VM fixes are the exception — they are already
committed in `~/noct-remacs` as `3dd7e87 Fix GC bugs`.

## 1. What the goal became

The starting brief was six items: port more terminal Emacs core, keep
the full/small bundle split, allow a custom build from a feature file,
raise compatibility so GNU Emacs packages run unmodified, and find the
incompatibilities by porting real packages.

Item four of that list — *Elisp execution compatibility* — was an
explicit **non-goal** in `docs/design.md` §1 when the session started,
and the section still says so. **Someone should decide whether to
rewrite it.** Everything below assumes the goal changed; the document
has not caught up, and that is the one piece of drift left behind.

Two policy calls were made along the way and should be treated as
settled:

- **Clean-room is maintained.** GNU Emacs source is not read. Its `.el`
  files are used as *scanner input* (running a program is black-box
  observation, which §1 already permits) and its runtime is *queried*
  for interfaces (`func-arity`, `documentation`, `subrp`). No
  implementation is consulted.
- **If Emacs implements it in C, remacs treats it as core** — the
  `napi.def` layer. If Emacs implements it in Lisp, remacs writes Lisp.
  This removes the judgement call from every individual function.

## 2. What was built

### The measuring instruments (`tools/`)

Three programs turn "does this package work?" into a ranked work list.
None of them existed before.

```
tools/compat-report.sh <corpus>/*.el   # scan   -> what each file is missing
tools/missing.py <report-dir>          # gather -> classify -> buckets
tools/classify.el                      #   (asks a running GNU Emacs)
```

- **`remacs --compat FILE`** (`editor/lispcompat.noct`) loads an Elisp
  file with every "this does not exist" recorded instead of fatal, so
  one run names every gap rather than the first. Two passes: evaluation
  of top-level forms, then a static walk of the form tree for the defun
  bodies that never ran. Details and the false-positive rules are in
  `design.md` §9.1.
- **`tools/classify.el`** asks a running Emacs what each name *is*:
  special form, macro, C primitive, or Lisp — plus arity, argument list,
  docstring, and for a C primitive the C source *file name*, which gives
  the subsystem. `subrp` alone is not the C test; Emacs 30
  native-compiles Lisp into subrs, so `subr-native-elisp-p` is what
  separates them. This tripped me up first time round.
- **`tools/missing.py`** joins the two and buckets the result, including
  the platform dimension described in `docs/noct-gaps.md`.

**A scan runs the file.** Top-level forms are evaluated, so a file with
side effects has them — scanning Emacs's own `lisp/` leaves a
`blessmail` script in the working directory. Scan from somewhere you do
not mind being written to.

### The Elisp runtime, three layers

Each layer was implemented, then verified against real GNU Emacs through
the existing three-way oracle harness (`tests/run-lisp.sh` runs each
file on the interpreter, on the compiler-off interpreter, and on Emacs,
and diffs all three). The Lisp oracle went from 4 tests to 8.

1. **`defmacro` + backquote** (`05-macros.el`). The hard part is that
   code and data are different representations here — see `design.md`
   §9.0 for the three rules that avoid a conversion pass that guesses.
   The Lisp compiler expands macros at compile time rather than treating
   a macro call as a function application.
2. **Non-local exit** (`07-nonlocal.el`): `catch`/`throw`,
   `condition-case`, `unwind-protect`, built on the evaluator's existing
   unwind mechanism rather than a second one (`design.md` §9.0.1).
3. **The loader** (`08-loader.el`): `provide`, `require`, `load`,
   `autoload`, `featurep`, `load-path` (`design.md` §9.0.2).

On top of them, `editor/prelude.noct` defines in *Emacs Lisp* what Emacs
also defines in Emacs Lisp: `when`, `unless`, `dolist`, `dotimes`,
`push`, `pop`, `prog1`, `ignore-errors`, `save-excursion`,
`with-current-buffer`, `with-temp-buffer`, `eval-when-compile`, and the
rest. Adding to it costs no Noct.

Measured across 149 local packages, four rounds took the count of
distinct missing special forms from **54 → 42 → 33 → 25**; backquote
went from 1069 uses across 81 files to zero.

### Two things deliberately left out

`save-restriction` (needs narrowing primitives) and `save-match-data`
(needs a match-data accessor). A version that skipped the save would be
worse than an honest gap — it would look like it worked.

## 3. Where the project actually stands

Against the original six:

| | status |
|---|---|
| Port more terminal core | **Not started.** The target list is now data, not guesswork — see §4. |
| full / small bundles | Unchanged, all tests green |
| Core goes to full, `-s` only if needed | Policy only; no decisions have come up yet |
| **Custom build from a feature file** | **Not started.** Cheapest item on the list; see §5. |
| Package compatibility | Three runtime layers done, 54→25 missing forms |
| Find incompatibilities by porting packages | Instruments built and run over 1756 files; **no specific package has been made to work yet** |

## 4. The work list

`tools/missing.py` over the GNU Emacs 30.1 corpus (1654 files) yields
13910 distinct missing names. The buckets:

```
not-in-emacs   5491   the packages' own names — not gaps in remacs at all
elisp          7420   macros and Lisp functions — the prelude, or a library
core            776   C primitives — napi.def
out-of-scope    195   coding systems, faces, images, the window system (§1 non-goals)
review           26   fell outside the known C files; read by hand
evaluator         2   special forms
```

The 5491 dropping out mechanically is what makes the rest readable.

Of the 776 core primitives, **710 are implementable on today's VM** and
66 are blocked on platform support that does not exist. The blocked ones
are concentrated in five groups (time, file-stat, file-manage,
proc-signal, net) and are enumerated with what would unblock them in
**`docs/noct-gaps.md`**.

Ready-now, by subsystem:

```
window.c 105   data.c 83   fns.c 80   editfns.c 49   buffer.c 42
process.c 33   keymap.c 26   keyboard.c 25   eval.c 24   floatfns.c 24
textprop.c 19  minibuf.c 19  syntax.c 18   lread.c 17   fileio.c 16
charset.c 15   chartab.c 13  search.c 12   ...
```

Ranked by how many files want them, the top is `string-match` (650
files), `nreverse` (460), `aref` (419), `looking-at` (415), `mapconcat`
(378), `erase-buffer` (362), `delq` (313), `put` (307). **`search.c` is
the best value per function**: 12 primitives, 1756 file-mentions, and
`editor/commands.noct` already has the Emacs-regexp translation and
match-data plumbing they would sit on.

The full table with arity, argument names and docstrings is regenerated
by `tools/missing.py <report-dir> --tsv out.tsv`; it is not checked in
because it is derived.

## 5. What to do next

**The agreed approach**: rather than picking a subsystem and driving it
to completion, work through what is implementable now while documenting
the Noct support each subsystem turns out to need. `docs/noct-gaps.md`
is that document and should be updated as the work reveals more.

Concretely, in the order I would take them:

1. **`search.c`, 12 primitives.** Highest value per function, no
   platform dependency, and the foundation is already written.
2. **`fns.c` / `data.c`, 163 primitives.** Pure functions over lists,
   strings, symbols and numbers. Mechanical, and the policy call
   ("Emacs C ⇒ remacs core") means they belong in `napi.def` — but note
   `napi.def` currently has 77 rows, and adding 163 is a change in kind.
   **Someone should decide whether the table wants sectioning or a
   second file before it triples.**
3. **`editfns.c` + `buffer.c`, 91 primitives.** Narrowing
   (`narrow-to-region`, `widen`) also unblocks `save-restriction` in the
   prelude.
4. **`textprop.c`, 19 primitives** — text properties are what every
   modern package assumes exist.
5. VM work, when scheduled: `time` first (21 primitives, no design
   questions), then `file-stat` (17 from one call), then `proc-signal`
   (5 from one extra argument, and it makes `shell.noct` and `gud.noct`
   honest about what `C-c C-c` does).

The **custom build** item is still untouched and still cheap: the
mechanism (`gen-napi.py --modules=`, self-registering modules) already
exists, and `tools/build-nap.sh` just needs its hardcoded rosters
replaced by a config file. What makes it *worth* doing is finer module
granularity — the core is most of the 8500 lines — so it pairs naturally
with whatever restructuring the 163 new `napi.def` rows force.

**Nobody has yet taken one real package and made it load.** That is the
end-to-end test the whole instrument was built to serve, and the
compatibility work will feel abstract until it happens. A small
self-contained package with no `defcustom`/`define-derived-mode`
dependency is the right first target.

## 6. The VM work, and one caution

Six VM bugs were found by running this editor and fixed in
`~/noct-remacs` (committed there; `docs/design.md` §13 items 6–10
record five of them — the nested-GC one is folded into item 10):

| | symptom |
|---|---|
| String concat truncated at 1023 bytes, silently | pasting one long line lost its tail |
| Ranged-for with start > end never terminated | `for (i in 1..n)` over an empty collection hung |
| `String.charAt` counted from the front every call | the Lisp reader was O(n²); a 113 KB file took 96 s |
| Tenure allocator scanned the whole heap per allocation | O(n²) on any large load |
| GC walked the heap by C recursion | deep cons chains overflowed the C stack |
| **A GC could start inside a GC** | rare string corruption; the worst of the six |

That last one is worth reading about before touching the collector: the
allocator's out-of-memory retry chain called `rt_gc_old_gc()` from
*inside* a running young collection, and the old GC's initialization
clears the `is_marked` bookkeeping the young collection is standing on.
It reproduced once in several hundred collections. The fix is in
`gc.c`; a debug verifier (`NOCT_GC_VERIFY=1`) that walks the whole heap
after each young GC and aborts on any surviving pointer into the cleared
region is what found it, and is left in place.

Result: of 1654 GNU Emacs Lisp files, **47 that previously could not be
scanned at all now scan**, including `dired.el`, `files.el`,
`font-lock.el` and a 4 MB dictionary that used to run forever.

**Caution.** Other agents work in other Noct checkouts. VM changes for
this project go in **`~/noct-remacs` only** — not `~/NoctLang`, not
`~/noct-gc`. (`~/NoctLang` has a stray `build-static/` directory I
created early on before that rule existed; harmless, but not mine to
delete now.)

## 7. Loose ends

- **`docs/design.md` §1 still lists Elisp execution compatibility as a
  non-goal.** The rest of the document describes a project that treats
  it as a goal. This needs a decision, then an edit.
- **Three pty tests depend on fixtures that no longer exist**
  (`case_compile.py` wants `/tmp/comptest.c`, `case_gud.py` wants
  `/tmp/gudtest` plus gdb, `case_skk_okuri_real.py` wants
  `~/SKK-JISYO.L`). The third also fails to clean up, so a second run
  aborts the whole suite under `set -eu`. A task chip was raised for
  this; it was never picked up. Everything else is green: unit 7, oracle
  15, Lisp 8, pty 53, both bundles, storyfuzz 40 runs.
- **`~/.emacs.d` was converted to UTF-8.** 38 files were ISO-2022-JP;
  they and their coding cookies were rewritten (three via GNU Emacs
  itself, whose codec handles JIS X 0201/0212 where Python's does not).
  A backup tarball was left in the session scratchpad, which will not
  survive. Verified loadable from GNU Emacs afterwards.
- **`nil` prints as `0`.** remacs represents nil as the integer 0, so
  `(princ nil)` cannot say "nil" without making `(princ 0)` lie. The
  Lisp oracle tests work around it by testing truth rather than
  printing. It is structural, not a bug, and worth knowing before
  someone tries to "fix" it.
