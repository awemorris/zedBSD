<!-- awesome-plan project=zedbsd record=ws019-p035 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase035/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p035: reserved FAT32 mkfs command

Status: completed q168; timebox 90 active minutes
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20); prerequisites p033/p034 completed

Add `mkfs -t fat32 DEVICE` using the portable codec and BLKRESERVE. Accept
canonical /dev names or bare device names, refuse symlinks/special non-block
objects, unsupported geometry and mounted/claimed media. Query and reserve
one O_RDWR description, print the volume identity and destructive warning,
require exact `FORMAT NAME:REGISTRATION` plus newline. No force or automatic
yes switch. Installer may send the phrase only after its own explicit NO/YES
confirmation and matching revalidated registration. Keep the reservation
through write, readback and close; no second open and no partition reload.

The command initially admits 512-byte sectors because the kernel FAT mount
path only supports them. Do not imply other codec geometries are mountable.
Keep existing UFS regular-file and pristine syntax unchanged. Report failures
after first attempted write as possibly changed media; output/close failures
must not return success. Formatter volume serial is not a boot identity.

Acceptance: host production command with independent syscall/codec oracles
for grammar, admission, cancellation, output failure and every lifecycle error;
existing formatter frontend regressions; disposable QEMU mounted-child/source
refusal, cancellation hash preservation, formatting then FAT mount and new
file write/read, unmount and independent mtools inspection; production image
and unrelated GPT/sibling ranges unchanged. Three target builds pass.

No native UFS/root or graphical frontend completion is claimed here.

Native investigation found that bare `cp` still dispatches an obsolete shell
builtin, despite `type cp` reporting /bin/cp. It always creates mode 0666 and
does not implement the maintained cp policies. Retire this duplicate dispatch
and implementation within this command-path acceptance; use normal PATH
resolution to the maintained external cp. Verify bare cp and copied executable
on native FAT, plus available shell host regressions. Other builtins are outside
this change. Temporary FAT/cp diagnostic output must be removed before gates.

Accepted 2026-09-09: [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase035-fat32-command/results.md). Host/sanitizer/legacy formatter and
shell regressions, final public QEMU/mtools acceptance and three builds pass.
