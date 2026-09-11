# WS012 Phase 005: interactive service console

Last updated: 2026-08-28

WSID: `ws012`

Phase ID: `p005`

Combined ID: `ws012-p005`

Status: Completed (`q018`, 2026-08-28)

Parent: [WS012](../ws.md)

Tests: [WS012 test index](../tests/README.md)

## Objective

Make argument-free `/sbin/service` enter the native `service>` console while
reusing the p004 command dispatcher, persistence rules, typed status model,
output, and exit behavior rather than creating a second administration path.

## Dependencies

- `ws012-p004` supplies the complete non-interactive command and formatting
  implementation.

## Console contract

Startup and the first prompt are:

```text
zedBSD Service Console
Type '?' for help.
service> 
```

The accepted commands are:

```text
show [NAME]
list
status NAME
start NAME
stop NAME
restart NAME
enable NAME
disable NAME
reload
help
?
exit
quit
```

- `show` and `list` display the p004 concise table; `show NAME` and
  `status NAME` display the p004 detail view.
- Every lifecycle or persistent command has exactly the same transition,
  authorization, diagnostics, and backend path as its argv equivalent.
- Enable/disable take effect immediately. There is no `save`, `commit`,
  candidate, dirty marker, or deferred state.
- A failed command prints its diagnostic and returns to `service>` without
  terminating the session. EOF, `exit`, or `quit` exits successfully.
- Input is one bounded line. An overlong line is consumed and rejected as one
  command; blank lines simply redisplay the prompt. Whitespace separates
  fields and there is no shell quoting, expansion, pipeline, or command
  execution.
- The initial console uses ordinary terminal input/output. Completion,
  history, and libedit integration are non-goals.
- Effective UID 0 is required before entering the console. The stable rc.conf
  lock, not a console-global lock, serializes persistent changes with other
  interactive and argv processes.

## Work packages

1. Refactor p004 only as needed so argv and console invoke the same typed
   dispatcher and formatter.
2. Add the banner, prompt loop, bounded tokenizer, `?`/help, exit handling,
   and recovery after command errors.
3. Ensure concurrent consoles observe fresh ZSV1 state for every command and
   re-read rc.conf under its lock for every persistent mutation.
4. Add pseudo-terminal or stream-driven host fixtures for every command,
   repeated commands, blank/overlong/malformed input, EOF, backend failure,
   and two-console enable/disable contention.

## Completion conditions

- argument-free invocation enters the documented prompt and all commands are
  reachable without an argv-only behavior difference;
- `?` gives concise usable help and no unsupported save/commit vocabulary is
  advertised;
- command failures preserve the session and cannot leave an in-process
  candidate or uncommitted policy;
- concurrent console and argv writers retain each other's policy changes;
- focused console tests and `make -j16` pass, and `git diff --check` passes
  without `make check` or `.internal/` use.

## Completion record (`q018`)

- Argument-free `/sbin/service` now enters the stream-injectable console while
  argv invocations continue through the p004 dispatcher. Effective UID 0 is
  checked before the exact banner and `service> ` prompt are emitted.
- The line reader accepts at most 511 bytes, rejects a 512-byte-or-longer line
  as one unit after consuming it completely, and then resumes at the next
  command. Space/tab tokenization accepts at most 16 fields and deliberately
  performs no shell quoting, expansion, pipelines, or command execution.
- `?` and `help` print the fixed command list without save/commit vocabulary;
  EOF, `exit`, and `quit` succeed. Blank input, local-command arity failures,
  dispatcher grammar/protocol failures, malformed/control-character input,
  and backend failure all recover to a fresh prompt and fresh backend request.
- Every administrative command invokes the same p004 dispatcher and formatter.
  There is no console-global candidate or lock: each persistent command reloads
  current rc.conf under its stable lock, so the p004 console/argv writer proof
  remains authoritative for concurrency.
- Review exposed ignored output-stream failures in help and local diagnostics,
  which could let the loop continue or report success after output was lost.
  Help now returns a status and every local write failure terminates the console
  with failure.
- The production console fixture passed strict C17, ASan/UBSan, and 20/20
  repeated runs; the p004 dispatcher regression also passed. Repository-wide
  `make -j16`, formatting, and `git diff --check` passed. The saved `config.mk`
  SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
  `make check` was not run and `.internal/` was not used.

## Reconsideration boundary

Stop before adding line editing or a general command language if the bounded
line loop is insufficient. Those are separate product choices, not reasons to
diverge from the p004 dispatcher.

## Interruption / resumption

Completed without interruption in q018. Continue with `ws012-p006`, which owns
the consolidated host regression, production amd64 QEMU behavior, persistence
and malformed-reload acceptance, fatal-log scan, and public documentation.
