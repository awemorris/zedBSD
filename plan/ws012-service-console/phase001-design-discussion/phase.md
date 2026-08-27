# WS012 Phase 001: service-console design discussion

Last updated: 2026-08-27

WSID: `ws012`

Phase ID: `p001`

Combined ID: `ws012-p001`

Status: Proposed

Parent: [WS012](../ws.md)

Reviews: [WS012 review index](../tests/README.md)

## Objective

Resolve the public behavior of the argv and interactive `/sbin/service`
interfaces before any implementation Phase is selected.

## Baseline

The current command already supports
`list|reload|status|start|stop|restart|enable|disable`, communicates through
`/run/init.sock`, and atomically edits exact `NAME_enable` assignments.
Argument-free invocation currently prints usage instead of entering a console.

## Scope

- prompt and command grammar;
- `show` output and whether dependency closure is a separate command;
- immediate runtime actions versus immediate persistent policy changes;
- YAML `/etc/rc.conf`, reload, atomic rewrite, and file-lock behavior;
- root permissions and socket authorization;
- versioned records returned by PID 1;
- error, timeout, daemon-exit, and partial-output behavior;
- decomposition into later implementation and verification Phases.

## Non-goals

- modifying source code or installed configuration;
- designing container isolation internals;
- changing init supervision, service dependency, or no-runlevel decisions.

## Open decisions

- Review and accept or revise the concrete YAML `/etc/rc.conf` proposal below.
- Review and accept or revise the concrete `ZSV1` init protocol proposal
  below. No binary framing or general serialization library is proposed.

## Fixed decisions

- `enable NAME` and `disable NAME` immediately and atomically update YAML
  `/etc/rc.conf`, acquire the common stable file lock, and request PID 1 policy
  reload. Interactive and argv behavior are identical.
- No `save`, `discard`, or `commit` command is included initially; there is no
  service-policy candidate state or stale-candidate generation.
- `start`/`stop` affect the current supervised instance only. Enablement is the
  persistent boot/restart policy only; neither operation implies the other.
- The list form shows concise `NAME`, runtime `STATUS`, persistent `ENABLED`,
  and `PID` columns without dependencies. `show NAME` may show its detailed
  direct `after`/`requires` relationships, command/type/restart policy, runtime
  state, enablement, and PID. It does not calculate a transitive closure.
- Multiple service consoles coordinate persistent changes only through a
  stable companion file lock and atomic replacement. PID 1 serializes runtime
  control requests.
- oneshot/daemon/respawn and stopped/starting/running/completed/failed/skipped
  semantics are an init/service-definition contract. The CLI displays the
  state supplied by PID 1 but does not redefine that lifecycle here.
- There is no compatibility migration or dual parser for current `key=value`
  rc.conf data. The installed default, init parser, and service writer change
  together because there are no existing users to migrate.

## Proposed minimal YAML `/etc/rc.conf`

```yaml
version: 1
hostname: zedbsd

services:
  syslogd:
    enabled: true
  getty_console:
    enabled: true
  networkd:
    enabled: true
  net:
    enabled: true
  cron:
    enabled: true
  ntpdate:
    enabled: false
    settings:
      servers: ""
```

`services.NAME.enabled` is the only generic persistent service policy.
Service-specific rc.conf options are scalar keys under
`services.NAME.settings`; their names and meanings are defined by that
service, not by the service CLI. Service process type, command, dependencies,
and restart policy remain in `/etc/service.d/NAME`, not in rc.conf.

The initial parser is a deliberately small mapping-only YAML subset:

- exactly two spaces per indentation level; tabs are rejected;
- mappings, whole-line comments, bounded keys, quoted strings, `true`,
  `false`, and bounded unsigned integers only;
- no sequences, flow collections, anchors, aliases, tags, multiple documents,
  multiline scalars, implicit dates/numbers, or duplicate keys;
- unknown top-level, service-policy, and service-setting keys fail validation;
- the canonical writer emits `version`, host keys, and `services`, retains a
  deterministic service order, and writes through the common lock plus
  same-directory temporary/sync/rename path.

This proposal intentionally resembles YAML without requiring a general YAML
implementation. The design review must decide whether `settings` is the right
single container for all service-specific options; no migration question
remains.

## Proposed init socket protocol (`ZSV1`)

“Machine-readable framing” means that `/sbin/service` must not scrape the
English text PID 1 currently prints. The initial replacement can remain a
small newline-delimited text protocol:

```text
ZSV1 LIST

ZSV1 SERVICE networkd running 1 184
ZSV1 SERVICE sshd running 1 231
ZSV1 END
```

For one service and one mutation:

```text
ZSV1 SHOW sshd
ZSV1 SERVICE sshd running 1 231
ZSV1 AFTER networkd
ZSV1 REQUIRES networkd
ZSV1 END

ZSV1 STOP sshd
ZSV1 OK stopped
ZSV1 END
```

Fields are fixed tokens: protocol marker, record type, validated service name,
lifecycle-state enum, enabled boolean (`0`/`1`), and decimal PID (`0` when no
process exists). Service names and enum tokens cannot contain whitespace, so
v1 requires no quoting or escaping. Human prose is generated by
`/sbin/service`, not transported as protocol data. Command/path display may be
read from the same validated `/etc/service.d/` definition rather than adding
unbounded strings to v1.

Framing is one bounded line per record plus a mandatory `ZSV1 END`; EOF before
`END`, an overlong line, an unknown record, or extra fields is a protocol
error. Versioning is simply the literal `ZSV1`. PID 1 rejects another version
explicitly rather than silently interpreting it. A future incompatible format
can use `ZSV2`; no negotiation handshake is needed for a local base-system
client and server upgraded together.

The implementation Phase should reuse the current 256-byte request bound, set
a fixed response-line bound, cap records by the init service-table limit, and
apply a bounded client timeout. This is enough to make list/status/control
stable without JSON, binary messages, or display-text parsing.

## Work packages

- [ ] Resolve each open decision with examples and failure behavior.
- [ ] Freeze the public argv and interactive grammar.
- [ ] Freeze runtime versus immediate persistent-policy semantics.
- [ ] Review and freeze the proposed YAML rc.conf schema and common lock
      contract; no migration path is required.
- [ ] Review and freeze the proposed `ZSV1` init control protocol.
- [ ] Define permissions, concurrency, and atomic-rewrite requirements.
- [ ] Write completion conditions and split later implementation Phases.
- [ ] Synchronize WS012, the master, and WS009 handoffs.

## Acceptance

The design review cases in the [WS012 review index](../tests/README.md) must all
have an explicit resolution. No build or QEMU result is claimed by this Phase.

## Actual results and evidence

The initial repository audit confirms that the argv command and PID 1 control
path exist, but responses are human-readable, argument-free invocation has no
console, and rc.conf still uses `key=value`. Concrete minimal YAML and `ZSV1`
proposals now make the two remaining review decisions explicit.

## Interruption / resumption

Resume at the first unresolved decision above. Do not place an implementation
Phase in a Queue until p001 records its command and state contracts.

## Remaining debt and handoff

All implementation, automated tests, QEMU integration, public documentation,
and WS013 container-status integration remain later Phases.
