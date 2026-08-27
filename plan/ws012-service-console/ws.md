# WS012: service administration console

Last updated: 2026-08-28

WSID: `ws012`

Status: Active; `ws012-p003` complete in `q018`, p004 in progress, p005-p006 pending in dependency order

Parent: [master plan](../master.md)

Last verified Phase: `ws012-p003`

Resume point: continue q018 with the in-progress `ws012-p004` argv and
persistent-policy Phase, followed by p005-p006. The p003 ZSV1 service and
system-action protocol, root-only socket, typed clients, and lifecycle/error
behavior are verified; fd 3 remains readiness notification from opted-in
services only.

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

Implement a coherent administration interface over the existing native init.
The prior `/run/init.sock` implementation is evidence, not a fixed product
contract. Planning does not authorize code changes; implementation begins only
when the dependency-ready Phase crosses an approved Queue boundary.

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
| `ws012-p001` | [Service-console design discussion](phase001-design-discussion/phase.md) | Complete | YAML, ZSV1, grammar, policy, permissions, and concurrency decisions accepted |
| `ws012-p002` | [YAML rc.conf model and persistence foundation](phase002-yaml-rcconf/phase.md) | Complete (`q017`) | Strict model, parser, canonical writer, stable lock, atomic replacement, and all-reader migration |
| `ws012-p003` | [ZSV1 init service-control protocol](phase003-zsv1-init-protocol/phase.md) | Complete (`q018`, 2026-08-28) | Replace the old socket grammar with bounded versioned service/system-action records and typed service/shutdown clients |
| `ws012-p004` | [Non-interactive service CLI and persistent policy](phase004-service-argv-persistence/phase.md) | In progress (`q018`) | Stable argv grammar/output, runtime controls, immediate locked enable/disable, and reload |
| `ws012-p005` | [Interactive service console](phase005-interactive-console/phase.md) | Pending (`q018`); depends on p004 | Argument-free prompt reuses the argv dispatcher with no candidate/save state |
| `ws012-p006` | [Service-console integration acceptance](phase006-integration-acceptance/phase.md) | Pending (`q018`); depends on p002-p005 | Host/QEMU lifecycle, concurrency, persistence, failure, cold boot, and documentation evidence |

The implementation dependency chain is p002 -> p003 -> p004 -> p005 -> p006.
q017 completed p002. [q018](../queue.md) is executing p003-p006 serially in
that order; p003 is complete and p004 is the current item.

## Latest completed evidence

q018 p003 passed the production-shared ZSV1 protocol, client, server, and
shutdown-argv fixtures plus protocol/client/server ASan/UBSan runs and
`make -j16`. One disposable amd64 QEMU boot verified the root-owned mode-`0600`
socket, typed state and lifecycle operations, synchronous reload, error
recovery, acknowledged halt, and a clean fatal-log scan. See
[the production QEMU evidence](tests/q018-p003-qemu-evidence.md).

The saved `config.mk` hash remained
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6` and
`git diff --check` passed. Neither `make check` nor `.internal/` was used. q017
remains the predecessor evidence for strict YAML persistence and the rebooted
disabled-policy proof.

## Accepted service-control decisions

- fd 3 is only the inherited readiness channel by which opted-in services such
  as networkd report `READY` or `FAIL` after work such as DHCP completion. It
  is not a general client-to-init control channel.
- `/run/init.sock` is accepted as the root-only PID 1 service-control channel.
  ZSV1 replaces the current grammar; no unversioned compatibility protocol is
  retained.
- ZSV1 contains service LIST/SHOW/START/STOP/RESTART/RELOAD and the fixed
  HALT/POWEROFF/REBOOT system actions. `/sbin/halt`, `/sbin/poweroff`,
  `/sbin/reboot`, and `/sbin/shutdown` migrate to the typed ZSV1 client. No new
  signal-control path is implemented in this iteration.
- The initial supervised service model remains the current one: daemon and
  respawn commands stay in the foreground as direct PID 1 children, allowing
  authoritative `waitpid()` state and restart handling. Traditional
  daemonizing/forking services are future design work.

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

## Fixed YAML v1 shape

The fixed v1 schema keeps host settings at top level and service policy in
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

It uses a strict two-space, mapping-only subset with bounded scalar values.
Generic service metadata remains in `/etc/service.d/`; only enablement and
service-specific rc settings belong here. Exact parsing, canonicalization, and
locking rules are in p002; the accepted `ZSV1` newline-delimited records are
in p003.

## WS completion conditions

WS012 is complete when p002-p006 demonstrate that the installed rc.conf has
one strict YAML representation; concurrent persistent writers cannot lose an
update or publish an invalid file; PID 1 exposes coherent bounded ZSV1 state;
argv and interactive clients share one dispatcher; runtime operations remain
separate from immediate persistent enablement; root-only authorization,
failure, reload, and reboot behavior pass in the production amd64 image; and
the public reference matches the verified implementation.

The WS may pause after any completed Phase when its actual boundary and next
dependency-ready Phase are recorded here and in the master. Container-specific
status remains a future WS013 integration and is not a completion dependency.

## Reconsideration boundaries

Return the affected Phase `uncleared` rather than weakening the contract if
the filesystem cannot provide stable-lock plus atomic-replace persistence,
PID 1 cannot expose a coherent bounded snapshot, or container details would
leak into the generic service protocol.
