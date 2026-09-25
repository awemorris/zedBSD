<!-- awesome-plan project=zedbsd record=ws061 -->

# WS061: expat の configure と compile を Linux と同等の水準にする（fg011）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG004
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q446（finished）
Resume point: 受け入れの計測は達成（p009 の後: configure 9.1〜9.4 秒・host 10.7 秒、`cc t.c -o t` 75〜85 ms・host 83〜85 ms、make（直列）12.3〜12.8 秒・host `-j1` 15.2 秒）。WS の完了には規約の Phase が要る（ユーザーの指示で最後）。並列の make は WS064
<!-- awesome-plan-current:end -->

## 目標

ユーザーの直近の目標（2026-09-25）: guest の expat の `./configure`（162 check、今 91 秒）と compile（`cc t.c -o t` 0.63 秒、link 0.36 秒）を Linux（同じ clang、host）と同等の水準にする。
BUG-033 の系譜（ws046-p007・p009・p012）の続き。fg010（Wayland）は継続。

受け入れ: host の Linux で同じ toolchain の値を参照に、configure の時間が host の 2 倍以内、`cc t.c -o t` が host の 2 倍以内（参照値は p001 で測る）。回帰は各 Phase で。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws061-p001](phase001/phase.md) | host の参照値と、guest の 1 check の費用の内訳 | cleared（q432-i01。host: configure 11 秒・cc 83〜92 ms。guest は全て page fault に律速: 1 fault 約 30 µs、`clang --version` 5797 fault、`true` 306 fault。configure 中の kernel 標本: fault 22%、exit の page table 解体 13%、USB の同期 flush 20%） | — |
| [ws061-p002](phase002/phase.md) | page fault の固定費用と exit の page table の解体 | uncleared（q433-i01。anon fault 8.4 → 4.2 µs、file 13.6 → 8.4 µs、`true` 8 → 4〜5 ms（目標 3 ms は未達）、`clang --version` 160 → 80〜100 ms。VM object の registry の hash で **configure 87 → 59〜64 秒（overlay）、59 → 35〜38 秒（tmpfs）**。残りの fault の数は p003、複写は ws046-p014） | p001 |
| [ws061-p003](phase003/phase.md) | fault-around | uncleared（q435-i01。file fault 3.2 → 1.4 µs、`clang --version` の fault 5862 → 1891（目標 1/8 は未達）、`cc t.c -o t` 0.21〜0.24 秒、configure 31 秒（tmpfs）・55 秒（overlay）。TLB: map の shootdown を削除、小さい範囲は invlpg。BUG-051（sshd の SIGSEGV 1 回）未解決） | p002 の変更、ws046-p014 |
| [ws061-p004](phase004/phase.md) | root の overlay（USB 上の loop の UFS）への書き込みの同期を減らす（configure の 22〜26 秒） | planned（2026-09-25 ユーザー指示で layout の変更 WS062 を先に。WS062 の後に不要なら取り消す） | — |
| [ws061-p005](phase005/phase.md) | process の生成と終了の固定費用: fork した子を同じ CPU に置き idle が盗む、解放した page の使い回し（HAL）、HAL の byte loop・registry lock・memstat・mutex_owned、destroy の unmap | cleared（q438-i01。`true` 3.4 → 1.1 ms、configure（`/root`）35 → 17〜20 秒・tmpfs 29 → 13 秒（host 11.2 秒）、make（直列）34 → 20〜25 秒（host `-j1` 15.2 秒）。thread の移動で出た race 3 つと page の stack の退行を直した。並列の make は F-016） | p003 の変更 |
| [ws061-p006](phase006/phase.md) | UFS の write cached を既定に、write-through を mount option に（2026-09-26 ユーザー指示） | cleared（q439-i01。configure（`/root`）17〜20 → 12.3 秒、device の書き込み 1/10、flush 1/80。journal の無い volume は電源断で漏れが残る → p007） | p005 |
| [ws061-p007](phase007/phase.md) | configure の残りの kernel の時間（readdir の読み、fault の待ちの走査） | cleared（q442-i01。configure（`/root`、journal）13.0 → 11.5 秒、host 10.9〜11.2 秒） | p006 |
| [ws061-p008](phase008/phase.md) | make を host と同等に（libc の同期の system call）（2026-09-26 ユーザー指示） | cleared（q444-i01。make 17.9 → 12.8〜13.1 秒、host `-j1` 15.2 秒。unlock・signal が待つ者がいなくても `usync` していた） | p007 |
| [ws061-p009](phase009/phase.md) | `cc t.c -o t` を host と同等以上に（2026-09-26 ユーザー指示） | cleared（q447-i01。loader の symbol の探索の hash を 1 度に、`cc`・`ld` を link に: `cc` 90〜99 → 75〜85 ms（host 83〜85）、configure 9.1〜9.4 秒（host 10.7）） | p008 |
| [ws061-p010](phase010/phase.md) | system call の入口を `syscall`/`sysret` に、libc の lock の adaptive spin（2026-09-26 ユーザー指示「優先」） | cleared（q446-i01。入口の費用 4.65 → 1.54%、`cc` 99 → 87 ms、configure 9.8〜10.2 秒・host 11.0 秒） | p008 |
| [ws061-p011](phase011/phase.md) | WS061 の変更の規約の適合（ユーザーの指示で最後） | planned | p009・p010 |
| [ws061-p007](phase007/phase.md) | UFS の journal を既定に（journal の無い image でも mount の時に作る）、`nojournal` の mount option、journal と write cached の両立（2026-09-26 ユーザー指示） | in-progress（q440-i01） | p006 |
