# WS025 fixture と実行証拠

UAS descriptor parser: `bash plan/ws025/tests/run-uas-descriptor-host.sh`.
Tests the production decoder using q144 captures and malformed variants under
ordinary/ASan/UBSan builds. Capture new QEMU descriptors with
`python3 plan/ws025/tests/capture-uas-descriptors.py plan/ws025/temp/OUTPUT`.

IMOD functional comparison: `python3 plan/ws025/tests/run-imod-qemu.py plan/ws025/temp/OUTPUT`.
Builds private 0/160/4000 artifacts, checks actual QMP MMIO readback, and runs
the maintained USB/HID campaign. It does not measure physical IRQ latency.

状態: p001–p004 完了、q092 / p005 高位 RAM の受け入れ実行中。

Parent: [WS025](../ws.md)。[確認項目](../acceptance.md) の ID を fixture/result へ付ける。

## 既存の再利用元

| 既存資料・fixture | 利用する範囲 |
| --- | --- |
| [UEFI normalizer fixture](../../ws003/tests/uefi-memory-map-test.c) | p002 の高位/型/overflow/予約の基準 |
| [BR-T24](../../ws003/tests/uefi-high-memory-usb-boot.sh) | firmware/RAM/USB boot の土台。高位 PFN 使用 oracle を追加して別結果にする |
| [q086 FS50 の実行器](../../ws018/tests/run-storage-acceptance.py) | native/overlay/FS 内容・永続化の回帰 |
| [q086 結果](../../ws018/phase019/results.md) | 既存の合格範囲と性能 baseline |
| [q087 Phase](../../ws018/phase020/phase.md) | 六 syscall、vector/PIPE_BUF/partial の境界。fixture は実行時の最新 owner を確認 |
| [UFS metadata audit runner](../../ws018/tests/run-ufs-metadata-audit.sh) | allocation/publication/失敗を production 実装へ接続 |
| [FAT native VFS runner](../../ws018/tests/run-fat-native-vfs-host-test.sh) | FAT operation/loop coherence |
| [UFS2 consistency runner](../../ws018/tests/run-ufs2-consistency-host-test.sh) | WS024 移行後の journal/snapshot 等の継承確認 |
| [AX211 DMA runner](../../ws004/tests/run-intel-ax211-dma-test.sh) | 共通 DMA 修正の回帰。全 HCD/device の代わりにはしない |

その他の VM/USB/ネットワーク回帰は p001 で最新 runner と ID を対応づける。private `.internal/` へ探索を広げず、必要な再利用 fixture は明示した WS 配下へ置く。

## 新規 fixture の作り方

- memory map/early arena/bitmap/DMA mask は production helper を直接使う host fixture とする。別実装をテストして済ませない。
- kernel の高位 PFN probe は test build または限定された診断入口で、少量の pages を範囲指定して使う。production image に通常 1 GiB cap を残すための設定にはしない。
- file/cache/dirty/error/async は memory disk と scriptable fault backend を使い、各 I/O の受理・完了・stable 状態を個別に注入する。
- crash backend は volatile write 消失・順序入替え・torn record をモデル化する。QEMU kill は別の補助 campaign とする。
- normal/準正常ケースは一起動内で連続実行する。各ケース終了後に fd/mount/claim/dirty/reserve の後始末を確認し、次へ状態を漏らさない。
- reusable source/runner はこの `tests/`、使い捨て log/image は WS 内 `temp/` の untracked 領域、要約と再現 command/hash は該当 Phase の `results.md` に置く。

## 実行 gate

計画時点では build/test を実行しない。Queue 選択後、変更に直接対応する strict/focused/sanitizer gate を先に通す。supported x86 build は `make -j16` で直列に実施し、amd64 runtime は `qemu-system-x86_64` を使う。共有 image/config に対する並列 build/runtime を避ける。

古い fixture 名から現在の boot oracle を推測しない。source/config/artifact identity を記録し、元 image を hash で保護した disposable copy を使う。aggregate `make check` と commit は行わない。

## WS025 memory runners

- `run-memory-host.py OUT`: production helper の ordinary / ASan+UBSan。OUT は未作成の WS temp 子ディレクトリ。
- `run-memory-boot.py OUT bios|uefi MiB [failure-regex]`: 通常 image の disposable USB boot。
- `high-memory.mk` + `run-high-memory.py OUT bios|uefi MiB`: link observer で実 PA を記録し、
  guest が fork/COW/munmap/realloc を 4 周行う。PFN の最後の mapping 後の free まで照合する。
- `high-acpi.mk`: UEFI RSDP を owned high RAM に移す native synthetic fixture。通常の discovery を使う。
- `run-memory-compat.py OUT bios|uefi old-loader|old-kernel OLD_IMAGE`: retained artifact を取り出し、
  現在の image の copy だけへ組み替える。旧 BIOS は 256 MiB、その他は 4 GiB。

Observer を使った後は `make -j16 -W platform/amd64/vmunix.ld ZEDBSD_CONFIG=config/ci/config-amd64.mk`
で通常 kernel/image に戻す。試験用と通常用の artifact/hash は各 results で区別する。
