<!-- awesome-plan project=zedbsd record=ws299 -->

# WS299 5330 の NVMe の読み書きを速くする

- Master: [plan/master.md](../master.md)（2026-10-27 作成、q1615 P1 の計画）。Primary Milestone: MG004。Related: MG003（[WS298](../ws298/ws.md) VMD の driver、q1612 NVMe から起動）。
- Status: incomplete（p001・p002・p003・p009 cleared。p004 は Q1 の判定待ち（2026-11-13 に乱読みの後の連続の読みの直しを足した）。p005・p006・p010 in-progress（p006 は受け入れ 3 を満たした）、p007・p008 planned。2026-11-13 q2044（P2）の乖離の確かめと測り直しは下の「今の前提」と「2026-11-13」。95% に未達は direct の読み 4 MiB（92%）・1 MiB（94〜95%）と、p6 の raw の 256 MiB ×4 の形（82%、同じ形なら 104〜117%））
- 出典: 2026-10-27 ユーザー「5330でNVMeの最適化（速度向上）をWSを立ててやってほしいです。（あとで順番にやってほしいです）」「新規WSはまず計画を広く進めて、全体の依存関係を見ましょう。」
- 全体の依存と着手の順: [plan/agents/P1/pm-plan.md](../agents/P1/pm-plan.md)。

- 2026-10-27 09:39 ユーザー（クリック）: 受け入れの速さの数は p001 の測定の後に決める、HMB は p001 で要否を判断する。

- 2026-10-27 10:28 ユーザー（Q1 の受け入れの案 (1)〜(5)、Debian 比 50%・40% などへの回答）「Debianの95%以上をゴールにします。」→ 受け入れ: 連続の読み・書き（raw と UFS の file）と乱読みが、同じ条件の Debian の 95% 以上、他の機種で遅くならない。HMB は「やる」（p006 を残す）。5330 の NVMe の root は、書く時に p5 いっぱいに広げる（ユーザーのクリック）。

## 単一の到達目標

Dell Latitude 5330（Intel VMD 8086:467f の下の KIOXIA BG5 KBG50ZNS256G、DRAM-less、PCIe）で、zedBSD の NVMe の連続の読み書きが、量に依らず一定の速さで、
同じ機械の Debian の測定の 95% 以上に届く（2026-10-27 ユーザー）。今の「512 MiB を超えると 10〜3.6 MB/s に落ちる」崖を無くす。

## 今の前提（2026-11-13、q2044、P2 が main fe6ea0f967 と照らした。下の 2026-10〜11-01 の記録より優先）

- 止まっていた理由: 2026-11-01 の測りの後、CPU の idle・P-state が無い（`sti; hlt` だけ・HWP なし）事が CPU 律速の項目の差の見立てに残り、電力制御の実装待ちになっていた（2026-11-13 ユーザーの見方）。その電力制御は 2026-11-13 に main に入った: [WS333](../ws333/ws.md)（completed。MWAIT の C-state、HWP の strategy、E-core の domain、TCO の watchdog、amd64 の tick を 1000 Hz から 100 Hz に）と [BUG-424](../bugs/BUG-424.md)（PCI の電源の枠組み、VMD の隠れた root port の save・restore）。
- WS299 の code は全部 main にある（ded5ae2e9・36d0d7b5d・1e0ac531a（p004）、28ad3e40e（p006 HMB）、b809c70d1・c9468203a（p010 O_DIRECT と 4 MiB の request）、42d5d4943（libc の memcpy）、114f6d405、3a4e64353・4e7b2a827（q1744 の書き手の CPU）、609295cd0 が main の祖先である事を `git merge-base --is-ancestor` で確かめた）。2026-11-01 の後に WS299 の道を変えた commit は `pci-vmd.c`（BUG-424 の suspend・resume、debec3781b・295eea4623）と `devfs.c` の小さな物だけで、`pci-nvme.c`・`buf.c`・`ufs.c` は変わっていない。
- tick 100 Hz: zedBSD の `clock_gettime` は tick の数え（`kern_clock_gettime`、src/kern/clock.c）なので、刻みが 1 ms から 10 ms になった。測る道具の時間（patwrite・directio の ms、dd の /bin/time の real）は 2 秒以上の測りで ±0.5 % ほどの誤差。randread の `mean_us`・`max_us` は zedBSD では意味が無い（MB/s は全体の時間なので使える）。buffer cache・readahead・writeback の待ちは waitq の起こしで、tick には依らない（buf.c の `BUF_FLUSH_INTERVAL` の周期の flusher だけが tick の刻み）。
- WS300 との関係: BUG-424 の案のとおり WS300 p002（VMD の suspend）は BUG-424 で置き換え、p003（HMB を SHN の前に止める）・p004（ASPM）・p005（APST）は WS300 に残って main の上で作り直す（[BUG-424](../bugs/BUG-424.md)「WS300 との比べ」）。WS299 の範囲（速さ）は変わらない。APST・ASPM は既定 off の案なので、WS299 の測りの条件は WS300 が入っても既定では同じ。
- build: top の Makefile と `mk/` に作り直された。kernel は `make PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y build/x86_64/vmunix`（`llvm-toolchain-cached` → `sysroot` の後）。`make ZEDBSD_CONFIG=… disk-image` と `build/amd64/` は無い。このため `plan/ws299/tests/` の QEMU の image の作り方（`config-qemu-cache128.mk`・`config-qemu-cache32.mk`・`config-5330-cache128.mk` の先頭の使い方、`qemu-suite.sh` の前提の image）は古く、p007 で今の `make CONFIGMK=… disk` の形に直す。`build-randread.sh` は 2026-11-13 に `build/x86_64/sysroot` に直した（main の build/ を読む fallback も外した）。
- 5330 の実機: zedBSD は p4〜p6（p7 は測定用の 32 GiB）。p4 の普段の kernel と `zedbsd.cfg` は Debian の `~/ws245/vmunix-p027-orig`（sha256 先頭 4c98d617279bf96b）・`zedbsd.cfg-p027-orig`（1b33a395b0ff4bd7）で、測りの後はそれに戻す。zedBSD の起動は一回だけ（`~/boot-zedbsd.sh`、UEFI の BootNext）と `watchdog=600`（止まれば TCO の watchdog で Debian に戻る）。S0 idle・sleepctl は使わない（BUG-428・429 の前）。p5 の userland は前の image のままで、kernel だけを差し替えて測る。
- 測りの道具: `plan/ws299/tests/measure-5330.sh NAME VMUNIX`（1 本: lock → Debian の mode → p4 に kernel と watchdog → zedBSD の mode → p4 を戻して sha を照合 → `compare.py` で Debian と並べた表）。
- 記録の直し: p010 の「ユーザーの継続の判断待ち」（2026-10-28）は、2026-10-31 のユーザーの指示（5330 が空いたら担当を問わず進める）で継続が決まっていたので外した。

## 今の事実（2026-10-27 の調べ、根拠は file:行。当時の記録で、今は上の「今の前提」が優先）

