<!-- awesome-plan project=zedbsd record=ws019-p032 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase032/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p032: public diskpart GPT initialization

Status: completed q165; timebox 90 active minutes; [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase032-diskpart-gpt-init/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20); prerequisite of p006
Depends on completed p030/p031. Standing autonomous execution authorization.

Expose `diskpart init DISK DISKUUID [START COUNT TYPE PARTUUID NAME]...` through
the existing command. Zero through sixteen complete partition groups, numbered
in argument order. Sectors are decimal logical sectors; GUIDs must be valid,
nonzero and partition GUIDs distinct. Existing codec validates geometry,
overlap, bounds and UTF-8 names. This command accepts explicit layout; installer
MiB alignment/ESP sizing remain p006. No filesystem formatting or secure erase
is claimed. Keep existing add/delete/show/reload semantics in this queue.

Open O_RDWR, query identity, acquire BLKRESERVE, construct and validate the entire
new table before prompting. Display exact disk identity and proposed partitions
with destruction warning; require exact `ERASE NAME:REGISTRATION` input. EOF,
wrong input, incomplete input line or output failure make no metadata writes.
No force/machine mutation mode. Noct can feed the phrase only after its product
NO/YES confirmation; it must not substitute an unrelated fresh device identity.

Retain the same descriptor/reservation through snapshot revalidation, backup
and primary GPT, PMBR, checked flush/readback and BLKREREADPART. Errors after any
write attempt report potentially partial modification; written-but-reload-failed
returns 3. Close and output errors cannot report success. Cancellation/error
must release the reservation so subsequent normal operations work.

Host CLI tests use actual command/codec with syscall faults: malformed groups,
GUID/name/range overlap, refusal/cancel/EOF, output/close failures, reservation
failure, partial write, flush and reload failure, and full success. Existing
parser/editor/machine-output regressions retain sanitizer coverage.
QEMU uses disposable blank and existing GPT media; verify no-write cancellation
and busy source refusal, successful complete table creation and live child
geometry, then independently decode final metadata/CRCs. Existing FAT mounted
child refusal is exercised in the command run. All three target builds pass.
Do not mark p006 or p007 complete from this prerequisite.
