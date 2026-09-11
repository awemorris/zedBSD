<!-- awesome-plan project=zedbsd record=ws018-p020 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws018/phase020/phase.md`

親: [ws018](https://github.com/awemorris/zedBSD/issues/19)

# ws018-p020: Request-sized syscall I/O

Date: 2026-09-06
Status: completed
Queue: q087
Authorization: user's explicit request to remove syscall 4KiB splitting.

Allocate min(request length, 256KiB) for regular files before file_io_begin.
Halve failed allocation attempts until the existing 512B stack fallback applies.
The cap bounds concurrent physical allocations: kern_malloc currently needs
contiguous physical backing for large allocations. No arbitrary request-sized
unbounded allocation, VM mapping change, BIO/USB size change, or writeback change.
Read/write/pread/pwrite share the helper; readv/writev keep element boundaries
but no longer split an element every 4KiB. Stream and PIPE_BUF behavior remains.

Acceptance: production function fixtures for all six syscall entry paths,
64KiB -> one transfer for scalar I/O and a single iovec; cap-crossing lengths,
partial backend/error/EOF, user-copy fault, allocation pressure and cleanup,
positional offsets, vector zero elements, stream reads and PIPE_BUF atomicity.
Use ASan/UBSan and native QEMU USB-root file/MAP_SHARED/pipe/persistence stories.
Run supported amd64/pcat/pc98 builds serialized with make -j16. Reuse the q086
256KiB-overwrite+fsync benchmark to observe, not guarantee, downstream improvement.
Record source hashes and limitations. No commit, no aggregate make check.

Evidence: [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws018-kernel-architecture/phase020-syscall-request-batching/results.md).