- 測定（[ws298 p002](../ws298/ws.md):46-51、kernel f0effe7ad、`/dev/nvme0n1` を dd）: 1・16・64・256 MiB は 0〜1 秒、512 MiB は 5 秒、1024 MiB は 52 秒、
  195 GiB の位置の 256 MiB は 71 秒（約 3.6 MB/s）。遅さは位置ではなく boot からの読みの量に依る。nvme の timeout・回復の行は無い。dd の script は git に無い（`5330-run.sh` の EXTRA）。
- 経路: devfs の `block_pread_data`（`src/kern/devfs.c:1625-1716`）→ `disk_read`（`src/kern/disk.c:2416-2431`）→ buffer cache の `buf_read`・`transfer_run`
  （`src/kern/buf.c:673-731`、`2339-2480`、1 run は 4 KiB の line ×16 = 64 KiB）→ `disk_transfer_direct`（`disk.c:3923-4015`、BIO を 1 つずつ待つ）→ NVMe。
- NVMe の driver（`src/drivers/pci/pci-nvme.c`）: 1 command は 4 KiB（`NVME_IO_BOUNCE_SIZE`、:38、MDTS を読んでも :5128-5130 で 4 KiB に切る）、slot ごとの bounce buffer に
  1 つの PRP（PRP list・SGL 無し）、読みは `kern_memcpy` で写す（:4894-4897）、1 BIO の中で同時に 4 command（:47-52）、I/O queue は 1 本（depth 64）、割り込みは 1 command に 1 回。
- VMD（`src/drivers/pci/pci-vmd.c`）: 下の device に MSI-X の entry を 1 つだけ（`VMD_CHILD_VECTOR`、:80、`host_allocate_irqs` :573-617）、`vmd_interrupt`（:1318-1366）が子の handler を全部呼ぶ。
- cache の大きさ: buffer cache の上限は RAM/8（`buf.c:350-360`、`CONFIG_BUF_CACHE_KIB` は既定 0、5330 は 8 GiB で約 1 GiB）。
- 崖の仮説（p001 で確かめる。数は調べの見積もり）:
  - **H1（最有力）**: buffer の header の slab の確保 `alloc_metadata`（`buf.c:1934-1957`）が slab の list を頭から線形に、`spin_lock_irqsave` の下で歩く。読みの量の 2 乗で増え
    （256→512 MiB で約 4 倍＝1 秒→5 秒に合う）、上限に達した後は 1 line ごとに約 1.7 万 slab を歩く（約 1 ms/4 KiB ＝約 4 MB/s に合う）。割り込みも止める。
  - H2: 上限の後の line ごとの evict と、確保の失敗で 4 KiB・深さ 1 の同期の読み（`buf.c:2383-2389 → 2110`）に落ちる。
  - H3: NVMe の driver の天井（4 KiB・bounce の写し・深さ 4・1 queue・1 vector）。量に依らない上限（約 250 MB/s）で、崖の説明ではない。
  - H4: BG5 は DRAM-less で、zedBSD は HMB（Host Memory Buffer）を渡していない（BG5 は HMB に対応、Linux は 64 MiB を渡す。p001 で確かめた）。遠い LBA の読みに効くが、量に依る遅さは説明しない。
- 数える口: `vfs.bufcache.{max_bytes,current_bytes,stats}`・`vfs.io.stats`（`IO_BUF_SINGLE_MEMORY` など）・`vfs.readahead.stats`（`src/kern/sysctl.c:68-79`）。

## 現在の状態（2026-10-27 10:30、P1）

- p001（測定）: H1（`alloc_metadata` の slab の線形の walk）が崖の原因と確かめた（cache 128 MiB の kernel で崖が消える、sys ≈ real）。H2・H5 は否、H3（driver）が次の天井、H4（HMB）は連続には効かない。[phase001](phase001/phase.md)。cleared（2026-10-28 Q1。数は Debian の 95%、HMB はやる）。
- p002（直し）: header の確保を O(1) の index に。5330 の連続の読み 4 GiB が 320〜384 MB/s で一定（前は上限の後 3.3〜21）、raw の書き 597〜655 MB/s（前 4〜14）。QEMU の回帰も PASS（2026-10-27 q1617、P1）。cleared。
- p009（root を広げる）: `ufs-grow.py` で 5330 の root を 15104 MiB に。大きな file の後の reboot が止まる、もとからある不具合（`vm_object_sync_range_buffer` の 2 乗の探し）を直した。QEMU・5330 で確かめた。cleared（2026-10-28 Q1）。
- 再開点（2026-10-27 ラップアップ）: p004 の設計（[phase004](phase004/phase.md)）の design-reviewer から（範囲は 2026-10-28 Q1 が決定）。測る口で 256 KiB の command は device 約 270 µs・起床 3 µs・CPU 約 160 µs が直列と分かった（重ねる＝先読みが次の手）。

- **2026-10-28 Q1 の判定と範囲の決定**: p001・p009 は cleared。p004 の範囲は P1 の案のとおり「raw の block device の先読み（buffer cache の worker で device の待ちと CPU の仕事を重ねる）」に変える。複数の queue・VMD の vector の配り方は、起床が 3 µs で 1 本の流れに効かないので後へ（p007 で乱読みの QD を測った後に要否を決める）。UFS の file の先読み・書きの合流は p005。HMB（p006）はユーザーの「やる」のとおり残す。
- 次（q1634、P1、2026-10-28 ユーザー「P1は5330で作業します。NVMeの高速化の続きをお願いします。」）: p004 の design-reviewer → 実装 → host・QEMU・5330 → p005 → p006 → p007 → p008。
- p004（2026-10-28 P1、commit ded5ae2e9・36d0d7b5d・1e0ac531a）: raw の block device の先読み（buffer cache の worker 4 本、窓 2 MiB）と、cache が上限の時の line の付け替え・hit の 16 line ずつの写し（cache_lock の取り合いの直し）。5330 の raw の連続の読み 4 GiB が 1.68〜2.24 GB/s で平ら（中央値 1917 MB/s、p003 の 583 の 3.3 倍、Debian の b-chunk 1.1 GB/s・d-chunk 1.4〜1.6 GB/s を越える）、raw の書き 926〜1032 MB/s（回帰なし）。QEMU 4 本 PASS、build 5 config warning 0。D5（付け替え・hit の写し）は design-reviewer（重大なし）を反映し、128 MiB の cache の負荷試験（`qemu-cache-stress.sh`）PASS。review の後の 5330 の raw の書きは 1 GiB の後半で 440〜560 MB/s に落ちた（sys は同じ、device の側の待ちと見るが未確認、p006・p007）。乱読みと `5330-run.sh` の素の起動は未実施（p007）。[phase004](phase004/phase.md)。
- p005（2026-10-28 P1）: UFS の file の読み書きの道を調べた（1 MiB の読みが 64 KiB の同期の request 16 回・写し 4 回、flusher は 4 KiB ずつ、割当は 64 KiB ごと）。測定の追加と案だけ、設計は未着手。[phase005](phase005/phase.md)。
- 再開点（2026-10-28 P1）: p004 は Q1 の判定待ち。p005 は phase005 の「計画の案」の 1（UFS の file の読みの測定、Debian の ext4 の読み）から。p006（HMB）・p007・p008 は未着手。

