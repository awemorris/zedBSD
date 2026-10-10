# WS197 p013: Bluetooth応答監視と復旧の検証 / 2026-10-10

Scope: bt/main.c、bluetoothd/{main.c,privsep.c,privsep.h,protocol.h,session.c}の6source。base main 1b07745e4。独立codex/ws197-media-uat worktree、private build/ws197-media-uat。ユーザーのdaemon自動再起動/device reopen/reset指示、以前の実機SSH/update/main承認を適用。スマホ側の接続削除は現状維持。

## Build / standards

```sh
make -j8 BUILD=build/ws197-media-uat ZEDBSD_CONFIG=config/current-uat.mk \
 ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
 -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
 -o /home/awe/zedBSD-claude1/.claude/worktrees/ws197-media-uat/build/llvm/.zedbsd-install-23.1.0-zedbsd8 \
 'DYNAMIC_CPPFLAGS=-nostdinc -I. -Iinclude -Iuserland/desktop/include -isystem /home/awe/zedBSD-claude1/build/amd64/sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC' \
 build/ws197-media-uat/bin/bt build/ws197-media-uat/bin/bluetoothd
python3 plan/tools/style-check.py userland/base/bt/main.c userland/base/bluetoothd/main.c userland/base/bluetoothd/privsep.c userland/base/bluetoothd/privsep.h userland/base/bluetoothd/protocol.h userland/base/bluetoothd/session.c --summary
git diff --check
```

結果: build exit0、warning0/error0、check-dynamic-elf合格。private最終log bt-recovery-build-final.log。style-check total0、diff-check clean。clang-format-19 19.1.7（edited ranges、InheritParentConfig/ColumnLimit0）の後、全文規約が要求するdefinition argument TABと一行prototypeを復元。

C全文のmanual確認: 6sourceの全変更、コメント/ANSI宣言/各失敗と成功のreturn、reader bufferとfd寿命、monotonic deadline、poll/HUP/invalidfd、privsep進捗に返信しないdescriptor順序、親が自身の子だけをTERM/KILLしてreap、normal stop exit0/stall exit1、initialization別budget、firmware記憶の対象keyだけを除去、bond/power保持。既存HAL/UAPI/desktop interface/init設定/toolchainに変更なし。今回の限定conformanceでありWS全体p009は未完。

最終cksum（転送後の実機と一致）:

| Artifact | CRC | Bytes |
| --- | --- | --- |
| bt | 1437685180 | 19984 |
| bluetoothd | 3262366189 | 294080 |

## 短いhost確認

private local UNIX socket/fork probeで実production implementationを使用。CLI silent responseは5秒/exit2、fresh CHECKはexit0。sessionの連続wrong-opcode応答でも40ms deadlineでETIMEDOUT、closed descriptorはEBADF、peer HUPはENODEV。ASan/UBSan（detect_leaks=1）PASS。supervisor子のALIVE→SIGSTOPは約18秒でTERM・3秒後KILL/reap、親exit1、残存子無し（timeout25内PASS）。一時probeはprivate build内のみ、恒久の回帰toolは追加しない。

## 実機 SSH / 10.0.30.3

- 初期状態は親7/子9、service running、bt show6秒timeout。既存sudo service restart bluetoothd後は親398/子399、bt show READY/exit0。元のblocking stackは取得していない。
- 元binは/tmp/ws197-bt-recovery/originalへ保存、/bin/bt・/sbin/bluetoothd更新。service restart exit0、最終cksum一致。
- 初回SIGSTOP502: 13:31:49停止→13:32:03 WATCHDOG→13:32:08 READY、親501/子502から親523/子524へ自動再起動。CHECK ready。ただしalarmベースCLIはnativeで12秒SSH timeout、host合格をnative合格と扱わない。
- CLIをnonblocking socket + deadline poll + buffered readへ修正して追加確認。SIGSTOP524: 13:36:13停止、bt show5.11秒/exit2（BT SHOW state=timeout）。13:36:28 WATCHDOG→13:36:33 READY、親583/子584。CHECK ready/exit0。
- 再起動完了前に送ったreopen/check/resetはdaemon終了/socket拒否。再起動後、reopen exit0/0.12秒→CHECK exit0、reset exit0/0.22秒→CHECK exit0。daemonとcontrollerが応答している状態で各操作を1回実行。
- 保存bond/powerを削除せず、スマホ再pairをせず、compositor/Phone/sessiondはそのまま。スマホno-linkはユーザーが接続削除済みと説明した状態で、今回のcontroller復旧の不合格とはしない。

## 運用・限界

root親がmain-loopのALIVEを監視、通常15秒・firmware初期化90秒。stall時に自身の子へTERM、3秒後KILL/reap、exit1でinitの既存restart=on-failureを利用。sessiond/compositorの新root request不要。既存initのfailure restart上限5回は保持。親も含むkernel waitの停止を救済できる保証ではない。元のhang根因は未確定。

manual fallback: sudo service restart bluetoothd。fresh HCI診断: bt check。daemonは応答するがcontrollerをやり直したい時: bt reopen、bt reset（接続は一時切断）。CLI SHOW/CHECK5秒、REOPEN/RESET60秒、一般30秒/scan+15秒、pair/agentはhuman waitを保持。

QEMU、image build、aggregate make check、負荷/耐久、full HID regressionsは未実施（今回の有限scope/user方針）。main統合後read-backはphase/Queue/WSに記録。GitHub/shared Board/cacheの投影はQ1、push無し。

Source/証拠7fa0a0e9cをmainへfast-forward、HEAD read-back・clean tree確認。以後の変更はPhase/Queue/WSの結果記録のみ。
