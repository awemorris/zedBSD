<!-- awesome-plan project=zedbsd record=ws300p005 -->

# ws300-p005: NVMe の APST と power state の log

Phase ID: `ws300-p005`
Parent: [WS300](../ws.md)
Status: in-progress（2026-11-15 P5 q2114-i01: APST は main の上で実装、host 試験・build・QEMU の回帰まで。状態を見る口の UAPI は Q300-2 の後で未着手。cleared は Q1）
Phase disposition: normal
Queue: q2114-i01（P5）。以前: q1683-i01（P6、branch `agent/p6-ws300`、merge されず）

## 範囲

[design.md](../design.md) §4.4。PSD の読み、表の組み立て、Set Features 0Ch を probe・resume・recovery の後に。既定では送らない、`nvme.apst=on` の時だけ（review R2）。状態を見る UAPI は足さない（Q300-2）。

## 実装（2026-11-15 P5、commit 1f22d22af8）

- `include/drivers/pci/pci-nvme-protocol.h`（既存の header の inline、新しい .c は無い＝mk を変えない）: `struct drv_nvme_power_state`・`drv_nvme_power_profile`・`drv_nvme_apst_summary`、
  `drv_nvme_power_parse`（NPSS・APSTA・PSD の MP（MXPS で µW に）・NOPS・ENLAT・EXLAT）、`drv_nvme_apst_target`（番号の大きい最初の、非操作・両方の latency が報告され和が許容以下・電力が元の状態以下の状態）、
  `drv_nvme_apst_build`（256 byte の表、ITPT は和が 15000 µs 以下なら 100 ms、他は 2000 ms、要約）、`drv_nvme_apst_command`（Set Features 0Ch、APSTE、PRP1 の表は 1 page の中で dword 揃え）。
  値の出典は block の comment（NVMe Base 1.4c の field、Linux 6.19 の nvme の module の parameter の既定値は値だけ、2 段の idle の時間は Procedure referenced。comment の形を 2fd8d9f591 で guardrail の形に直した、x86_64 vmunix warning 0・host 試験 PASS を再確認）。
- `src/drivers/pci/pci-nvme.c`: probe の Identify Controller の検査の直後に parse。HMB の setup の後に `nvme_apst_setup`（`nvme.apst=on` が無ければ何もしない。APSTA が 0 なら
  「not supported」の log。page を 1 つ取り、非操作の状態ごとに 1 行、`nvme_apst_apply`）。`nvme_apst_apply` は resume（`nvme_resume_steps` の HMB の後）と recovery（HMB の後、`recovery_command` 1）でも送り直す
  （page を取っていなければ何もしない）。移る先が無ければ APSTE = 0。失敗は log だけ。page は `nvme_dma_free` で返す。
- log（probe、`nvme.apst=on` の時）: `nvme: ps3 non-operational, at most 50000 uW, entry 800 us, exit 1000 us`、`nvme: autonomous power state transitions on (5 states, 4 transitions)`、`nvme: ps0 to ps3 after 100 ms idle`。

## WS299 との重なり（`pci-nvme.c` の変えた所）

include 2 行（`kern/boot.h`・`kern/platform.h`）、`NVME_TOKEN_APST_ON` の define、`struct nvme_controller` の `hmb_state` の後に 2 欄、前方宣言 2 行、`nvme_resume_steps` の末尾、recovery の
`nvme_hmb_reenable(controller, 1)` の後、`nvme_dma_free` の identify の後、probe（Identify Controller の検査の後と `nvme_hmb_setup` の後）、`nvme_hmb_disable` の後に 2 関数。
`pci-nvme-protocol.h` は `drv_nvme_hmb_plan` と `drv_nvme_queue_count_result` の間に 1 block。

## 確かめた事

