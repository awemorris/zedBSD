<!-- awesome-plan project=zedbsd record=queue-history q430 -->

# Queue q430: private の file の mapping の page cache の共有の設計を直す（ws046-p012）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q430
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「…引き続きパフォーマンス問題の修正と、バグ修正と、上記観点での修正をお願いします。」（パフォーマンス: BUG-033 の残り）。範囲は [ws046-p012](ws046/phase012/phase.md)（設計 1・3・4 の実装と測定、p011 の当て直しは失敗の再現まで）。HAL は変えない。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q430-i01 | [ws046-p012](ws046/phase012/phase.md) | cleared（object cache を LRU に、最後の unmap で mapping の object を cache に残す、clean な object の同期 walk を省く。`cc t.o -o t` 0.51〜0.68 → 0.36〜0.38 秒、`libLLVM.so` の file の fault 25 → 15.3 µs/page、2 つ目の process は cold の pass 無し。configure 91 秒。`KERN_SYSTEM_DROP_CACHES` を足し、stress の baseline の間欠（BUG-045）は非同期の後始末との競走と特定して試験を直した。回帰 boot PASS・sh 1389/1425（1 件改善）・make 91/91・COW OK・SMP 6/6。p011 の当て直しは p014） |

依存: ws058-p002（object cache 256、cleared）。

Upcoming Work Outlook: ws057-p003（判断待ち）、ws056-p001 の判断（BUG-046）、BUG-036・039・040・041、WS055。
