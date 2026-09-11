# ws009-p003: init and service reference

WSID: `ws009`  
Phase ID: `p003`  
Combined ID: `ws009-p003`  
Status: complete  
Parent WS: [WS009](../ws.md)

## Objective

Publish the current native init and service-management contract without
presenting planned dependency-aware shutdown or definition reload as complete.

## Completion result

- [x] Boot, mount, hostname, and configuration ownership are described.
- [x] `rc.conf` and `service.d` grammars and limits are recorded.
- [x] FD 3 readiness, service commands, restart behavior, and shutdown are
  documented from the current implementation.
- [x] Current gaps (`required`, shutdown order, definition reload) are explicit.
- [x] Documentation navigation and relative-link validation pass.

## Evidence

Run DOC-T00 from the [shared test index](../tests/README.md). Behavioral source
anchors are linked from the published
[reference](../../../docs/reference/init-services.md).
