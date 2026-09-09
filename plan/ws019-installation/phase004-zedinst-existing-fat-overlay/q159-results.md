# q159 results: public coexistence installer

Date: 2026-09-09. Queue finished; p004, p025 and p026 completed.

The ordinary amd64 image packages /bin/zedinst and /bin/noct. The installed
launcher now completes cancellation, installation and exact rerun. Noct manages
large-image copying with byte progress. The original cp timeout, public shell
status and null/zero prerequisites are resolved by p025/p026.

| Requirement | Evidence and scope |
| --- | --- |
| Admission and failure boundaries | q156/q157 results: 576 admission combinations, retained metadata/discovery mutations and native preflight. installer-transaction.noct injects each operation failure, including before/after publication, and checks cleanup plus all existing-file prefixes (102 scenarios). Destination tests cover all 64 subsets, 384 file conflicts and four path conflicts. These are model/component tests, not power-cut durability claims. |
| Public command from ordinary USB root | temp/q159-public6/guest.log and result.json: cancel, install and rerun passed, six final files published, status 0. Usage, noninteractive and source-alias refusal passed. |
| Preservation | public6 cancellation whole-disk comparison passed. GPT, FAT boot information, both sentinels, UEFI variables and production hashes compare equal before/after. |
| Exact rerun and differing-byte refusal | public6 rerun passed with zero extra required capacity. temp/q159-conflict2/result.json is PASS: changed kernel byte refused, digest unchanged by refusal, original restored and cmp passed. Accepted original target hash is unchanged. |
| Build and package | q159 progress and p025 results record amd64/pcat/pc98 builds, standalone/package verification and 16 public runtime checks. p026 records the later amd64 image build and packaged Noct module verification. |

public6's aggregate result remains FAIL: its later test used the shell builtin
cp instead of /bin/cp. It stopped before corrupting the kernel. The corrected
conflict-only runner starts from a disposable copy of that installed disk and
requires the terminal successful cases and unchanged protection hashes first.
conflict1 passed guest behavior but incorrectly compared UEFI variables across
OVMF initialization; conflict2 records preboot separately and compares across
the installer operation after login. Both historical failures are preserved.

Production image SHA-256:
4425d3655705ae08c6b3296d5cab88259dfebf8337635fe397acd576bdc93d0d

Accepted original target: temp/q159-public6/gpt.img, SHA-256:
0cc79ab8c01c498fd313651969a27670f306eb8416ec9e90554578f72af46f77

Sessions 9572, 53670 and 93168 are terminal. No QEMU remains from this queue.
p005 must still prove installed NVMe-only boot; p006/p007 dedicated installation
and p027/p028 source selection/tree copy are required subsequent work. This
completion covers the p004 coexistence transaction, not all of WS019.
