# WS012 design review cases

Parent: [WS012](../ws.md)

The only current Phase is a discussion Phase. These are review cases, not
executable tests and not implementation authorization.

| Case | Required design result |
| --- | --- |
| `SVC-D001` | Every argv and interactive command has one unambiguous state transition |
| `SVC-D002` | Runtime operations and persistent enablement cannot be confused |
| `SVC-D003` | The mapping-only YAML proposal, immediate enable/disable, stable file locking, atomic rewrite, reload, and concurrent-session outcomes are explicitly accepted or revised |
| `SVC-D004` | The newline-delimited `ZSV1` PID 1 records are bounded, versioned, terminated by `END`, and usable without parsing display text |
| `SVC-D005` | Read-only operations and privileged mutations have explicit authorization rules |
| `SVC-D006` | Ordinary services are complete without CPAR; the machine protocol reserves later extension without making blocked container design an initial prerequisite |
