<!-- awesome-plan project=zedbsd record=ws034p003 -->

# ws034-p003: PCI列挙UAPI（`/dev/system`）と lspci

Phase ID: `ws034-p003`
Parent: [WS034](../ws.md)
Status: **cleared**（q325-i03、2026-09-23）
Phase disposition: normal
Queue: q323（q323-i08、未着手のまま uncleared）→ q325（q325-i03、cleared）
実行: メインセッション

## 範囲

`/dev/system` に PCI の列挙を足し、`userland/base/lspci` を独自実装する。
UAPI の形は WS034 の決定（2026-09-23 ユーザー承認）に従う: index 指定の `_IOWR`、
既存の `KERN_SYSTEM_GET_DEVICE` と同じ列挙方式、名前の表は同梱しない。

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `include/uapi/system.h` | `struct system_pci_device_info`（72 byte、固定幅のみ）と `KERN_SYSTEM_GET_PCI_DEVICE`（`_IOWR('s', 13, ...)`。12 は `KERN_SYSTEM_GET_MOUNTS`）。大きさと `vendor`・`driver` の offset を `_Static_assert` で固定 |
| `src/drivers/pci/pci.c`、`include/drivers/pci/pci.h` | `drv_pci_system_describe(index, info)`: `drv_pci_foreach_device()` の順で index 番目の function を探し、address・vendor/product・subsystem・class/subclass/prog-if・revision・header type・bound driver 名を埋める。範囲外は `ENOENT` |
| `src/drivers/generic/system-device.c` | `KERN_SYSTEM_GET_PCI_DEVICE` の handler。PCI core を link しない platform（pc98・sun4u・x68k・rpi4）では weak 参照が NULL になり、最初から `ENOENT`（kernel の既存の weak 参照と同じ形） |
| `userland/base/lspci/` | `lspci [-Dkv]`。`lspci -n` と同じ数値の形（`00:02.0 0300: 8086:46a8 (rev 0c)`）で address 順に並べる。`-k` で bound driver、`-v` で subsystem と header type、`-D` で segment |
| `config/ci/config-amd64.mk`、`config/ci/config-pcat.mk`、`plan/ws035/tests/config-amd64-userland.mk` | `ZEDBSD_USER_PROGRAMS` に `lspci` |

## 検証

| 検証 | 結果 |
| --- | --- |
| amd64 `disk-image` | PASS。新しい warning なし |
| pcat（i386、ILP32）・pc98（PCI なし、weak 参照）の `vmunix` | PASS。i386 で `_Static_assert` が通る |
| `run-uapi-hosted-test.sh` | PASS |
| QEMU q35（KVM）で `lspci -k`・`lspci -Dv` | 7 function。address・vendor/product・class・subsystem が QEMU の `query-pci` と一致。xHCI に `Kernel driver in use: xhci` |
| 不正な引数（`-x`、余分な operand） | usage を出して exit 2 |

証拠: `evidence/`（ゲストの出力と QEMU の `query-pci` から作った一覧）。
revision は `query-pci` に無いので突き合わせていない。

## 残したこと

- 実機では試していない。
- PCI の bus tree には lock が無い（既存の `drv_pci_find_device` なども同じ）。bus の rescan と
  同時に列挙すると、途中の状態を見る可能性がある。PCI の hot-plug は今は無い。
- `ws034-p004`（USB 列挙と lsusb）は同じ `/dev/system` に足すので、この Phase の後に行う。
