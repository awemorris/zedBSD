<!-- awesome-plan project=zedbsd record=queue-q411 -->

# Queue q411: guest の clang の遅さ（ws046-p007 の再開）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「これはコンパイルできたとしても遅すぎます。issueとして扱います。ゲストでclangが遅いことを解決するphaseを書いてください。」「引き続き自走してください。」範囲は [ws046-p007](ws046/phase007/phase.md)（調べる範囲: 測定と、根拠のある原因 2 つまでの修正）。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q411-i01 | [ws046-p007](ws046/phase007/phase.md) | uncleared（主因の libc の allocator と buffer cache の hash を直した: guest の compile 513 秒 → 5〜7 秒、expat の make 97 秒。512 MiB で受け入れの 2 項目が未達、残りを p009 へ） |

依存: ws046-p004（cleared）。WS053（completed）の後に再開（q405-i01 の再開の条件）。kernel の VM を変える場合も HAL は変えない（要るなら差分ごとに承認を求めて uncleared）。

Upcoming Work Outlook: ws046-p008（guest で package を最後まで。依存を p007 の修正に直した）、ws046-p009（BUG-033 の残り）、ws046-p005（規約と回帰）。

## 結果の要約（ws046-p007、q411-i01）

- 測定（guest 512 MiB）: `xmlparse.c` の compile が 513・550 秒（host 1.03 秒）。memory・swap・disk は律速でなく、gdbstub の標本で clang が user 空間の同じ loop にいた。
- 修正 1（`src/libc/heap.c`・`heap.h`）: `free()`・`realloc()` の pointer の確かめを鎖をたどる代わりに両隣の指し返しで、伸長の最後の block を `last` で。
  host の模型の trace の hash が修正の前と一致、ws004 の heap の試験 PASS。guest の compile 5.4〜6.4 秒。
- 修正 2（`src/kern/buf.c`）: buffer cache の hash の bucket 64 → 8192、key を乗算の hash に（line の揃いで 8 つの bucket しか使われていなかった）。2 GiB で compile 5.05 秒（host の 4.9 倍）。
- expat の make: 512 MiB で 97 秒、2 GiB で 91 秒（前は 51 分以上で未完）。
- 回帰: 5 config の build（我々の code の warning 0）、4 platform の boot、guest の sh 1388/1425（落ちる集合が WS053 p002 と同じ）、make 91/91、対話 41/41、SMP-STRESS 0。
  POSIX-R2 の失敗は前からある（BUG-034）、POSIX-R2-REMAINING は compile できない（BUG-035）。
- uncleared: 512 MiB で `clang --version` 0.89〜1.06 秒、compile が host の 6.3〜6.7 倍。残り（file の cache の大きさ・共有、USB の busy wait）を ws046-p009 に。
  harness の既定の memory を 2 GiB にするかはユーザーの判断の候補。
