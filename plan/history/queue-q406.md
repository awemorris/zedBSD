<!-- awesome-plan project=zedbsd record=queue-q406 -->

# Queue q406: vmunix の LTO の調査と設計（ws053-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「clang/llvmのLTOを安全にvmunixに適用できないか、検討をお願いします。」「LTOは検討後、実現可能なら、適用をお願いします。これは優先度高めでお願いします。」「引き続き自走してください。」範囲は [ws053-p001](../ws053/phase001/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q406-i01 | [ws053-p001](../ws053/phase001/phase.md) | cleared（LTO は安全に適用できる。full LTO+HAL が速く小さい） |

依存: なし。HAL を含めることは 2026-09-24 ユーザー判断「統合して大丈夫です」。

Upcoming Work Outlook: WS053 の適用の Phase（p001 の結果による）、ws046-p007（guest の clang の遅さ、中断中）、p008、p005、WS047 p001、WS045、WS049〜WS052（優先度の指示待ち）。

結果: ws053-p001 cleared。vmunix の LTO は安全に適用できる。full LTO で kernel に `memmove`・`memcmp` が無いことが分かり、`kcrt.c` に足した。
HAL を含める（ユーザー判断）。full LTO+HAL: boot・make の差分試験 91/91・対話 41/41、system call −10%・cached read −16%・exec −3%、vmunix −2.8%、link 20 秒。
既定を full にし `ZEDBSD_KERNEL_LTO` で選べるようにする設計。guest の clang の遅さは LTO では直らない（kernel の処理の量）。
