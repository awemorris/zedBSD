# ws019-p028 acceptance — q161

Status: completed, 2026-09-09. This accepts reusable native-tree utilities and
Noct adapters. Source admission, dedicated provisioning and full installer UI
acceptance remain p027/p006/p007/p029. WS019 is not complete.

| Contract | Evidence |
| --- | --- |
| Complete byte-safe enumeration; implicit print once | find-manifest-test.py: 19; temp/q161-find1 native PASS with exact NUL digest and absent-source failure |
| Owner/mode/time, recursion, physical links | cp-attributes-test.py: 13; cp-tree-test.py: 17; archive2/3 native UFS contents, owner 17:23, mode 6751, copied inode isolation, hard/symbolic links and sync/remount |
| Accurate completion records | cp-report-test.py: 10, including third/last file metadata failure leaving two records and no END, report write/close failures and skips |
| Frozen census before copying; monotonic progress | installer-treeprogress.noct: 25 in both host engines and native; tree-copy-test.py: 12 actual command/PTY cases; archive3 native 0/4, 2/4, 4/4 with separate directory counts |
| Partial/unknown/duplicate records and child cleanup | Above parser/adapter tests; nonzero status overrides END; callback failure reaps child; missing report fails |
| Independent contents/attributes/link verification | diff-tree-test.py: 24, both entry sets, binary/unusual-name bytes, symlink cycles, split/merged hard-link groups, nanosecond differences, short reads and EOF/read/close/traversal/output faults; depth refusal under 1-MiB stack |
| Native nanosecond persistence | time1/time3 reproduce zero fractions; getter/setter ABI guards removed; time4 and final verify2 persist 1700000001.123456789 / 1700000002.987654321 for file, directory and link |
| Noct verifier on target | verify2 accepts equal read-only UFS trees, then intentional permission change produces diff status 1; independent on-disk timestamp checks pass |
| Maintained builds | /tmp/zedbsd-q161-final-{amd64,pcat,pc98}.log PASS, session 73258 terminal; final verify2 session 49285 exit 0; git diff --check PASS |

Final production SHA-256:
`eb64e9f7088706bba0815b1f80ce9e2601c205e27535c89e82c8814e5d152d1d`.
Production stayed unchanged. Raw NVMe UFS was the intended writable test target.
Current root-image inventory: 38 directories, 242 regular files, one symlink,
no hard-link groups, 17,327,492 regular bytes, maximum pathname 66 bytes. All
actual node types are supported; fixtures also cover hard links/FIFO/unusual
names. Sockets are explicitly unsupported.

Failures remain preserved: archive1 harness key mapping; time1/time3 timestamp
loss; time2 additional USB enumeration, independently recorded as BUG-017.
NVMe acceptance is not USB recovery. The real installer screenshot was shown
from temp/q161-find1/installer-confirmation.png; its escape artifact is p027.
No aggregate make check or physical media writes. All handles are terminal.
Detailed history: q161-progress.md.
