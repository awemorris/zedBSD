# WS023 test notes

The final WS023 gate combines mechanical source checks with manual semantic
review and existing behavior tests. The mechanical audit is intentionally
limited to rules that can be decided without interpreting C behavior.

Run it from the repository root after the host Noct toolchain exists:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws023-x86-hal-style/tests/x86-hal-style-audit.noct "$PWD"
```

The audit checks the fixed 88-file inventory count, exact file envelope, and
absence of ordinary line comments, `goto`, declarations in `for`
initializers, and collapsed function/control bodies. It does not claim that a
comment is meaningful, that declarations and definitions are ordered
correctly, or that a rewrite preserves behavior. Those remain mandatory
manual Phase review items.

Behavior verification reuses the focused test owners named by each Phase.
Final configured builds use distinct build roots for PC/AT, PC-98, generic
amd64, and Intel Mac so architecture-root artifacts cannot race.
