# WS020 shared tests

Parent: [WS020](../ws.md)

| Case ID | Phase | Required observation |
| --- | --- | --- |
| `MAC-T001` | p001 | Architecture/Board/Variant and capacity values round-trip, validate, and leave compiled amd64 artifacts invariant |
| `MAC-T010` | p002 | Variant-aware image checker proves exact MBR/GPT/partition/loader inclusion and exclusion contracts |
| `MAC-T011` | p002 | Primary-only GPT accepts only a valid, capacity-matched, zero-backup materialized medium and rejects corrupt/contradictory alternatives |
| `MAC-T020` | p003 | Hybrid positive 2-firmware cells, single-firmware positive/negative cells, and all eight UEFI capacity cells pass in QEMU |
| `MAC-T030` | p004 | One provisional Intel Mac boot passes, followed only at final acceptance by five consecutive cold boots of the frozen artifact |

Reusable runners added by an authorized Phase live here.  Disposable sparse
media and QEMU logs live below `../temp/` and remain untracked.  Do not consume
`.internal/` or invoke aggregate `make check`.
