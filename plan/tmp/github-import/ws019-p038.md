<!-- awesome-plan project=zedbsd record=ws019-p038 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase038/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p038: immutable root-image source view

Status: completed, q171
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 60 active minutes

Prove the existing public mount path can expose the retained boot lower UFS
image as an independent read-only copy source. Resolve the loop device by the
live kern.boot.root_image registration, never by a hardcoded loop number or
configuration pathname. Reject ambiguous, writable or mismatched geometry.

Use a disposable QEMU USB boot and an owned /run directory. Mount UFS read-only,
verify stat device and immutable /bin/noct bytes against the source build,
reject a write, distinguish an overlay-only marker from the source tree,
unmount and repeat. Verify the live root image identity remains unchanged and
normal root operation survives cleanup. Verify source rootfs.img and production
image bytes remain unchanged. Capture the actual installer source screen and
cancel without target changes. Record failure before any revised attempt.

This bounded prerequisite verifies an existing mechanism. Integration into the
Noct workspace/transaction remains p007; it is not full native installation
acceptance. No new kernel mount API is required if this mechanism passes.