## 範囲

- 含む: 崖の原因（buffer cache の確保・evict）、NVMe の大きな転送（MDTS・PRP list、bounce の写しを減らす）、queue の深さと数、割り込み（VMD の vector の配り方）、
  連続の読みの readahead・UFS の cluster、書き（連続の書き・fsync）、HMB（p001 で要否）。
- 含まない: NVMe・VMD の電源（APST・ASPM・D3cold は [WS300](../ws300/ws.md)）、S0ix（[WS052](../ws052/ws.md)）、他の機種の NVMe の調整（回帰しない事だけ確かめる）、file system の形式の変更。

## 制約

- HAL の API（`include/hal/hal.h`）を変えるなら差分ごとの事前承認（[Guardrail](../guardrail.md)）。DMA の確保は既存の `drv_dma_buffer` と `pmem` の口で足りる見込み（p001 で確かめる）。
- NVMe・VMD の値と手順は NVMe の仕様（公開）と Intel の資料を優先。Linux（GPL）は値と手順の参照だけ、code は写さず block ごとに Value(s)/Procedure referenced（Guardrail の出典の規則）。
- 試験: host 試験（buffer cache の確保の計算量・NVMe の PRP list の組み立て）と 5330 の実機（`5330-run.sh --nvme-kernel`、`/tmp/i915-hw.lock`、遠隔の電源は無い）。
  QEMU の nvme は回帰だけ（T に依頼）。実機の測定は 1 本の script（配置 → 起動 → 測定 → 回収 → 要約）。console・serial の log で判定しない。
- 共有の file: `pci-nvme.c`・`pci-vmd.c` は WS300 と同じ。同じ担当が WS299 → WS300 の順に持つか、Q1 が file の所有を分ける。`buf.c` は kernel の共通（全機種に効く）。

## 受け入れ（2026-10-27 10:28 ユーザー「Debianの95%以上をゴールにします。」で数を決めた）

1. 5330 で `/dev/nvme0n1` の連続の読み 4 GiB（cache の上限の 4 倍）の速さが、最初の 256 MiB と最後の 256 MiB で大きく違わない（例: 比が 0.8 以上）。
2. 連続の読み・書き（raw と UFS の file）と乱読みが、同じ 5330・同じ条件の Debian の測定（`plan/ws299/tests/bench-debian.sh`）の 95% 以上。他の機種で遅くならない。
3. 崖の原因を数で示す（仮説の確かめの記録）。直した後に `vfs.io.stats` の single の落ち込みが 0 に近い。
4. host 試験 PASS、amd64 と buffer cache を使う全 platform の build warning 0、`boot-test.sh` の回帰（QEMU、T）、5330 の素の起動（`5330-run.sh` PASS）。
5. 規約の全文の見直し。

## Phase の案と見積もり（LW、±50%）

| Phase | 内容 | 依存 | LW | 状態 |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | 調査と設計: 測定の script（5330 の Debian と zedBSD で同じ大きさ・位置、counter を前後で取る）、Debian の基準、H1〜H4 の確かめ（`CONFIG_BUF_CACHE_KIB` を小さくして崖が動くか、`alloc_metadata` の時間）、直しの順と受け入れの数、design-reviewer | WS298（済） | 4 | cleared（2026-10-28 Q1 判定。H1 を確かめ、数はユーザー「Debianの95%以上」、HMB は「やる」） |
| [p002](phase002/phase.md) | buffer cache: header の確保を O(1)（空きの list）にし、irq を止める区間を短く。evict を run 単位に。host 試験で計算量 | p001 | 4 | cleared（2026-10-27 11:41） |
| [p003](phase003/phase.md) | NVMe の大きな転送: MDTS に合わせた command、PRP list（bounce を使わず page に直接、使う所だけ bounce）、BIO の上限を上げる（`d_max_transfer_blocks`） | p001・p002 | 6 | cleared（2026-10-27、Q1 判定。raw の読み中央値 583 MB/s = p002 の後の 1.52 倍、書き 1.5 倍） |
| [p004](phase004/phase.md) | raw の block device の先読み（buffer cache の worker で device の待ちと CPU の仕事を重ねる）。複数の queue・VMD の vector は p007 の乱読みの測定の後に要否（2026-10-28 Q1） | p003 | 6 | in-progress（実装・QEMU・5330 済み、raw の読み 4 GiB 中央値 1917 MB/s。Q1 の判定待ち、q1634。2026-11-13 q2044: 乱読みの後の連続の読みで run が切れる件を直した 35b527f23c（1 GiB 1471 MB/s = Debian の 97%、cksum 一致）、QEMU は未実施） |
| [p005](phase005/phase.md) | 連続の読み書き: UFS の file の readahead の窓と cluster の読み書き、書きの合流（raw の先読みは p004 へ、2026-10-28 Q1） | p002・p004 | 4 | in-progress（raw 書き 119%・raw 読み 150%・UFS 読み 126%、UFS 書き 55% で未達、2026-10-28 P1。2026-10-31 q1744: 書き手の CPU の直し 3a4e64353。2026-11-01 q1788 の 5330: file の書き p7 90%・root の UFS の上書き 108%・新しく 96%・file の読み 112%、file の書き p7 は 95% に未達。2026-11-13 q2044: file の書き p7 99%・読み 117%、root の UFS の上書き 105%・新しく 101%、p6 の raw の書き 1 GiB は同じ形で 104〜117%、256 MiB ×4 の形は 82%（基準の形を直す案を Q1 へ）） |
| [p006](phase006/phase.md) | HMB（DRAM-less の BG5） | p003 | 4 | in-progress（実装・5330 で動作、QD1 98%。QD32 は 44% で未達、2026-10-28 P1。2026-11-13 q2044: QD1 104%、QD32 direct 97%・cache 経由 107%、受け入れ 3 を満たした。cleared は Q1 の判定） |
| [p010](phase010/phase.md) | block device の O_DIRECT（2026-10-28 ユーザー「O_DIRECT を UAPI に足す」、Q1 が p010 に確定、p007 の前） | p004・p005 | 4 | in-progress（2026-10-28 q1657: direct 書き 88%（59% は道具の fill の libc memcpy だった）、読み 1 MiB 103%・4 MiB 91%、QD32 99%。下の「結果」。継続は 2026-10-31 のユーザーの指示で決まった。2026-11-13 q2044: direct の書き 96%、読み 1 MiB 94〜95%・4 MiB 92〜93% が未達、command の大きさ・C-state・pin の region の探しは効かなかった） |
| p007 | 5330 の測定の受け入れと回帰（QEMU の nvme・USB の起動は T、5330 は 1 本の script）。QEMU の image の作り方を今の build system（`make CONFIGMK=… disk`）に直す所から（2026-11-13） | p002〜p006・p010 | 2 | planned |
| p008 | 規約の全文の見直し | p007 | 2 | planned |
| [p009](phase009/phase.md) | 5330 の NVMe の root を p5 いっぱいに（`ufs-grow.py`、UFS の群の探しを O(1)）と、大きな file の sync の 2 乗の直し（reboot が止まった） | p002 | 2 | cleared（2026-10-28 Q1 判定: 受け入れ 3 点を QEMU・5330 で確かめた。xattr・msync の道を狙う試験は未実施のまま） |

