<!-- awesome-plan project=zedbsd record=ws035p007 -->

# ws035-p007: hda ドライバ（QEMU intel-hda、再生・録音）

Phase ID: `ws035-p007`
Parent: [WS035](../ws.md)
Status: **cleared**（q342-i01、2026-09-24）
Phase disposition: normal
Queue: q342（q342-i01）
実行: メインセッション
設計: [hda-design.md](../hda-design.md)（p022）

## 作ったもの

| ファイル | 内容 |
| --- | --- |
| `src/drivers/pci/pci-hda.c`、`include/drivers/pci/pci-hda.h` | HD Audio driver。class 0x0403 の controller ごとに `/dev/dspN`・`/dev/mixerN` |
| `Makefile`、`config/drivers/pci.drivers`、`platform/amd64/vmunix.mk`、`src/kern/platform/pcat.c` | `CONFIG_DRIVER_PCI_HDA`（amd64、既定 n）。`KERN_AUDIO_BACKENDS ?= $(CONFIG_DRIVER_PCI_HDA)` |
| `include/drivers/audio/audio.h`、`src/drivers/audio/audio.c` | p006 の変更（下の「フレームワークの変更」） |
| `plan/ws035/tests/` | `hda-fixture.c`・`run-hda-fixture-test.sh`（host）、`audiotest.c`（ゲスト）、`hda-wav-check.py`、`boot-hda.sh`、`run-hda-qemu.sh`、`config-amd64-hda*.mk` |

## フレームワーク（p006）の変更

1. **DMA device を backend が渡す**（設計 §3）: `drv_audio_register(ops, private_data, dma, &device)`。`constraints` を ops から除いた。
   hda は `drv_pci_device_dma()` を渡す。
2. **DRAIN は最後の byte の後に無音の fragment を2つ流してから止める**。QEMU で見つけた: DMA が最後の byte を取った時点で
   止めると、QEMU の codec が持つ 8 KiB の buffer にある最後の約 27 ms が鳴らない（96000 frame 中 1295 frame が欠けた）。
   実機の controller にも FIFO がある。DMA の位置は音より先を行くので、その分を無音で押し出す。
   リングの書き込み位置より先は割り込みが消した無音なので、余分に流すのは無音だけ。

## 設計（p022）からの差分

1. **RIRB の応答 status（`RINTCTL`）を有効にした**。設計は RIRB の割り込みを使わないとしていたが、QEMU は
   `RINTCNT` 個の応答の後、software がその status を消すまで CORB を止める。status は `RINTCTL` が有効なときだけ立つので、
   無効のままでは2つ目の命令から先が永久に止まった（ゲストで `hda: codec 0 not used: 42`）。
   controller の割り込み（`INTCTL.CIE`）は無効のままなので、割り込みは来ない。各命令の後に status を消す。
2. **待ちは tick に加えて register の読み出し回数でも打ち切る**。PCI の attach は scheduler の tick が進む前
   （`kernel_main` より前の platform 初期化）に走るので、tick だけで待つと永久に待つ（ゲストで boot が
   "TIMECOUNTER READY" で止まった。CPU 0 は `sched_yield` の中を回っていた）。MMIO の読み出しは PCIe を渡るか
   VM exit になるので 1 回 0.2 µs 以上かかる。1 ms あたり 5000 回で上限を置く。venus の reset 待ちと同じ考え方。
3. **snoop の強制は PCI Express capability の Device Control の Enable No Snoop（bit 11）を消す**。設計は Intel の
   `DEVC`（0x78）の bit 11 としていたが、それは HDA の PCIe capability（0x70）の Device Control そのものなので、
   capability を探して消す形にした（vendor を問わない。capability が無ければ何もしない）。
4. codec を使わなかった理由を log に出す（`hda: codec N not used: E`）。p008 の実機確認で要る。
5. publish の log に割り込みの種類（MSI・INTx）を出す。

## 検証

| 検証 | 結果 |
| --- | --- |
| host fixture `run-hda-fixture-test.sh`（通常＋ASan/UBSan） | **PASS** 3 試験 |
| audio framework の fixture（p006、変更後） | **PASS** 12 試験 |
| amd64（hda あり・なし）、pcat・pc98（audio のみ）の `vmunix` | PASS、warning 0。hda なしの CI 構成は audio の symbol 0 |
| QEMU 6 構成（`run-hda-qemu.sh`、最終 source で再実行） | **全部 PASS**（下表） |

host fixture は register の模型と、CORB に書かれた verb に RIRB で答える codec の模型を持つ。
**QEMU の CORB の流量制御も真似る**（`RINTCNT` で止まり、`RINTCTL` で立つ status を消すまで進まない）。
修正前の driver をこの fixture にかけると、ゲストと同じ `codec 0 not used: 42` で落ちることを確かめた。

- QEMU の hda-duplex 相当: reset、CORB/RIRB、経路（pin 3 出力、pin 5 入力）、BDL の中身（8 entry、4096 byte、IOC）、
  stream の register（CBL 32768、LVI 7、FMT 0x0011、tag 1）、converter の verb、RUN、割り込みの振り分けと status の消去、
  音量（50% で 0x4a×50/100、mute bit）、detach で全部返る。
- 実機に似た codec: Intel の display codec（address 0）を飛ばし、address 2 の codec を使う。内蔵 speaker（EAPD あり）と
  headphone（HP bit）が mixer 経由で同じ DAC、録音は mic jack を内蔵 mic より優先（selector で 1 を選ぶ）、音量は DAC。
- 失敗: 答えない codec、display codec だけの controller。attach は失敗し、何も残さない。

QEMU（`plan/ws035/phase007/evidence/` に各構成の記録）:

| 構成 | 結果 |
| --- | --- |
| `ich9-intel-hda`＋`hda-output,mixer=off`、WAV 48 kHz S16 2ch | 2 秒 96000 frame が **bit 一致**（frame 0 から、前に無音以外なし）。2008 ms、underrun 0、MSI |
| 同じで 1000 frame（半リングに満たない短い音） | **bit 一致**。DRAIN が start して鳴らしきる |
| `hda-output`（`mixer=on`） | 振幅 16000 の矩形波が、音量 100 で 16000、50 で 7969、mute で 0 |
| `hda-duplex`＋`-audiodev none` | 録音 1 秒分が 1017 ms、3 秒分が 3008 ms、overrun 0。同じ開き方で再生も通る |
| `intel-hda`（ICH6、8086:2668） | 96000 frame が **bit 一致** |
| `ich9-intel-hda,msi=off` | INTx で 96000 frame が **bit 一致** |

## 確かめていないこと

- **実機**（p008、人が行う）。Latitude 5320 の HDA は VFIO で渡せない。確認項目: codec の選択と経路（speaker・headphone）、
  EAPD、snoop（PCIe の No Snoop）、LPIB の精度、DSP（SST・SOF）構成の機械で attach しないこと。
- 録音の中身（QEMU の `none` は無音しか出さない）。速さと流れだけを確かめた。
- 44.1 kHz と S32 の再生（QEMU の codec は 32 bit を受けないので S32 は一覧に出ない。44.1 kHz は一覧に出るが鳴らしていない）。
- jack の抜き差し、複数 controller、detach（PCI の hot-unplug）はゲストでは試していない（fixture だけ）。
