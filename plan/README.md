# zedBSD plans

The authoritative program plan is [master.md](master.md). Planning documents
are organized by permanent workstream ID (WSID):

```text
plan/
  master.md
  governance.md
  wsXXX-name/
    ws.md
    phaseXXX-name/
      phase.md
    tests/
      README.md
```

Use `wsXXX-pYYY` when referring to a Phase unambiguously across the project.
For example, `ws001-p009` is WS001 Phase 009.

## Workstream index

| WSID | Workstream | Plan |
| --- | --- | --- |
| `ws001` | POSIX.1-2024 compliance | [WS001](ws001-posix/ws.md) |
| `ws002` | System services | [WS002](ws002-services/ws.md) |
| `ws003` | Dell Latitude 5320 bring-up | [WS003](ws003-bringup/ws.md) |
| `ws004` | Hardware expansion | [WS004](ws004-hardware/ws.md) |
| `ws005` | Networking and WPA | [WS005](ws005-networking/ws.md) |
| `ws006` | Input and evdev | [WS006](ws006-input/ws.md) |
| `ws007` | Graphics and desktop | [WS007](ws007-graphics/ws.md) |
| `ws008` | Noct and BeUI | [WS008](ws008-noct/ws.md) |
| `ws009` | Documentation | [WS009](ws009-documentation/ws.md) |

Status, interruption, resumption, and evidence rules are defined in
[governance.md](governance.md).