合計 約 32 LW（p006 を取り消せば 28）。

## 判断が要る点

- 1 queue を CPU ごとの queue にする時の割り込みの配り方（VMD の vector の数、IRQ の affinity）が HAL の口で足りない時は HAL の差分の案（承認）。

## 2026-10-28 測り直しと継続（q1640、P1）

- 2026-10-28 ユーザー「P1はfstrimを使った上で、SSDの消去の影響をなるべく排除して、再度目標達成まで継続してください。」
- 解釈（Q1）: 測る前に 5330 の Debian で `fstrim -av`、書きの試験の先の p6（zedBSD の swap、1 GiB）だけ `blkdiscard`。p1〜p5 は discard しない。測定の間に休みを置き、同じ条件で Debian の基準も取り直す。
- 2026-10-28 ユーザー「P1は、UFSのパーティションが非常に小さいため、そこへの読み書きでは正確な計測ができないです。32GBくらいの空のパーティションを作って、それに対する読み書きをDebianとzedBSDで比較する必要がありそうです。」→ 約 32 GiB の測定用の区画を作り、raw と file system（Debian は ext4、zedBSD は UFS）の読み書きを同じ区画で比べる。空きが足りず ext4 を縮める（live OS の起動が要る）時はユーザーに確かめる。
- 2026-10-28 ユーザー「P1に伝えてください。USBメモリを挿入しましたので自由に書き換えて起動していいです。ext4を縮小して32GBの空きパーティションを作ってください。容量に余裕があれば、zedBSDのUFSも32GBで作り直すのがいいと思います。今後のI/O性能評価で、16GBだと小さいと感じたからです。」 → Debian の ext4 を offline で縮め、32 GiB の測定用の区画（p7）と、32 GiB の zedBSD の root の UFS を作る（P1、q1640）。


### 進み（2026-10-28、P1、q1640。数は 5330、Debian と同じ条件: fstrim・discard・休み）

- 測定の条件の改訂（[phase005](phase005/phase.md)「測定の条件」）: 以前の読みの位置は trim 済みの空きだった。読みは Debian の ext4 の 12 GiB の乱数の file（`/home/awe/ws299/data.bin`）の範囲、書きと file system は新しい 32 GiB の区画 p7 `zedBSD-nvme-bench`（ユーザーの指示で p2 を縮めて作った、p5 も 32 GiB に移した、[phase005](phase005/phase.md)「測定の区画」）。道具は `randread.c`・`patwrite.c`（両方の OS で同じ）。
- Debian の基準: p7 の raw 4 GiB 書き（page cache＋fsync）1549 MB/s、raw 冷えた読み page cache 1.2 GB/s・direct 2.1 GB/s、ext4 の 4 GiB の file 書き（fsync）1775 MB/s・冷えた読み 1.2 GB/s。data.bin の範囲の raw の連続読み 1.5 GB/s。乱読み 4 KiB: HMB on QD1 57.8 MB/s・QD32 964 MB/s、HMB off 36.4・448。
- zedBSD main の kernel（`zb7-base`・`zb7-basefs`）: raw 書き 1302、raw 読み 1.8 GB/s（buffer cache 経由、Debian の page cache 経由の 150%）、UFS（`mkfs -t ufs --profile=native`、tail journal で write-through・1 block ごとに flush）の 4 GiB 書き 7 MB/s・読み 330 MB/s、乱読み QD1 35.9・QD32 445（HMB off の Debian と同じ）。
- p005d（cluster 書き・背景の書き戻し・UFS の先読み・VM の直読み）: UFS 読み 956 MB/s（sys 4.38 s / 4.49 s で CPU が律速、間接 block の 512 byte の読みが 1 block に約 6 回）、raw 書き 1165。UFS の書きは tail journal の volume では変わらず 7 MB/s（`IO_DRIVER_FLUSH` 728057 回、order_barrier）。
- p006a（HMB）: `nvme: host memory buffer 64 MiB in 4 chunks`。乱読み QD1 56.5 MB/s（Debian 57.8 の 97.8%）、QD32 424 MB/s（Debian 964 の 44%: zedBSD の側が約 10 万 IOPS で頭打ち、CPU 約 42 µs/IO）。
- 実装（WIP commit）: 40cc0c334・de51615b6（p005 D1〜D4）、6e3119ad6（UFS 先読み）、91eaa1c51（VM 直読み）、28ad3e40e（p006 HMB）、bmap の memo（間接の sector を 1 回の読みで 1 度）、114f6d405（raw の block device の書きを遅らせる: 開いた者だけが使う範囲は cache に dirty で残し、cluster・worker・fsync・最後の close で書く）。
- ユーザーの決定（2026-10-28、Q1 経由）: 「O_DIRECT を UAPI に足す」（block device、Linux と同じ意味）。設計は kernel-and-driver-designer に依頼中、design-reviewer を通す。
- 残り: UFS の書き（root と同じ batched journal の volume を p7 に置いて測る `p7ufswrite`、tail journal の volume の flush の多さは mkfs の既定の問題として Q1 へ）、UFS の読みの CPU（bmap の memo の効果）、raw の書き（遅らせる書きの効果）、乱読み QD32 の頭打ち（buffer cache・NVMe の 1 queue・VMD の 1 vector のどれか、O_DIRECT の後に direct で測る）、O_DIRECT、p007（QEMU・5330 の回帰、`5330-run.sh` の素の起動）、p008。

### 再開点（2026-10-28、P1 のラップアップ、q1640）

