# q018 `ws012-p006` integration evidence

Date: 2026-08-28 JST

Authoritative matrix: [q018-p006-results.tsv](q018-p006-results.tsv)

## Commands and artifacts

From the repository root, the consolidated host suite was run outside the
restricted sandbox because its AF_UNIX client/server cases receive `EPERM`
inside that sandbox:

```sh
./plan/ws012-service-console/tests/host-service-acceptance.sh \
  /tmp/ws012-p006-host-final-004
```

All eight production-linked fixtures passed both strict C17 and ASan/UBSan,
for 16 passing cells. The generated `results.tsv` and individual compile/run
logs remain in that output directory. The runner records every expanded
compiler command before executing it.

The production image was built and exercised with:

```sh
make -j16
./plan/ws012-service-console/tests/qemu-service-acceptance.sh \
  /tmp/ws012-p006-qemu-final-004
```

The QEMU runner used QEMU 10.0.11 with two writable copies of
`build/amd64/hdd-image.img`. Its main cell booted, mutated service state,
rebooted the same copy through ZSV1, verified persistent state, requested
halt, and reached PID 1's final-action checkpoint. The second clean cell
requested poweroff and reached the same checkpoint. Both controller and QEMU
statuses were zero.

## Production results

- Initial YAML boot applied hostname `zedbsd`, started the enabled dependency
  set, exposed disabled `ntpdate`, and reported one coherent sorted LIST/SHOW
  view. `/run/init.sock` was a root-owned socket with mode `0600`
  (`mode=c180`), and the image-installed `/etc/rc.conf` was a root-owned
  regular file with mode `0644` (`mode=81a4`).
- Stop/start/restart changed the cron runtime state without changing the
  `rc.conf` checksum.
- Argument-free `service` remained at the `service>` prompt through `?`,
  `help`, a blank line, list/show/status, every lifecycle and policy command,
  reload, a typed unknown-service error, and a following successful command.
  `quit` returned one usable shell prompt. No `[PID] stopped` result occurred.
- Disable changed persistent policy while cron remained running. A ZSV1
  reboot then produced exactly two `init: system running` records, and cron
  booted stopped and disabled. Enable changed only policy; a later explicit
  start produced running and enabled state.
- Replacing `rc.conf` with invalid assignment-format content made reload fail
  with a typed error, preserved the last valid in-memory policy, and a restore
  plus reload succeeded.
- Reboot, halt, and poweroff each reached the new
  `init: executing system action ACTION` checkpoint after service teardown.
  This proves the control acknowledgement was followed by PID 1's final
  action boundary rather than stopping validation at the first shutdown log.
- Guest and QEMU/HMP fatal scans were all empty. The production image and
  `config.mk` hashes were unchanged after both cells.

The input hashes before and after acceptance were:

```text
build/amd64/hdd-image.img  53ae5461791db11c69a9ca83cfa092b9ddaf15afbf60e9f2513ae9de8806cc41
config.mk                  3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6
```

## Integration defects repaired

1. Registered base-system data inherited group-write bits from source files.
   Root-image assembly now assigns deterministic mode `0644` to every
   registered data payload while retaining explicit account and executable
   overrides.
2. The shell used `posix_spawn()` for a foreground external, but that call
   returned only after exec. A terminal-reading child could therefore enter a
   new background process group and receive `SIGTTIN` before the parent called
   `tcsetpgrp()`. A close-on-exec pre-exec gate now establishes the process
   group and foreground terminal ownership before release. Gate release
   retries `EINTR`, suppresses only its local `SIGPIPE`, and restores the prior
   disposition. QEMU directly proved the original `service` failure fixed.
3. Shutdown acceptance could previously observe only `init: stopping
   services` and terminate QEMU before the final ioctl boundary. PID 1 now
   emits and flushes an action-specific final-handoff marker; the runner waits
   for and validates it.

The same handoff ordering still needs focused work for foreground pipelines,
and `fg` currently resumes before foregrounding. These unrelated general shell
job-control residuals are not hidden: [ws001-p014](../../ws001-posix/phase014-shell-job-control/phase.md)
owns their implementation and regression matrix.

## Other gates

The modified C files passed clang-format 19.1.7. `git diff --check` passed, and
DOC-T00 reported `Markdown relative-link check: PASS (744 links)` after final
book synchronization. `make check` was not run and `.internal/` was not used.
