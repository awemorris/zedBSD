<!-- awesome-plan project=zedbsd record=ws300p003 -->

# ws300-p003: S0 idle の NVMe の状態（SHN＋D3hot のまま、shutdown の前に HMB を止める）

Phase ID: `ws300-p003`
Parent: [WS300](../ws.md)
Status: cleared（2026-11-15 Q1、merge dbb0415a3f）
Phase disposition: normal
Queue: q2114-i01（P5）。以前: q1683-i01（P6、branch `agent/p6-ws300` の 83b998e5e、merge されず）

## 範囲

[design.md](../design.md) §4.2。推し（Q300-1）: 今の SHN＋D3hot を保つ。shutdown の前に HMB を止める（NVMe Base 2.0e §8.9）。suspend・resume の時間を log。

## 実装（2026-11-15 P5、commit 4966ba2a1a、`src/drivers/pci/pci-nvme.c` だけ）

- `nvme_hmb_disable`（新）: HMB の状態が ENABLED の時だけ、既存の `drv_nvme_set_hmb_command(enable = 0)` で Set Features 0Dh（EHM = 0）を送る。成功で READY（memory は持ったまま、
  resume の `nvme_hmb_reenable` が Memory Return つきで渡し直す）。timeout は UNKNOWN にして ETIMEDOUT を返す（suspend は中止、`nvme_resume_start` の disable と admin の queue の作り直しで戻る）。
  拒否は log「host memory buffer not taken back (N); shutting down with it」で 0 を返し SHN を続ける（SHN の後の disable が controller を reset し HMB の使用を終える）。
- `nvme_suspend_stop` の最初（割り込みの mask の前。admin の完了は割り込みだけで待つ）に呼ぶ。
- log: `nvme: suspended in N ms`、`nvme: resumed epoch=E in N ms`（`clock_ticks` と `kern_ticks_to_ms`）。`plan/ws052/tests/p004-guest.sh` は部分一致なので追従は要らない。
- comment の開発の管理情報（ws052-p004）を 2 か所外した（`nvme_suspend` の頭と `suspended` の欄）。
- branch の p003（P6）を読んで書き直した。他の実装の code は写していない。

## WS299 との重なり（`pci-nvme.c` の変えた所）

`nvme_suspend`・`nvme_resume`（頭の comment、時間）、`nvme_suspend_stop`（頭）、`struct nvme_controller` の `suspended` の comment、前方宣言 1 行、`nvme_hmb_reenable` の直後に `nvme_hmb_disable`。

## 確かめた事

- build（基点 main 5202274642 で warning 0 を先に確かめた）: `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.x86_64.mk build/x86_64/vmunix` rc 0、warning 0。
  `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.vf2.mk build/vf2/vmunix`（riscv64、NVMe を持つ）rc 0、warning 0。arm64（rpi4・rpi5）は NVMe を持たない（`mk/platform-rpi5/defaults.mk`）。
- host: `cc -std=c99 -Wall -Wextra -Werror -I include -o build/tmp/nvme-hmb-test plan/ws299/tests/host/nvme-hmb-test.c && build/tmp/nvme-hmb-test` → `nvme-hmb-test: PASS`（disable の command の組み立てを含む）。
- QEMU: `p004-guest.sh roundtrip` 2 回とも全部 ok（下の「QEMU」）

## 未実施

- 5330（BG5 は HMB を使う）で HMB の停止が受けられるか、suspend・resume の時間（p006、BUG-428・429 の後）。QEMU の nvme は HMB を求めないので、HMB の停止の送りの道は QEMU では通らない。
- 範囲の外の気づき: system の shutdown（`nvme_shutdown`）も HMB を止めずに SHN する（design §4.2）。

## QEMU（2026-11-15 P5）

- image: `sh plan/tools/guest/test-image.sh $PWD/plan/ws300/tests/config-amd64-sleep.mk build/p5-sleep3`（tree 1f22d22af8、p003 と p005 を含む）→ `image rc=0`、`check-x86_64-hdd-image: … (uefi): PASS`。
  WS052 の `config-amd64-sleep.mk` は今の tree で build できない（include が相対 path、OpenSSL の package が `tools/build/check-dynamic-elf.py` の移動で失敗。Q1 へ報告済み）ので、自前の ssh の image に sleepctl を足した写しを使った。
- 試験: `GUEST_RUNTIME=build/p5-guest` で `guest.py start build/p5-sleep3/hdd-image.img` → `wait` → `sh plan/ws052/tests/p004-guest.sh roundtrip build/p5-roundtrip` → `stop`（1 本の scratch の script、各段に timeout）。
  2 回（`build/p5-roundtrip`・`build/p5-roundtrip2`）とも全部 ok: refusals・reconnect・round-trip・old-file・io・usb-keyboard・dmesg の `nvme: suspended`・`nvme: resumed`・`xhci: suspended`・`xhci: resumed`（p004 rc=0）。
  dmesg（SSH で読んだ kernel の log）: `nvme: suspended in 1 ms`、`nvme: resumed epoch=3 in 0 ms`（2 回目は `in 1 ms`・`epoch=2 in 1 ms`）、`system: sleep (devices): result 0 device "" resume 0`。
- 1 回目だけ、起動の root の mount の前に `nvme: I/O queue recovered epoch=2` と約 23 s の待ち（`boot: at 23985 ms: the VFS is initialized`）があった。2 回目は無し（826 ms）。変えた所は I/O の道に触れない
  （probe の parse と、`nvme.apst=on` 無しで何もしない setup）。[BUG-059](../../bugs/BUG-059.md)（QEMU の nvme の遅れで起動中に I/O queue の回復）と同じ形と見るが、基点の kernel での比べはしていない（未確認）。
- QEMU の nvme は HMB を求めない（起動の log に host memory buffer の行が無い）ので、HMB の停止の送りの道は通っていない（ENABLED でない時に何も送らない道を通った）。