- 最後の commit: d847e644f（agent/p1-ws299b、基点 b1add2a6c）。QEMU の回帰 6 本 PASS は d173b958e の tree（d847e644f はそこから UFS の allocation run の定数を戻しただけ、build warning 0）。
- 5330 の状態: p2 149.8 GiB、p7 `zedBSD-nvme-bench` 32 GiB、p5 32 GiB（zedBSD の root、`5330-run.sh --nvme-image` で起動 PASS）。p4 の kernel は `build/p1-p005f/vmunix`（d173b958e の 1 MiB の run 入り。次の測定は自分の kernel を入れる）。p7 は root と同じ形の UFS（`bench-debian.sh p7ufs`）が載っている。USB の stick は SystemRescue。
- 受け入れ（Debian の 95%）の今: 達＝raw の連続の書き（1837/1549）・読み（cache 経由 1.80/1.2 GB/s）、UFS の file の読み（1518/1200）、乱読み QD1（56.5/57.8）。未達＝UFS の file の書き（983/1775、55%）、乱読み QD32（424/964、44%）、direct の読み書き（O_DIRECT が未実装）。
- 次の順（推し）: (1) p010 O_DIRECT の実装（[phase010](phase010/phase.md) の設計と review の反映方針どおり、a 正しさ → b 速さ）。(2) 乱読み QD32 の頭打ち（device の待ち 78 µs で出ている command が平均 8 本、CPU 42 µs/IO。`randread.c` に `fdeach`（thread ごとに open）を足してある: 共有の fd の lock か、buffer cache・`disk_lock`・NVMe の `command_lock`・VMD の 1 vector かを切り分ける。O_DIRECT の後は direct でも測る）。(3) UFS の書きの CPU（書き手 1 本が律速、allocation run ごとの CG・inode の更新と metadata の buf の読み書き。run を 1 MiB にしても効かなかった）。(4) p007（QEMU の回帰 `build/tmp/qemu-suite.sh` の形を `plan/ws299/tests/` に移す、`5330-run.sh` の素の起動、他機種）。(5) p008 規約（source の `ws299` などの開発の管理情報、`buf: readahead` と nvme の測る口の log を消すか CONFIG にするか）。
- 片付け（Q1 に頼む）: 5330 の `/home/awe/ws299/data.bin`（12 GiB）は乱読みと data の範囲の読みにまだ使うので残す。build/tmp/src-73135a9ec（73135a9ec の kernel の build の一時物）、build/tmp/qemu-* の古い run は消してよい。
- 2026-10-28 5330 が O_DIRECT の kernel（b809c70d1）で p7direct の測定の後に固まった。ユーザーが入れ直し、観察（原文）「入力受け付けない状態でデスクトップが表示されていました。割り込みを受け付けられない状態になったとか、カーネルがpanicしたとかだと思いました。」→ P1 が O_DIRECT の道を QEMU で調べる。
- 2026-10-28 ユーザー「P1にお伝えください。フリーズは再現したときに対策を考え直します。仮想メモリのロックの変更は、P1の担当にして、一時的にそれに対応する別なWSを処理して、そのあとでNVMeのWSに戻りましょう。P1は、Fable 5.1 Highのサブエージェントに助言を求めてOKです。状況を共有して、検討の進め方を助言してもらってください。」 → q1646 は paused、WS308（VM の lock）の後に戻る。5330 の固まりは再現した時に考え直す。

### 再開点（2026-10-28、P1、q1646 paused）

- p010（O_DIRECT）: 実装 b809c70d1（UAPI `O_DIRECT 0x100000`、open・F_SETFL・`/dev/fd`、syscall の 4 経路、devfs `block_pdirect`、`buf_direct_prepare`）。host・build 5 config warning 0・QEMU（`qemu-directio.sh`、boot-test、nvme-shapes）PASS。5330: direct の読み 95〜101%、書き 59%、乱読み direct QD1 99〜100%・QD32 48%。詳細と表は [phase010](phase010/phase.md)。
- 2026-10-28 ユーザー「仮想メモリのロックの変更は、P1の担当にして、一時的にそれに対応する別なWSを処理して、そのあとでNVMeのWSに戻りましょう。」→ q1646 は paused、[WS308](../ws308/ws.md)（VM の metadata lock、QD32 の頭打ちの有力な原因）の後に戻る。
- 5330 の停止（zb-d2 の後の reboot で desktop のまま固まった）: 2026-10-28 ユーザー「フリーズは再現したときに対策を考え直します。」原因は不明、QEMU の繰り返しでは再現せず（2 回正常）。
- 戻った時の順: direct の書き 59% の切り分け（Fable 5.1 High に助言を求めてよい、2026-10-28 ユーザー）→ QD32 の測り直し → UFS の file の書き（55%）→ mkfs native の write-through（2026-10-28 ユーザー、UFS の高速化の後、要れば先に。2026-10-29 Q1: [BUG-355](../bugs/BUG-355.md)・[BUG-358](../bugs/BUG-358.md) の q1688（P4、ユーザー「P4は、先にUFSが遅いバグを修正してください。」）で mkfs の native の tail journal ごと扱う）→ p007 → p008。
- 2026-10-28 18:50 P1（WS308 の後に q1646 を再開）: 乱読み QD32 は WS308 で Debian の 99.3%（direct）・cached 241k になった（[phase010](phase010/phase.md) の「再開」）。direct の書き 59% の切り分けは、基準の取り直しで 5330 の zedBSD が起動の後に答えなくなり止まった（電源の入れ直し待ち）。次は 5330 が戻ってから、p7direct の zedBSD と Debian の取り直し → bs 4 MiB と command の大きさで比べる → UFS の file の書き（55%）→ mkfs native の write-through → p007 → p008。

## 2026-10-28 夜（q1657、P1）

- 承認（ユーザー、2026-10-28）:「P1はfstrimを使った上で、SSDの消去の影響をなるべく排除して、再度目標達成まで継続してください。」「P1は自律的にゴールに向かわせてください。」「…両方でLinuxと同等以上の性能にチューニングしたいです。」「P1は、現在原因がわかっていないパフォーマンスの問題については、Fable 5.1 Highに何度でも助言を求めてOKです。」「さきほどの「mkfs で作った native の UFS が write-through になり、4 GiB の書きが 7 MB/s しか出ない件」は、P1に依頼します。UFS読み書きの高速化のあとに取り組んでもらいます。」「もしUFS読み書きの高速化にwrite-throughに修正が必要なら、先に取り組んだり同時に取り組んだりしてもOKです」
- 2026-10-28 ユーザー（Q1 経由）「P1はmemcpyを改善して計測し直した後、全体の比較レポートを送ってほしいです。その上で継続判断させてください。」→ Q1: q1657 の範囲に libc の memcpy（`src/libc/string.c`、案 (a): 小さい長さは重なってよい load/store、内部の 2 段の call を無くす、大きい長さは ERMS の時 rep movsb）を足す。build の flag（案 b）と memset はしない。全長さ×alignment の host 試験、memcpy の micro bench と WS299 の全体の測り直し（zedBSD と Debian を同じ時間帯）、1 つの report を Q1 に送って止まる（UFS の書き・mkfs には進まない）。

### 結果（2026-10-28 21:50、P1、q1657）

