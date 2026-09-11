<!-- awesome-plan project=zedbsd record=ws002-p019 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase019/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p019: integrated QEMU acceptance and repair

WSID: `ws002`

Phase ID: `p019`

Status: complete for the documented minimum system

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Run the installed system under bounded `qemu-system-x86_64` through boot/init,
logging, getty/login/shell, networking, scheduling, optional time setup,
service control/failure, persistence, and orderly shutdown. Repair integration
defects that prevent the documented minimum rather than handing them off.

## Acceptance and result

The minimum system reached complete integrated operation. The authoritative
executable entry is `tests/phase19-qemu-test.py` with its rc.conf, service, and
smoke fixtures, indexed in [WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md). POSIX gaps found
outside the minimum remain in [WS001](https://github.com/awemorris/zedBSD/issues/2).

## Interruption record

Not interrupted. The subsequent network synchronization redesign is
`ws002-p020`; new hardware/network expansion belongs to WS003–WS005.

## Completion conditions

- The installed QEMU system passes boot, logging, login/shell, services,
  scheduling, networking, optional time, persistence, and shutdown scenarios.
- Defects blocking the documented minimum system are repaired and retested.
- Remaining out-of-scope POSIX incompatibilities are recorded in WS001 with evidence.
