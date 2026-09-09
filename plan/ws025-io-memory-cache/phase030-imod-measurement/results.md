# ws025-p030 results

## q143 QEMU comparison (2026-09-09)

Implemented `tests/run-imod-qemu.py` and the private USB/HID runner's validated
`USB_HID_XHCI_IMOD` override. Each cell uses a separate build/image. QMP discovers
the xHCI PCI BAR, reads RTSOFF, then reads interrupter-zero IMOD. Observed low
16 bits match the requested interval for **0, 160 and 4000**, and all three
USB/HID campaigns pass.

Evidence: `../temp/q143/result.json`, `i*/metadata.txt`, `i*/results.tsv`,
`i*-readback.json`, and `i*/xhci/guest.log`. Each includes ordinary USB-root
boot, capability discovery, keyboard-to-console routing, relative/absolute
events, stale-descriptor hotplug generations, and concurrent 64 MiB root-device
reads with pointer events. All private builds use `make -j16`. Shell/Python
syntax and relevant `git diff --check` pass. Production xHCI source and user
config hashes remain unchanged; default remains 4000.

Wall time in JSON includes builds and scripted key delays: it is **not**
device latency or a CPU comparison. The raw read probe checks successful byte
counts, not write persistence. No WLAN, write/fsync, IRQ/s, CPU, percentile
latency or new terminal-halt measurements were made in this queue. p030 remains
uncleared for those cells. Next software step: add a bounded native write/fsync
and readback workload with actual counters/timestamps to these same isolated
artifacts. Physical tuning remains separate; lack of hardware does not prevent
that implementation.

## Historical q125 result

Status: uncleared
Date: 2026-09-09

## 確認した事実

HEAD 34a1f6dの現ソースとq122/p024の既存証拠を確認した。
現行IMOD=4000。0/160/4000の実機比較が採用条件で、QEMUの同値結果は代用不可。現在テスト機SSHがtimeout。

## 未完了と再開

実機比較の実行環境と複合topologyを取得できず、既定値選択に必要なデータがない。

比較可能な実機topologyと遠隔または直接起動経路。

production実装・新規性能測定を行ったとは主張しない。既存挙動と既定値を維持した。
