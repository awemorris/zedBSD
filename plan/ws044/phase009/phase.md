<!-- awesome-plan project=zedbsd record=ws044p009 -->

# ws044-p009: 実機で init が SIGILL で落ちる（命令 cache の同期）

Phase ID: `ws044-p009`
Parent: [WS044](../ws.md)
Status: cleared（2026-09-24。実機で login prompt まで起動）
Queue: ユーザーの報告による割り込み。

## 報告（2026-09-24 ユーザー、実機）

「実機で起動し、initが落ちました。 kern: pid 1 killed by signal 4 (vector 0) at 0x0000000010006a59c, address 0x0000000000000000」

## 調べたこと

- "vector" は ESR の例外 class（`src/kern/signal.c` と `src/hal/arm64/int.c`）。0 は Unknown reason（未定義命令）。WFx の trap（1）や system register の trap（0x18）ではない。
- image の全ての実行 file と library に、ARMv8.1 以降（LSE の atomic、LDAPR ほか）や暗号拡張（BCM2711 には無い）の命令は無い。EL0 で trap する命令は init の到達しない `wfe` だけ。
- kernel は program の code を普通のデータの書き込み（SD からの PIO の copy、page cache）で memory に置く。Cortex-A72 の命令 cache はデータ cache と同期しないので、
  実行する前にデータ cache を PoU まで書き出し、命令 cache を無効化する必要がある。arm64 の HAL には `hal_sync_instruction_stream` があるが、どこからも呼ばれていなかった（`ptrace` の `hal_icache_invalidate_range` だけ）。
  QEMU には cache が無いので起きない。
- あわせて: 診断の差分（p008）で `SCTLR_EL1` を確定した値にしたとき、EL0 の権限（UCT・DZE・UCI・nTWI・nTWE）が切れていた。

## 変更（HAL、承認済み）

差分 `plan/ws044/proposed/arm64-icache-sync.diff`（承認 2026-09-24「承認、すぐ進める」）:

- `space.c`: user の page を実行可能に対応付けるとき（`hal_space_map`）と実行可能に変えるとき（`hal_space_prot`）に、その page を `hal_sync_instruction_stream` で同期する（device と非 cache は除く）。
- `locore.S`: MMU の有効化で EL0 に UCT・DZE・UCI・nTWI・nTWE（Linux と同じ）。

## 検証

| 検証 | 結果 |
| --- | --- |
| rpi4 の image の build | 成功（`build/rpi4-font/hdd-image.img`） |
| QEMU raspi4b の boot test | login prompt |
| 実機 | ユーザーの確認待ち |

## 実機の結果（2026-09-24 ユーザー）

「ログインプロンプトまで表示されました。USBが使えないみたいです。」 init の SIGILL は直った。USB は driver が無い（PCIe の root complex・VL805・非 coherent な DMA）ので [WS048](../../ws048/ws.md) として計画した。
