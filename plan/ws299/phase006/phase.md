<!-- awesome-plan project=zedbsd record=ws299-p006 -->

# ws299-p006 NVMe の HMB（Host Memory Buffer）を BG5 に渡す

- 親: [WS299](../ws.md)。Queue: q1640 / q1640-i01（担当 P1）。前提: p003（NVMe の大きな転送）。
- Status: in-progress（2026-11-13 q2044 P2: 受け入れ 3 を満たした（QD1 104%、QD32 direct 97%・cache 経由 107%）。recovery・resume の再有効化は未実施。cleared は Q1 の判定）
- Disposition: normal
- 出典: 2026-10-27 ユーザー「Debianの95%以上をゴールにします。」と HMB は「やる」（ws.md）。

## 測った事（2026-10-28、trim されていない範囲、`randread.c`）

- Debian（`deb2-read`）: 4 KiB の乱読み QD1 HMB on 57.8 MB/s（70.9 µs）、off 36.4（112.5 µs）。QD32 on 964 MB/s（235k IOPS）、off 448。fio も同じ数。
- zedBSD（main の kernel、`zb7-base-read`）: QD1 35.9 MB/s（113.5 µs）、QD32 445 MB/s（108653 IOPS）＝ HMB off の Debian と同じ。乱読みの差は HMB だけ（queue・VMD の vector の数ではない）。
- phase001 の表の HMB on の 36.0/340 は trim 済みの範囲の数で、比べない（p005 の「測定の条件」）。

## 設計（2026-10-28、kernel-and-driver-designer の案、P1 が決めた点を併記）

### 仕様の値（NVM Express Base Specification、Identify Controller と Set Features の Host Memory Buffer。版・図の番号は実装の時に仕様で確かめて block ごとに書く）

- Identify Controller: HMPRE（byte 275:272、4 KiB 単位）、HMMIN（279:276、4 KiB 単位）、HMMINDS（335:332、4 KiB 単位、0 は制限なし）、HMMAXD（337:336、descriptor の最大数、0 は制限なし）。HMPRE ≠ 0 が支持。
- Set Features（09h）FID 0Dh: CDW11 bit0 EHM・bit1 MR、CDW12 HSIZE（CC.MPS の page 単位）、CDW13 HMDLLA（16 byte 揃え）、CDW14 HMDLUA、CDW15 HMDLEC（entry の数）。descriptor 16 byte（BADD 8 byte、MPS 揃え・BSIZE 4 byte、MPS page の数・予約 4 byte）、LE。
- controller の reset（CC.EN=0）で HMB は無効になる。再び有効にする時は MR=1 で同じ memory を返す。

### 大きさ

- target = min(HMPRE × 4 KiB, `NVME_HMB_MAX_BYTES`)。`NVME_HMB_MAX_BYTES` は pci-nvme.c の中の定数（既定 64 MiB、BG5 の HMPRE と同じ。P1 の決定: Makefile の CONFIG は足さず、`#ifndef` で上書きできる定数にする）。HMPRE = 0、または HMMIN × 4 KiB > 上限なら使わない。
- amd64 の PCI の DMA は 32 bit で 1 回 16 MiB まで（`src/drivers/pci/pci-pcat.c`:92-96、VMD も同じ）。chunk は min(16 MiB, target) を 2 の冪、下限 max(HMMINDS × 4 KiB, 2 MiB)、数は min(HMMAXD か 256, 16)。`drv_dma_alloc_coherent` が失敗したら chunk を半分にして続け、下限を割るか数の上限で止める。合計が HMMIN 未満なら全部を返して使わない。descriptor の list は 1 page の coherent memory。

### 有効・無効

