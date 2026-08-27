# WS012: service administration console

Last updated: 2026-08-27

WSID: `ws012`

Status: Proposed; design discussion only

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: review the concrete minimal YAML `/etc/rc.conf` and `ZSV1`
line-protocol proposals in `ws012-p001`; migration and candidate/save semantics
are no longer part of the initial service console.

Shared reviews: [WS012 review index](tests/README.md)

## Goals

- Preserve script-safe `/sbin/service COMMAND [NAME]` operation.
- Make argument-free `/sbin/service` enter a native interactive console.
- Separate runtime start/stop state from persistent enable/disable policy.
- Keep PID 1 authoritative for process state and `/etc/rc.conf` authoritative
  for persistent service policy.
- Convert `/etc/rc.conf` to a documented YAML configuration containing service
  enablement and the remaining host/service settings.

## Objective

Design a coherent administration interface over the existing native init and
`/run/init.sock` baseline. The first and only current Phase is discussion and
specification; it does not authorize code changes.

## Scope

- interactive command grammar, help, prompts, and tabular output;
- immediate atomic enable/disable persistence and file-lock behavior;
- YAML `/etc/rc.conf` schema, parser, canonical writer, and direct replacement
  of the unused `key=value` format;
- a bounded, versioned init control protocol suitable for both argv and
  interactive clients;
- future integration points for container-backed services from WS013.

## Non-goals

- changing zedBSD's single native init or adding runlevels;
- making enable/disable implicitly start or stop a service;
- implementing the WS013 container runtime in this WS;
- SysV, systemd, or another vendor CLI compatibility claim.

## Dependencies

- [WS002](../ws002-services/ws.md) supplies init, service definitions,
  supervision, `/etc/rc.conf`, and the current argv command.
- [WS013](../ws013-containers/ws.md) may later add container-specific status
  fields without changing the generic service lifecycle.
- WS009 owns final public administration documentation.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws012-p001` | [Service-console design discussion](phase001-design-discussion/phase.md) | Proposed | Resolve the public grammar and state/protocol semantics, then extract bounded implementation Phases |

No implementation Phase is defined until `ws012-p001` is complete.

## Confirmed inputs

- `service start sshd`, `stop`, `restart`, `status`, `enable`, and `disable`
  remain valid non-interactive forms.
- `service` with no arguments enters `service>` and `?` prints help.
- `show` provides name, lifecycle state, enabled policy, and PID.
- Traditional non-container services remain first-class.
- `enable` and `disable` immediately update persistent policy and reload PID 1;
  there is no initial service-policy candidate, `save`, `discard`, or `commit`.
- The service list is concise and does not include dependencies. `show NAME`
  may include detailed `after` and `requires` relationships.
- Concurrent persistent edits use file locking plus atomic replacement. Because
  rename changes the inode, every writer must lock one stable companion lock
  file rather than relying on a lock held only on the old `rc.conf` inode.
- Service type and lifecycle definitions remain owned by init/service.d design,
  not by the service-console grammar Phase.
- Current `key=value` rc.conf compatibility is intentionally omitted. The
  installed configuration and all base-system readers/writers switch together.

## Current YAML proposal

The proposed v1 schema keeps host settings at top level and service policy in
one mapping:

```yaml
version: 1
hostname: zedbsd
services:
  syslogd:
    enabled: true
  ntpdate:
    enabled: false
    settings:
      servers: ""
```

It uses a strict two-space, mapping-only subset with scalar values. Generic
service metadata remains in `/etc/service.d/`; only enablement and
service-specific rc settings belong here. The exact subset and proposed
`ZSV1` newline-delimited init records are specified for review in p001.

## WS completion direction

The current planning-stage WS may pause after p001 has produced fixed decisions
and a later Phase map. Implementation completion conditions are deliberately
not invented before that discussion closes.

## Reconsideration boundaries

Stop the design discussion before committing to implementation if atomic
policy persistence conflicts with `/etc/rc.conf`, PID 1 cannot expose a stable
snapshot, or container details would leak into the generic service protocol.
