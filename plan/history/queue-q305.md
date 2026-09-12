# Queue q305: GPU登録APIの修正

Status: finished
Active Queue: none
Attempt: q305-i01
Phase: ws014-p002 / cleared / normal
Authorization: current user「では、実装を変更してください」。直前に具体化した通常GPU登録APIと共通registryの修正。
Timebox: 120 active minutes
Finished UTC: 2026-09-12T13:31:30.303719+00:00

## q305実行結果（2026-09-12）

q305-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)の修正を完了し、Phaseをcleared、Queue q305をfinishedとする。ユーザー指定どおり、GPUコアは通常の `drv_gpu_register(ops, private_data, **device)` / `drv_gpu_unregister(device)` で個々のdeviceを登録・解除する。GPU公開ヘッダのPCI依存、registration wrapper、専用service table、一括publish APIを除いた。同じ `struct drv_gpu_ops` を共有する複数deviceがそれぞれのprivate dataを持つ。

GPUと共通cdev/devfsの固定台数制限を動的registry・snapshotへ変更し、VFS mount時の登録消去を除いた。早期・追加登録を通常のdevfs経路で扱う。使用中のunregisterはEBUSYでhandle/backendを保持し、解除成功後はhandleを消費する。古いinodeは世代の異なるdeviceや解放済みbackendへ接続しない。既存5 callbackとresource ioctlの責務は維持する。

実GPU/cdev/PCI coreを使う40 GPUの通常・ASan/UBSan試験、ILP32/LP64のUAPI照合、共通cdev/devfsの80 device登録・全件列挙・mount・割当失敗・世代と参照寿命の試験がPASS。共通層の限定runnerは既存GCC -fanalyzer gatesを含めPASS。amd64対象kernel buildとvmunix checkerもPASS。変更箇所の適用コーディング規約全文とlifetime/rollbackをレビューした。最終source hashと手順は `plan/history/queue-q305.md` に保持する。

WS014はincomplete、p001/p003/p004はplanning。QEMU＋Venus・i915は未実行で、新しいactive Queueはない。GPUフレームワーク資料とp001の責務表を現行ops/登録契約へ更新した。ソースと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。GitHubは計画Issue・Projectを同期し、本文・native lifecycle・Projectフィールドを読み戻す。

## 検証証拠

| 実行した確認 | 結果と実際の範囲 |
| --- | --- |
| `plan/ws014/tests/run-gpu-framework-test.sh` | PASS。実gpu.c/cdev.c/pci.c、40 GPU、ops共有/private分離、動的snapshot、追加登録、名前再利用と古いinode、権限、session/handle、割当・copyout失敗rollback、PCI順序と使用中解除。通常＋ASan/UBSan/LeakSanitizer |
| 同runnerのUAPI照合 | PASS。ILP32/LP64サイズ・offset・ioctl encoding |
| `sh plan/ws006/tests/run-dynamic-cdev-devfs-test.sh` | PASS。80 cdev登録、全件lookup/readdir、mount前登録保持、snapshot/ディレクトリ割当失敗、割当中の空registry化、世代の差替えとold fd、並行reset参照寿命。通常＋ASan/UBSan＋既存GCC -fanalyzer全gate |
| `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix` | exit 0。amd64 vmunix check: PASS。config.mk変更なし |
| ソースレビュー | root＋独立agentでlifetime、rollback、ID再利用、snapshot bounds、変更箇所への適用規約全文を確認。empty snapshot契約とコメント/宣言順等を修正後、上記確認を完了 |

既存WS006 runnerの旧host fixture defines/UAPI includesと、testだけに残った旧hal_key_event名を現行kern名へ合わせた。WS006を再開した判断ではない。GPU fixtureの追加GNU11 declaration-after-statement確認もPASS。補助的なstrict C89 compileは既存HAL/atomic/fileヘッダのinlineとhost snprintfで成立せず、対応済みC11 runnerで検証した。HAL実装・ヘッダ変更はない。

QEMU、Venus、ネイティブi915、実GPU描画は検証していない。mmap/submit/fence/display等はp003へ引き継ぐ。q304の過去証拠は上書きせず、現行contractへの合格はこのq305証拠で判断する。

## 検証対象のSHA-256

Base commit: `16077b64937804d9f155e1eaea65c380abb54e6c`。以下は未コミット作業ツリーの検証対象であり、GitHub mainへコードをpushした意味ではない。

| File | SHA-256 |
| --- | --- |
| `include/drivers/gpu.h` | `4b9719e1a5bfc3c0f9c3defe5690e92f2d90211a9b96fe8cbb93b1935a095617` |
| `src/drivers/gpu/gpu.c` | `dc36561c17369fa124a506d74de5d41baed592a6b5431aaba5441da983715420` |
| `include/kern/cdev.h` | `65db2668e9d1c5718fdaf85052efeb976505df5c24f765bf08efcebf09963ae9` |
| `src/kern/cdev.c` | `cd33bf379c0dd735c184e42776adea32f1710ade623c4f054db8f934aea66b52` |
| `src/kern/devfs.c` | `1e42520343c15aa9c9062b2ceb6758fe95306d6af8c7de1be0e4b80638cadfaf` |
| `src/kern/vfs.c` | `3dda68eacfff7c8acc7ddefefa92d291edeea33a95a8700e578113a23f7139fc` |
| `src/drivers/pci/pci.c` | `998250d083fcca13461fc3c628b7b4214fe4ffb12314daf8d6b90bf7c29ecaa3` |
| `include/drivers/pci.h` | `e9ce00c11a77c94a2cd0f80b9e81f5c09fcc780425fec620b8685ab459bb9214` |
| `include/uapi/gpu.h` | `82de065c9ecdff56c93676d1360e1d9666366e983281c7fa60cdd3fec7163051` |
| `plan/ws014/tests/gpu-framework.c` | `a321a959ab64712db40e55fd39c5fd2877d65982364987fdb399a77f79ea4575` |
| `plan/ws014/tests/run-gpu-framework-test.sh` | `baede988792d32be8e38816b36ee3ce50d8d3a2c4c9e9d6808629f622281a598` |
| `plan/ws014/tests/gpu-uapi-layout.c` | `8c91c3bd3d3f7fc22a4526c6fe39237222464d0225ed00779621635af40ce00c` |
| `plan/ws006/tests/dynamic-cdev-devfs-test.c` | `69602d5ee5aec9d058656cc64448c4a09a943755e93faa63f2c119b1ece61f4a` |
| `plan/ws006/tests/run-dynamic-cdev-devfs-test.sh` | `6b647ac913113bb717a6079d4e827f19b589531bdcafcf73b0392713924df0b3` |
| `plan/ws006/tests/input-device-ownership-test.c` | `c6be33901e55268b9cb206b0b55cd7dfdde3df119cee4fbcb4ce1f20f4548569` |
| `build/amd64/vmunix` | `4c2538d768b678b22987422b0e79381767aa0bc6d73feb9f3cbafda20575b8eb` |