- 有効（MR=0）: `nvme_probe_namespace` で I/O queue の作成の後、disk を作る前（admin の timeout が admin を止めるので、I/O queue の前には置かない）。
- 再有効化（MR=1、status の誤りなら MR=0 で 1 回）: `nvme_io_recover` の I/O queue の作り直しの直後（recovery の mode）、resume の I/O queue の作り直しの直後。
- 無効と free: 動いている間に EHM=0 は送らない（shutdown・suspend は先に割り込みを mask する、detach の後の admin は ENXIO）。P1 の決定: 明示の EHM=0 を送らず、CC.EN=0 と CSTS.RDY=0 の確認（または bus master を切った確認）を無効の境界とし、free は `nvme_dma_free` の中だけ（lifecycle が disable → master → irq drain → dma_free の順）。順序は chunk、list。shutdown・quarantine では free しない（今の DMA の保ち方と同じ）。suspend の間も保つ。
- 失敗: Set Features が status の誤り → HMB なしで続ける。ETIMEDOUT → 状態 UNKNOWN、disable が確かめられるまで返さない。確保の失敗 → 1 行の log、HMB なしで続ける。
- log は 1 行（`nvme: host memory buffer 64 MiB in 4 chunks`）。HMPRE = 0 は何も出さない。

### 危険

- disable の確認より前・timeout の後・quarantine の時の free。有効な間に CPU が chunk・list を書く事（禁止）。4 GiB より下の 64 MiB が他の 32 bit の DMA と取り合う。失敗した遷移が memory を抱えたまま残る（意図した漏れ）。WS300 の D3cold の経路が resume の再有効化を通らない事。

## 受け入れ

1. host 試験（`plan/ws299/tests/host/nvme-hmb-test.c`）: plan（HMPRE=0、BG5 の値、HMMIN > 上限、HMMAXD・HMMINDS、確保の失敗を挟む縮め方）、CDW10〜15 の bit、descriptor の byte、Identify の offset。
2. QEMU（HMB の無い NVMe）: boot-test、`qemu-nvme-shapes.sh` PASS、log に host memory の行が無い。
3. 5330: dmesg の 1 行と、乱読み 4 KiB の QD1・QD32 が同じ条件の Debian（HMB on）の 95% 以上（ws.md の受け入れ 2）。連続の読み書きが下がらない。
4. build の warning 0（config-5330・pcat・visionfive2・rpi4・m68k-virt）。

## 結果（2026-10-28、P1）

- 実装 28ad3e40e（`pci-nvme.c` の `nvme_hmb_setup`・`nvme_hmb_enable`・`nvme_hmb_reenable`・`nvme_hmb_free`、header の helper）。host 試験 `nvme-hmb-test`・`nvme-prp-test` PASS、config-5330 の build warning 0。
- 5330（`zb7-e-read`、kernel p006a）: `nvme: host memory buffer 64 MiB in 4 chunks`。乱読み 4 KiB QD1 56.5 MB/s（71.9 µs、Debian の HMB on 57.8 の 97.8%）、QD32 424 MB/s（103603 IOPS、Debian 964 の 44%）。QD32 は zedBSD の側の頭打ち: device の post→完了の平均 78 µs で、出ている command は平均 8 本ほど（32 thread の残りは CPU・lock で待つ、CPU 約 42 µs/IO）。HMB の前（`zb7-base-read`）も 445 MB/s で同じ頭打ち。
- QEMU（HMB の無い NVMe）: boot-test・nvme-shapes PASS（log に host memory の行が無い事は未確認）。recovery・resume の再有効化は 5330 で起こしていない（未実施）。
- 残り: QD32 の頭打ちの原因（buffer cache の 1 IO ごとの道、`disk_lock` など大域の lock、NVMe の 1 queue・`command_lock`、VMD の 1 vector）。O_DIRECT（p010）の後に direct でも測る。

## 再開点（2026-10-28、P1）

- 設計・実装・5330 の QD1 は済み。QD32 の 95% は未達。

## 2026-11-13（q2044、P2）: 電力制御の後の測り

- main fe6ea0f967 の kernel、5330（[ws.md](../ws.md) の「2026-11-13」）: dmesg に毎回 `nvme: host memory buffer 64 MiB in 4 chunks`。乱読み 4 KiB direct QD1 58.1 MB/s（Debian の HMB on 55.8 の 104%）、QD32 902.4（930.3 の 97%、thread ごとの open 881.3 = 95%）、cache 経由の QD32 996.4（107%）。WS308 の lock の直しと電力制御の後、受け入れ 3 の乱読みは満たす。連続の読み書きも下がっていない（ws.md の表）。
- 未実施: recovery・resume での再有効化（5330 で起こしていない）、QEMU の log に host memory の行が無い事の確かめ、他の機種の build（今日は x86_64 だけ）。
