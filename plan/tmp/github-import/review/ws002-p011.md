# ws002-p011: service foundation and administrative layout

WSID: `ws002`

Phase ID: `p011`

Status: complete

Parent WS: [WS002](../ws.md)

## Objective and design

Freeze service/rc.conf parsing, declarative `/etc/service.d` records, runtime
paths, `/bin` versus `/sbin` classification, and independent package Makefiles
before PID 1 depends on them. Configuration is parsed as data and never sourced
as shell code.

## Acceptance and result

Parser/path/install host gates and the top-level build passed as part of the
completed WS002 baseline. Exact original scope and transitions remain in the
[legacy plan](../history/phase011-019-legacy-plan.md); shared cases are indexed
in [WS002 tests](../tests/README.md).

## Interruption record

Not interrupted. Later parser defects are new WS002 maintenance or a consumer
WS Phase; they do not reopen this historical record silently.

## Completion conditions

- rc.conf and service-definition parsers reject malformed/unsafe input predictably.
- Administrative paths and `/bin` versus `/sbin` placement are fixed and tested.
- Relevant base packages honor the documented standalone build/install contract.
