# Q085 managed Wi-Fi command stories

Status: all 30 accepted in ordinary and ASan/UBSan host execution after the
structural rewrite and P048. See [individual results and boundaries](results.md).

Every command below has the `net wifi` prefix. `key A auto` abbreviates
`set-key scenario-A scenario-password auto`; B is a second synthetic profile.
`tick` means servicing scheduled daemon work, not another enable command.
Each story starts with a fresh disabled daemon unless its precondition says
otherwise. Assertions cover command exit, state, selected radio, L2/L3 ownership,
and a fixed completion deadline after every step. Faults are OS/radio boundary
inputs; the fixture must execute production command and policy code.

| ID | Commands and injected boundary condition | Required outcome / recovery |
| --- | --- | --- |
| 01 | key A auto; enable; list; disconnect; connect A; disable | Normal complete lifecycle, one DHCP acquisition per connection |
| 02 | enable; key A auto; tick; list; disable | Notification connects without another enable |
| 03 | key A auto; enable; enable; list; disable | Repeated enable preserves working L2 and L3 |
| 04 | enable; enable; key A auto; tick; disable | Repeated idle enable remains usable |
| 05 | enable; key A with wrong key auto; tick; correct key A auto; tick; disable | Failed authentication cleans up and corrected key reconnects |
| 06 | key A auto; enable; disconnect; key B auto; tick; list; connect B; disable | Profile updates cannot override manual pause |
| 07 | key A auto; tick; list; enable; disable | Saving a key while disabled does not enable radios |
| 08 | connect A; key A manual; enable; connect A; disable | Disabled connect fails without mutation; explicit recovery works |
| 09 | key A auto; enable; connect unknown; list; disable | Unknown target leaves active connection intact |
| 10 | key A auto; enable; disconnect; disconnect; enable; disable | Repeated manual disconnect and resume work |
| 11 | enable; disable; disable; enable; disable | Disable is repeatable with no stale owner |
| 12 | key A manual; enable; list; connect A; disable | Manual profile is never automatically selected |
| 13 | key A auto; enable; another owner's invalid store; enable as that owner; list; disable | Failed takeover preflight preserves prior working owner |
| 14 | key A auto; enable; another owner disconnect/connect/profile notification; list; disable | Authorization rejects interference; notification is ignored |
| 15 | enable with no radios; key A auto; attach radio; tick; list; disable | Empty topology remains enabled and later connects |
| 16 | key A auto; enable with first radio failing up; list; disable | Second usable radio wins |
| 17 | key A auto; enable with first radio failing scan; list; disable | Scan failure does not block the second radio |
| 18 | enable; list with one broken radio and one valid cache; repair; list; disable | Partial result retains healthy radio and truthful error |
| 19 | key A auto; enable with malformed first-radio list; list; disable | Malformed radio cannot hide a valid candidate on another |
| 20 | key A auto; enable with scan still running; list; tick after completion; disable | Bounded pending selection and later convergence |
| 21 | key A manual; enable; connect A with rapid repeated scans; list; disable | Real machine progress stays within the 64-record contract |
| 22 | enable/list with child closing pipes before exit; disable | Prompt reap; closed-pipe hung child remains deadline-bound |
| 23 | key A auto; enable; carrier loss queued; connect A; disable | Explicit connect retires the reconnecting identity first |
| 24 | key A auto; enable; external L3 replacement; disconnect; enable; disable | Replaced resources preserved; stale claims cannot pin policy forever |
| 25 | key A auto; enable; disconnect fails busy; carrier event; retry disconnect; enable; disable | Failed retirement retains token, suppresses recovery, retries cleanly |
| 26 | key A manual; enable; connect A with DHCP failure; retry connect A; disable | Failed transaction retires L2/L3 before retry |
| 27 | key A auto; key B auto; enable with connect failure and failed cleanup; retry disable; enable; disable | No second candidate until first L2 retirement is proven |
| 28 | key A auto; enable; detach and reuse name; list; disable; enable; disable | Retired device identity cannot target a replacement device |
| 29 | enable; background scan/connect running; list/profile notification/disconnect/disable | Requests are serviced or background work yields within the transport bound |
| 30 | key A manual; enable; connect A with child wait failure or total deadline exhaustion; retry; disable | Truthful failure, bounded reap and subsequent usable state |

Separate regressions retain direct `wifi list` nonblocking snapshot behavior,
credential store publication, ZNV2 framing, confirmed commit and wired recovery.
Radio doubles do not constitute RTL8822BU/AX211 physical RF acceptance.