- 5330 の起動: 18:46 に止まった kernel（w308c-z）で `5330-run.sh --nvme-none p1-q1657-boot1` PASS（ssh まで 21 s）。新しい image（c9468203a＋memcpy、`build/p1-q57img`）を `--nvme-image p1-q1657-img` で p5 に書いて PASS。この夜の 20 回近い zedBSD の起動で止まりは再現しなかった。
- direct の書き 59% の切り分け: 測る口（一時の patch、`plan/ws299/temp/`、commit しない）で、1 MiB の direct の書きの kernel の時間は約 0.47 ms（BIO 0.46、pin 0.02、prepare・finish 0.005）。4 GiB の I/O の時間は zedBSD 2.05〜2.13 s、Debian 1.96〜2.24 s で同じ程度。差は道具 directio の fill の側にあった: 長さを変数にした memcpy を 8 byte ごとに呼んでいて、clang はそれを call memcpy のまま残し（gcc は inline の mov にする）、zedBSD の libc の memcpy（1 回 3.2 ns、Debian 0.9 ns）が 1 MiB あたり 0.5〜1.4 ms かかっていた。→ fill を定数 8 byte の写しに直した（書く byte は同じ）。directio に TSC での内訳（fill・io・sync）、CPU の移り、P-core の数を足した。
- NVMe: 1 request を 4 MiB（`NVME_IO_MAX_BIO_BYTES`）、同時の command を 16（256 KiB×16）、direct の piece を 4 MiB（staging は 1 MiB のまま）に（c9468203a）。比べた形（128 KiB×8、128 KiB×32、256 KiB×16）では書きの I/O の時間は変わらず、4 MiB の direct の読みが 2578 → 2809〜2867 MB/s。QEMU: qemu-directio PASS、qemu-nvme-shapes PASS、boot-test PASS（`build/tmp/boot-test-q57/login.png`）。build: config-5330・pcat・visionfive2・rpi4・m68k-virt の kernel で warning 0。host: buf-direct-test PASS。
- libc の memcpy（`src/libc/string.c`、amd64 だけ、42d5d4943）: 16 byte 以下は重なってよい head と tail、64 byte 以下は 16 byte の head と tail、それより長い物は 32 byte の loop と最後の 32 byte、2048 byte 以上で ERMS があれば rep movsb。host 試験 `plan/ws299/tests/host/memcpy-test.sh`（長さ 0〜1100 の全部と境界の長さ × src・dst のずれ 0〜15、286720 件、guard の byte、rep movsb あり・なしの両方）PASS。image の build で libc は warning 0（image の warning 476 件は openssh の package と perl の locale）。
- memcpy の micro bench（5330、`cpuloop`、P-core の上、ns/回。同じ起動で新・旧の libc を静的に link した物を並べた。zedBSD の時計は 1 ms 刻みで ±10% ほどぶれる）: 8 B 旧 3.7〜4.1 → 新 1.07（glibc 0.91）、64 B 3.1〜3.2 → 1.31（1.02）、1 KiB 21〜23 → 21（7.2）、4 KiB 69 → 46（29）、64 KiB 1587 → 1465（1055）、1 MiB 52.7 µs → 50.8 µs（36.2 µs）、32 MiB 9.5 → 9.4 GB/s（18.4 GB/s）。
- 1 thread の CPU の速さ（`cpuloop chain`）: Debian 725〜730 Msteps/s（P-core）、zedBSD 480〜660（P-core でも 500〜660、E-core に移ると 470〜570）。zedBSD の amd64 の idle は `sti; hlt` だけ（C1）で、HWP も無い。
- UFS の file の書きの profile（tick ごとの kernel の PC、一時の patch）: 書き手の CPU の大半は buffer cache の 4 KiB の line ごとの処理（reference_line・evict_one・buf_release・buf_read・mark_dirty・finish_run_line・write_range など、1 line あたり約 2.4 µs）と copyin_pinned。

| 項目（5330、4 GiB など） | Debian（q57-D、同じ夜） | zedBSD（q57-Z、c9468203a＋memcpy） | 比 | 前回の比（q1640・q1646） |
| --- | --- | --- | --- | --- |
| raw の書き p7（page cache・fsync） | 1934 MB/s | 1752 | 91% | 119%（Debian 1549） |
| raw の冷えた読み p7（cache 経由） | 1.3 GB/s | 1812 MB/s | 139% | 150% |
| raw の direct の読み p7（dd、Debian だけ） | 2.2 GB/s | — | — | — |
| file の書き p7（ext4 / root と同じ UFS、fsync） | 1917 | 988 | 52% | 55% |
| file の冷えた読み p7 | 1.3 GB/s | 1512 MB/s | 116% | 126% |
| direct の書き 1 MiB | 1579 | 1384 | 88% | 59%（道具の fill を含む） |
| direct の読み 1 MiB | 2486 | 2554 | 103% | 95% |
| direct の読み 4 MiB | 3101 | 2809 | 91% | 85% |
| data 範囲の direct の読み 1 MiB | 1526 | 1554 | 102% | 101% |
| 乱読み 4 KiB direct QD1（notime） | 55.8 | 57.3 | 103% | 99% |
| 乱読み 4 KiB direct QD32（notime） | 923 | 917 | 99% | 48%（WS308 の前） |
| 乱読み 4 KiB direct QD32 procs（notime） | 919 | 919 | 100% | — |
| 乱読み 4 KiB QD32（時計入りの randread、cache） | 930（hmb1） | 491〜506 | 53% | 44% |
| data 範囲の raw の連続の読み（read mode の chunk2〜15、dd 1 MiB） | 1.5 GB/s | 1.68〜2.24 GB/s | 112〜149% | — |
| 同じ、最初の 900 MiB（bs64k・bs1m・bs4m・chunk0・chunk1） | 1.5 GB/s | 320〜655 MB/s | 21〜44% | 同じ形（前から） |
| root の UFS の 1 GiB 上書き（ufsover）/ 新しく（ufsnew） | ext4 1.6 GB/s | 1309 / 985 | 82% / 62% | — |
| p6 の raw の書き 1 GiB（rawtotal） | 1.7 GB/s | 1309 | 77% | — |

- 残る差の見立て（未確認の物を含む）: (1) CPU の速さ（idle が C1 だけ・HWP なし・hybrid を知らない置き方）で、書き手の CPU が律速の項目（UFS の書き・direct の書きの fill）は 10〜30% 遅くなる。所有の外（HAL の idle・scheduler）。(2) UFS の書きは buffer cache の line ごとの費用。(3) 時計入りの randread は clock_gettime が syscall（vDSO が無く 1 ms 刻み）のため。(4) read mode の最初の約 900 MiB が遅いのは、乱読みが残した 4 KiB の line を追い出す道が遅いためと見ている（cache は乱読みの後に既に満杯、chunk2 から速い。未確認）。(5) memcpy は 1 KiB〜4 KiB（dst を揃えていない、AVX なし）と 32 MiB（non-temporal の store なし）で glibc より遅い。
- 再開点: ユーザーの継続の判断待ち（2026-10-28 ユーザー「その上で継続判断させてください。」）。証拠は worktree の `build/hw5330/ws299d/`（切り分け）・`build/hw5330/ws299e/`（全体）、git の外。

## 2026-10-31（q1744、P1）

