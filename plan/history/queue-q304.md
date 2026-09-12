# q304: GPUフレームワーク実装

Queue: finished / Attempt: q304-i01 cleared / Phase: ws014-p002 cleared

ユーザーがp002をQueueへ入れ、GitHub同期後の実行を指示した。続けてコーディングスタイル厳守の指示を受けた。120 active minutesの単一Phase。開始前にQueue/Phase/WS/Master/Past LogとProjectを公開・読み戻しし、その後コード実装を開始した。

## q304実行結果（2026-09-12）

q304-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)をclearedとし、単一PhaseのQueue q304をfinishedにした。GPUフレームワーク、5 callbackのstruct drv_gpu_interface、PCI所有のservice公開・解除、/dev/gpuN、session/世代handleを実装した。

実gpu.c/cdev.cと最小backendの通常・ASan/UBSanテスト、ILP32/LP64の固定ABI照合、実pci.cのlifecycleテスト、amd64対象kernel buildがPASS。全文規約レビューで目的コメント・参照寿命を確認し、git diff --checkもPASS。clang-format 19.1.7のdry-runは全文規約と衝突する関数定義/forward declaration整形等を指摘したため非zeroで、機械的整形は適用していない。詳細はPhase本文の検証記録。

WS014はincomplete。p001の残る設計判断とp003/p004はplanning、Venus/実GPU描画/i915は未開始。次はp003でmmap、submit/sync、display等の不足を実利用から補う。現在の実行Queueはなし。コードと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。

## 検証記録

| 検証 | 実行結果と限界 |
| --- | --- |
| run-gpu-framework-test.sh | PASS。実gpu.c＋実cdev.c。通常実行とASan/UBSanでpublication/permissions/handles/rollback/retirementを確認。held spinlockなしのcallback、同session再入拒否、別session進行、32個の上限と他session分離も確認 |
| gpu-uapi-layout.c（同runner） | PASS。-m32/-m64、全field offset、56/32/16 byte構造体、3 ioctl値一致 |
| run-pci-service-lifecycle-host.sh | PASS。実pci.c。attach/publish/unpublish/detach順序、busy/forced detach、publication rollback、cleanup失敗保持とretry、再入拒否 |
| make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix | PASS。amd64 vmunix checkerもPASS。config.mkは変更していない |
| git diff --check | PASS |
| 全文規約レビュー | C89宣言/定義、forward、目的コメント/段落、型・変数とprotocol説明、名前、call/return/条件、lock/ownership/参照寿命、HAL非変更を確認。reviewで欠けたif/caseコメントとheader空行を修正 |
| clang-format --dry-run --Werror（19.1.7） | 非zero。root設定が要求された複数行関数定義を畳み、1行forwardを分割する等、全文規約と不一致。全文規約を優先し自動整形を適用しなかった。formatter PASSとは報告しない |

immutable callback tableの初期化には参照先の宣言が必要なため、そのforward declarationをtableより前に置く。通常変数→forward→callback table→public→staticの順。backend/private dataはPCI detachまで借用、stale cdevはoffline wrapperだけを保持する。これらを独立レビューでも確認した。

全6 platformのソースリストへGPU coreを組み込んだが、今回buildしたのはamd64。host fixtureはkernel allocator/lock/uaccessを置換する。実schedulerの競合、VFS syscall、QEMU/実GPU描画の検証とは呼ばない。mmap等の不足はp003へ明示し、p001/p003/p004やWS全体をclearしていない。

検証時点UTC: 2026-09-12T12:16:18.812792+00:00

Host compiler: cc (Debian 14.2.0-19) 14.2.0

Kernel compiler: clang version 23.1.0 (git@github.com:awemorris/zedBSD.git fbdda93b5dc2c19bae8970a4dcbaf8cb417a7fbf)

Base commit: `16077b64937804d9f155e1eaea65c380abb54e6c` + uncommitted q304 changes

## 検証対象SHA-256

| File | SHA-256 |
| --- | --- |
| `include/drivers/gpu.h` | `fbcdd1ec161f308b52399c78c10e28423e711dbc56a6538620484bf6d28c4a3e` |
| `include/uapi/gpu.h` | `82de065c9ecdff56c93676d1360e1d9666366e983281c7fa60cdd3fec7163051` |
| `src/drivers/gpu/gpu.c` | `34124b044ee8047303fe4a306ee262a79c2d9ca7d6490199dbed584369af6c5d` |
| `include/drivers/pci.h` | `e9ce00c11a77c94a2cd0f80b9e81f5c09fcc780425fec620b8685ab459bb9214` |
| `src/drivers/pci/pci.c` | `998250d083fcca13461fc3c628b7b4214fe4ffb12314daf8d6b90bf7c29ecaa3` |
| `src/kern/vfs.c` | `94d58a44d0343e19fb30e207401c75e0737ba6f0737151d743c0655911d8a656` |
| `build/amd64/vmunix` | `d14bbe72c88a1898e3d0f29f2572c44a3c49fd77dd1268d7cf874dbfa83a34dc` |
| `plan/ws014/tests/gpu-framework.c` | `f69803ba7f8ca7fde13843d904809390750e8d623f56946c3e2289670622e14a` |
| `plan/ws014/tests/gpu-uapi-layout.c` | `8c91c3bd3d3f7fc22a4526c6fe39237222464d0225ed00779621635af40ce00c` |
| `plan/ws014/tests/pci-service-lifecycle-host.c` | `37fb77c494feef3080262e8934440948ed27880ce7bc1b436447a23173a29bfa` |
| `plan/ws014/tests/run-gpu-framework-test.sh` | `0929a8767b2a4de9709f89fafb24fd276bce194eaf57cbe06a7cf16024e28039` |
| `plan/ws014/tests/run-pci-service-lifecycle-host.sh` | `901dd4fe07f5fa741776200638b1ec350aef4fc7f5ac506f4d1d6d1c79d5fa3f` |
| `platform/amd64/vmunix.mk` | `6813267fda3caecf0c7247e3f9d771b4f094f07633e802c5bb09ae100db96cbd` |
| `platform/arm64/vmunix.mk` | `04c4c94d380c331ca8666e6e9de8d1525033619980085c12eb94e99746d6baeb` |
| `platform/pc98/vmunix.mk` | `229e7924be69641d2d93380be58d29f3523e4b4900bdd4f916fc355c6522af7d` |
| `platform/pcat/vmunix.mk` | `94a37e070d1cda8b8c7b87ddc590c7f75a3d58fddcfa8a39463d42cbfa7c6317` |
| `platform/sparcv9/vmunix.mk` | `335206c29a28787f374f4032ab7eab59bacaa9f27a7ba20dcf5386e7d8acd399` |
| `platform/x68k/vmunix.mk` | `6bae84cf945636ebd1685a8edb1e942394f2ffc43d3b1c5a814182b9de8b443c` |
