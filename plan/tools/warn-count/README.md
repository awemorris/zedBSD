# warn-count: the warnings of the userland packages

Counts, per package, the compiler warnings that `-Wall -Wextra` would give the
programs and shared libraries built through `userland/base/package.mk`
(`STANDALONE_PROGRAM_CFLAGS`, `STANDALONE_SHLIB_CFLAGS`).  Static libraries
(`STANDALONE_LIB_CFLAGS`) already compile with `-Wall -Wextra -Werror`.

Files:

- `measure.patch`: a measurement-only change to `userland/base/package.mk`.
  It adds `$(WARN_EXTRA_CFLAGS)` (default `-Wall -Wextra`, no `-Werror`, so
  every package still builds) to the two flag sets, and puts
  `$(WARN_CC_WRAP)` (default empty) in front of the compiler in the object
  rules.  Never commit it; revert it after the run.
- `cc-log.sh`: the compiler wrapper.  It runs the compiler and keeps its
  diagnostics in `$WARN_LOG_DIR`, one file per object, named after the
  object's path under `build/<platform>/packages/` with `/` turned into `%`
  (the first part is the package).
- `count.py`: reads such a directory and prints Markdown tables: per package
  (warnings, distinct source locations, objects), per `-W` flag, and the
  packages without warnings.
- `makefile.diff`: the proposed change (`-Wall -Wextra -Werror` on both flag
  sets).  A proposal for the user; not applied.
- `results.md`: the counts of the last run and the migration plan.

Run (in an agent's worktree whose `build/` has the toolchain; 4 jobs):

```sh
git apply plan/tools/warn-count/measure.patch
mkdir -p build/warn/x86_64
export WARN_LOG_DIR=$PWD/build/warn/x86_64/obj
make -j4 CONFIGMK=config/config.x86_64.mk sysroot
make -j4 -k CONFIGMK=config/config.x86_64.mk userland \
    WARN_CC_WRAP=$PWD/plan/tools/warn-count/cc-log.sh \
    < /dev/null > build/warn/x86_64/build.log 2>&1
/usr/bin/python3 -I plan/tools/warn-count/count.py build/warn/x86_64/obj $PWD
git apply -R plan/tools/warn-count/measure.patch
```

Only objects that are compiled during the run are logged: start from a build
directory without the packages (or one whose flags change, which the measure
patch does, since the objects follow their flags).  The firmware packages stop
at their license prompt when stdin is not a terminal; they have no C sources.