- 2026-10-31 ユーザーから、WS299 は 5330 が空いたらどの担当でもよいので割り当てる、また不在の間は設計・実装・試験を進められる所まで進める、という趣旨の指示を受けた（Q1 経由）。
- p004 の commit（ded5ae2e9・36d0d7b5d・1e0ac531a）は main に入っている（Q1 の判定待ちのまま）。
- 5330: main 9946e22c0 の kernel を WS291 の restore の後の古い userland に入れた起動で network が上がらず止まった（電源の入れ直し待ち、遠隔の電源は無い）。次は main の userland の image（/tmp/p1-ws299/img5330、config-5330.mk）を `--nvme-image` で書いてから測る。
- 書き手の CPU の直し（3a4e64353、[phase005](phase005/phase.md) の「2026-10-31」）: LRU の歩きで dirty の line を head へ回す、P6 の束の書きの取り込み、UFS の allocation run を 1 MiB・free map の探しを前の run の終わりから、`kern_memcpy_ram`（amd64 は rep movsb）。QEMU の 2 GiB で UFS の新しい file 490 → 851 MB/s、上書き 410 → 852、raw 743 → 835（同じ probe 入りの kernel どうし）。最終の kernel（4e7b2a827、レビューの重大 1 件 = tail journal の volume の run の大きさを直した後）で QEMU の suite・crash 試験 PASS（disk は tmpfs。host の disk に置くと readahead は main も NVMe の queue の recovery で FAIL）。他機種の build warning 0。
- 再開点（2026-10-31 05:30、P1 返却）: 5330 の電源が戻ったら、worktree p1-ws299e で `sh plan/ws299/temp/run-5330-q1744.sh q1744`（git の外の 1 本の script: (1) `/tmp/p1-ws299/img5330-final.img`（config-5330.mk の main の userland＋kernel 4e7b2a827、vmunix の sha256 先頭 a40a721cd0d2b3f7）を `5330-run.sh --nvme-image` で書いて素の起動、(2) `nvme-bench-5330.sh debian … read write p7raw p7fs p7direct`、(3) `nvme-bench-5330.sh zedbsd … - read write p7rawwrite p7rawread p7fswrite p7fsread p7direct`、log は build/hw5330/q1744）。image が /tmp から消えていたら `make ZEDBSD_CONFIG=plan/tools/hw5330/config-5330.mk BUILD=/tmp/... disk-image` で作り直す。未達が残れば `build/kp-5prof/vmunix`（prof.patch 入り）と `plan/ws299/temp/zb-prof.sh` で 5330 の profile。その後 p007 → p008（測る口の log・規約）。

## 2026-11-01（q1788、P1）: 5330 の測り（q1744 の続き）

- 条件: main 82c6ef325（q1744 の書き手の CPU の直しと、その後の main の変更を含む）を `make ZEDBSD_CONFIG=plan/tools/hw5330/config-5330.mk BUILD=build/q1788/img disk-image`（rc 0、kernel の warning 0、vmunix の sha256 先頭 92abccaa73c063e3）で作り直し、`plan/ws299/temp/run-5330-q1788.sh`（q1744 の script の写し、git の外）で `5330-run.sh --nvme-image`（PASS、`build/hw5330/q1788/p1-q1788-img.png`）→ `nvme-bench-5330.sh debian q1788-D read write p7raw p7fs p7direct` → `nvme-bench-5330.sh zedbsd q1788-Z - read write p7rawwrite p7rawread p7fswrite p7fsread p7direct`。log は worktree の `build/hw5330/q1788/`（git の外）。Debian と zedBSD は同じ夜（2026-11-01 01:20〜02:17）。
- 道具の直し（2 件）: (1) `nvme-bench-5330.sh` の zedBSD の account が待ちの loop（pipeline の subshell）の中でだけ決まり、本体の shell の `ZBU` が空のまま `echo $ZBU | sudo -S` に空の password を渡していた（今回 sudo が「authentication failed」、前の run は sudo の credential の cache で通っていた見込み）。reboot の行も single quote で remote の空の `$ZBU` を読んでいた。待ちの後に本体の shell で account を選び、reboot の行は local で展開する。(2) `bench-zedbsd.sh` の p7fswrite は mkfs の registration が読めない時に mkfs の出力を出す。
- p7fswrite が script の中で 2 回「mkfs failed: no registration」で止まった（p7 の discard → 起動 → 30 秒の後）。同じ手順を手で（起動の約 2 分後）流すと mkfs は registration を出して通り、script の 3 回目（q1788-Z4、直し (2) の後）も通った。原因は未確認（起動の直後に何かが p7 を開いている、などの見込み、確かめていない）。表の file の書き・読みは q1788-Z4 の数。

| 項目（5330、p7 は 4 GiB） | Debian（q1788-D） | zedBSD（q1788-Z・Z4） | 比 | 前の比（q57、2026-10-28） |
| --- | --- | --- | --- | --- |
| raw の書き p7（fsync まで） | 1899 MB/s | 1895 | 100% | 91% |
| raw の冷えた読み p7（cache 経由） | 1266 MB/s（3.39 s） | 1782 | 141% | 139% |
| file の書き p7（ext4 / UFS、fsync まで） | 1925 | 1724（sync の前 1918） | **90%** | 52% |
| file の冷えた読み p7 | 1250（3.43 s） | 1399 | 112% | 116% |
| direct の書き 1 MiB | 1586 | 1524 | 96% | 88% |
| direct の読み 1 MiB / 4 MiB | 2574 / 3055 | 2611 / 2841 | 101% / 93% | 103% / 91% |
| data 範囲の direct の読み 1 MiB | 1478 | 1525 | 103% | 102% |
| 乱読み 4 KiB direct QD1 | 55.8 | 58.2 | 104% | 103% |
| 乱読み 4 KiB direct QD32（時計入りの randread） | 928.5 | 767.4（fd を毎回開く形 516.2） | 83% | 99%（前は notime の形、同じ形ではない） |
| 乱読み 4 KiB QD32（cache 経由、時計入り） | 928.5 | 759.0 | 82% | 53% |
| root の UFS の 1 GiB 上書き（ufsover）/ 新しく（ufsnew） | ext4 1628 MB/s（ext4-w1m 0.66 s） | 1760 / 1556 | 108% / 96% | 82% / 62% |
| p6 の raw の書き 1 GiB（rawtotal） | 1.6 GB/s（b-w1m） | 1377 | 84% | 77% |
| data 範囲の raw の連続の読み（read mode の chunk2〜15） | 1.3〜1.6 GB/s | 1917〜2440 | 120% 以上 | 112〜149% |
| 同じ、最初の 900 MiB（bs64k・bs1m・bs4m・chunk0・chunk1） | 0.6〜1.7 GB/s | 320〜671 | 22〜53% | 21〜44% |

- 見立て: q1744 の書き手の CPU の直しで file の書きは 52% → 90%、root の UFS の書きは 82%/62% → 108%/96%。95% に届かない項目は file の書き p7（90%）、p6 の raw の書き（84%）、乱読み QD32（82〜83%）、read mode の最初の 900 MiB（前と同じ形）。CPU の idle・P-state（WS333）が入る前の測り（HWP なし）。乱読み QD32 の比が前（99%）より下がった理由は未確認（前の数は notime の形で、同じ道具の形どうしの比べではない）。
- 再開点: p007（受け入れと回帰）の前に、残る 4 項目のどれを追うかを Q1 が決める。WS333 の HWP・C-state が main に入った後にもう 1 度測ると、CPU が律速の項目の差が分かる。（2026-11-13 に測り直した、下の「2026-11-13」）

## 2026-11-13（q2044、P2）: 電力制御の後の測り直し