- host: `cc -std=c99 -Wall -Wextra -Werror -I include -o build/tmp/nvme-apst-test plan/ws300/tests/host/nvme-apst-test.c && build/tmp/nvme-apst-test` → `nvme-apst-test: PASS`。
  同じ物を `-fsanitize=address,undefined -fno-sanitize-recover=all` で → `nvme-apst-test: PASS`。BG5 の表（PS0〜PS2 → PS3 100 ms、PS3 → PS4 2000 ms、PS4 は留まる）、許容の境（47899・47900 µs）、
  15000/15001 µs の境、非操作でない・latency 未報告・電力が多い・遅い状態を飛ばす、32 bit を超える latency の和、短い page・短い表、32 状態、APSTA 0、command の組み立てと page を跨ぐ表の拒否。
  header を 6 通りに壊した突然変異（15000 の境、電力の比べ、MXPS の単位、未報告の latency、ITPS の位置、page の境）を全部検出（FAIL 1・3・9・3・4・1）。
- `plan/ws299/tests/host/nvme-hmb-test.c` → PASS（同じ header）。
- build: x86_64（`config/config.x86_64.mk`）・riscv64（`config/config.vf2.mk`）の vmunix が rc 0、warning 0。
- QEMU: `p004-guest.sh roundtrip` 2 回とも全部 ok（下の「QEMU」）

## 未実施

- 5330 で APST が入るか（「transitions on」の log、Get Features 0Ch の読みは口が無い: Q300-2）、遷移の数・滞在（受け入れ 3）、速さの回帰（WS299 の値）。p006。
- 状態を見る口の UAPI（Q300-2）。既定を on にする事（p006 の PASS の後、R2）。

## QEMU（2026-11-15 P5）

- image: `sh plan/tools/guest/test-image.sh $PWD/plan/ws300/tests/config-amd64-sleep.mk build/p5-sleep3`（tree 1f22d22af8、p003 と p005 を含む）→ `image rc=0`、`check-x86_64-hdd-image: … (uefi): PASS`。
  WS052 の `config-amd64-sleep.mk` は今の tree で build できない（include が相対 path、OpenSSL の package が `tools/build/check-dynamic-elf.py` の移動で失敗。Q1 へ報告済み）ので、自前の ssh の image に sleepctl を足した写しを使った。
- 試験: `GUEST_RUNTIME=build/p5-guest` で `guest.py start build/p5-sleep3/hdd-image.img` → `wait` → `sh plan/ws052/tests/p004-guest.sh roundtrip build/p5-roundtrip` → `stop`（1 本の scratch の script、各段に timeout）。
  2 回（`build/p5-roundtrip`・`build/p5-roundtrip2`）とも全部 ok: refusals・reconnect・round-trip・old-file・io・usb-keyboard・dmesg の `nvme: suspended`・`nvme: resumed`・`xhci: suspended`・`xhci: resumed`（p004 rc=0）。
  dmesg（SSH で読んだ kernel の log）: `nvme: suspended in 1 ms`、`nvme: resumed epoch=3 in 0 ms`（2 回目は `in 1 ms`・`epoch=2 in 1 ms`）、`system: sleep (devices): result 0 device "" resume 0`。
- 1 回目だけ、起動の root の mount の前に `nvme: I/O queue recovered epoch=2` と約 23 s の待ち（`boot: at 23985 ms: the VFS is initialized`）があった。2 回目は無し（826 ms）。変えた所は I/O の道に触れない
  （probe の parse と、`nvme.apst=on` 無しで何もしない setup）。[BUG-059](../../bugs/BUG-059.md)（QEMU の nvme の遅れで起動中に I/O queue の回復）と同じ形と見るが、基点の kernel での比べはしていない（未確認）。
- QEMU の nvme は HMB を求めない（起動の log に host memory buffer の行が無い）ので、HMB の停止の送りの道は通っていない（ENABLED でない時に何も送らない道を通った）。
- APST: `nvme.apst=on` を付けた起動は試していない（boot の行を変える image の標準の作り方が無い。QEMU の nvme は APSTA を持たない見込みで、持たなければ「not supported」の道）。この起動は `nvme.apst=on` 無しで、APST の page を取らず何も送らない道を通った。