- 条件: main fe6ea0f967 の kernel（x86_64、`CONFIG_KERN_THERMAL=y`・`CONFIG_DRIVER_X86_POWER_CONTROL=y`、build rc 0・warning 0、sha256 先頭 87db0203ada98f6d）を p4 に、`zedbsd.cfg` は元の物に `watchdog=600` を足して（TCO の watchdog の armed を毎回の dmesg で確かめた）。p5 の userland は d00e071（2026-11-02）の image のまま。道具は `plan/ws299/tests/measure-5330.sh`（Debian の mode → zedBSD の mode を `~/boot-zedbsd.sh` の一回だけの起動で → p4 を戻して sha256 を照合 → `compare.py`）。Debian（q2044a-D）は 11:45〜11:59、zedBSD（q2044a-Z）は 11:59〜12:23。log は worktree の `build/hw5330/ws299/`（git の外）。毎回の終わりに p4 を vmunix 4c98d617279bf96b・zedbsd.cfg 1b33a395b0ff4bd7 に戻し、sha256 で確かめた。
- 時計の刻み: 100 Hz で zedBSD の dd の時間（/bin/time の real）は 10 ms 刻み。256 MiB の段（0.1〜0.2 秒）は ±5〜8 % ぶれるので、curve は中央値で見る。4 GiB の項目は ±0.5 %。

| 項目（5330） | Debian | zedBSD | 比 | 11-01 の比 |
| --- | --- | --- | --- | --- |
| raw の書き p7 4 GiB（fsync まで） | 1346 MB/s | 1804 | 134% | 100% |
| raw の冷えた読み p7 4 GiB（cache 経由） | 1206 | 1775 | 147% | 141% |
| file の書き p7 4 GiB（ext4 / UFS、fsync まで） | 1798 | 1774 | **99%** | 90% |
| file の冷えた読み p7 4 GiB | 1217 | 1427 | 117% | 112% |
| direct の書き 1 MiB | 1575 | 1512 | 96% | 96% |
| direct の読み 1 MiB | 2750 | 2603 | 95% | 101% |
| direct の読み 4 MiB | 3112 | 2863 | **92%** | 93% |
| data 範囲の direct の読み 1 MiB | 1586 | 1562 | 98% | 103% |
| 乱読み 4 KiB direct QD1 | 55.8 | 58.1 | 104% | 104% |
| 乱読み 4 KiB direct QD32 | 930.3 | 902.4（thread ごとの open 881.3） | 97%（95%） | 83% |
| 乱読み 4 KiB QD32（zedBSD は cache 経由、Debian は direct） | 930.3 | 996.4 | 107% | 82% |
| root の file 1 GiB の上書き / 新しく（ext4 / UFS、sync まで） | 1619 | 1704 / 1627 | 105% / 101% | 108% / 96% |
| p6 の raw の書き 1 GiB（zedBSD は 256 MiB の dd 4 回、各 close で書き切る） | 1690（dd 1 回 conv=fsync） | 1395 | **82%** | 84% |
| p6 の raw の書き 1 GiB（両方 1 回の dd と sync、q2044c） | 1505（同じ時間帯）、1690（q2044a） | 1760 | 117%（104%） | — |
| data 範囲の raw の連続の読み chunk2〜15 の中央値（cache 経由） | 1511 | 2339 | 155% | 120% 以上 |
| 同じ範囲、乱読みの後の最初の 900 MiB（bs 64 KiB・1 MiB・4 MiB、chunk0・1） | 1490〜1543 | 327〜671 | **22〜44%** | 22〜53% |
| 同じ、乱読みの前（seqcold、新しい mode）の bs と chunk0〜15 の中央値 | 1490〜1543 / 1511 | 1491〜1917 / 1790 | 100〜124% / 118% | — |

- 見立て: 電力制御（HWP・C-state）の後、CPU が律速だった項目が上がった（file の書き p7 90 → 99%、乱読み QD32 82〜83 → 97〜107%）。95% に届かないのは 3 つ。
  1. **乱読みの後の連続の読み**: 測りの形の違い（Debian は b- の段の前に page cache を捨てる、zedBSD の read mode は乱読みの 4 KiB の line で満ちた cache のまま読む）が主。同じ形（seqcold）では 100〜124%。ただし zedBSD の遅さそのものは本物で、cache に散った line で readahead の run が切れ、command が平均 37 KiB（bs の段で drv_rd 10545 / 384 MiB、乱読みの前は 256 KiB）になっていた。→ readahead の run が cache にある line を 1 page の bucket に読み飛ばして run を切らない直し（35b527f23c、[phase004](phase004/phase.md) の「2026-11-13」）。kernel ea880d69a7b53468（q2044b）: 乱読み QD32 の直後の 1 GiB の連続の読み 1471 MB/s（Debian の cache を捨てた後の 1511 の 97%）、その 1 GiB の cksum は Debian で読んだ同じ block と一致。read mode の bs・chunk0・1 は 1491〜2684。
  2. **p6 の raw の 256 MiB ×4**: zedBSD は dd ごとの close で dirty を書き切る形、Debian の基準は 1 回の dd conv=fsync で形が違う。同じ形（rawone、新しい mode）では 104〜117%。256 MiB ごとの遅さは、dirty が上限（cache の 1/4 ≈ 246 MiB）の半分に達するまで device が書き始めない（約 65 ms）のと、close の書き切りの待ちと見る（未確認）。p005 の受け入れ（各 256 MiB を Debian の direct の d-w の中央値と比べる）は、buffered と direct の比べで同じ条件ではない。基準を同じ形（rawone 対 b-w1m）に直す案を Q1 に出した。
  3. **direct の読み 4 MiB（92〜93%）・1 MiB（94〜95%）**: 差は 1 MiB あたり約 25 µs。試して効かなかった物: command を 128 KiB×32 にする（q2044c、4 MiB 2807）、ordinary idle を HLT だけにする（`IDLE_WAIT_DEEPEST_CSTATE=1`、q2044d、2826）、pin の region の線形の探しを page ごとにしない（q2044e、2826、変更は戻した）。残る候補は 1 request の 16 command の完了の割り込み（VMD の 1 vector）と起床、request の間の隙間。未確認。
- 範囲の外の気づき（Q1 へ）: tick 100 Hz で `clock_gettime` が 10 ms 刻みになり、計測の道具（dd の real、randread の時間）の分解能が落ちた。vDSO の無い `clock_gettime` は master.md の Bug 化の候補にある物と同じ根。

### 再開点（2026-11-13、P2、q2044）

- 最後の commit: 下の q2044 の返却の SHA（agent/p2-ws299）。kernel の変更は buf.c の bucket の読み飛ばしだけ（35b527f23c）。
- 5330: p4 は vmunix 4c98d617279bf96b・zedbsd.cfg 1b33a395b0ff4bd7 に戻してある（sha256 で照合）。p7 は p7direct の後の中身、p6 は zedBSD の swap の頭を戻した状態。
- 次: (1) direct の読み 4 MiB の残りの差（1 request の 16 command の完了と起床、request の間の隙間を TSC で測る）、(2) p005 の raw の 256 MiB の受け入れの形の判断（Q1）と、要れば dirty の上限の半分より前に連続の書き手の cluster を書き始める直し、(3) p007: QEMU の image の作り方を今の build system に直して回帰（bucket の直しは QEMU で未試験）、他機種の build、`5330-run.sh` の素の起動、(4) p008。

## 日程（2026-10-30 ユーザー、原文）

「WS299はRealtek 2.5Gのあとにスケジュールします。」→ WS315 の completed の後（WS315 は 2026-10-30 に completed。2026-11-13 に q2044 で再開）
