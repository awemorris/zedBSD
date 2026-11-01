<!-- awesome-plan project=zedbsd record=ws253 -->

# WS253 起動の時間の短縮: desktop を先に、network などは後に

<!-- awesome-plan-current:start -->
Status: completed（2026-10-27 P3 の精査 q1608、ユーザー「…masterとWSを修正して、completedやcanceledにしてください。」: p001 の 2 段の起動・late・disk の待ちの短縮は main（4e3ded77f・d454d4bd3・cf638a523）、QEMU で約 9 s → 約 4 s。残りは 5330 の起動の時間の確かめだけ → 2026-10-27 ユーザー「実機確認だけが残っているWSやPhaseは、古いものはclearedにしてしまいます。」）
Completed: 2026-10-27（q1608）
Primary Milestone: MG001
Related Milestones: なし
Objectives: —
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

通常の起動（graphical login）で、電源から login 画面（sessiond の Wayland）までの時間を短くする。通常の起動では sessiond を network・audio・sshd などより先に立て、debug の起動（kmsg=console・login=console）は今の順序を保つ。desktop に要る service の一覧を理由とともに報告する。

## ユーザーの決定

- 2026-10-16 ユーザー「5330はデスクトップが起動しました。OKです。ただ、起動に1分くらいかかってました。起動時間がめちゃくちゃ長くなってます。これはIntelは関係なく、全般的な話です。これを解決するためのWSを作り、サブエージェントでQEMUを使って解析して、直してください。networkのupを待つ処理のせいかも？それだけでなく、init立ち上げるサービスって、デスクトップセッションを早めに起動するのがいいと思うんです。ネットワークはあとからでいい。もちろん、デバッグ用の起動ではそうではないので、デバッグ時だけ特別扱いしましょう。通常ブートではネットワークは後回し。audiod, networkd, sshdなど大半は後回しで、とにかくsessiondでWaylandを立ち上げる。どうしても依存のあるサービスは教えてください。」

## 結果

- init の 2 段の起動（`userland/base/init/main.c`）: 起動の parameter が `login=graphical` かつ `kmsg=quiet` の時だけ desktop を先に（late の印の service を desktop の後に）。debug の起動（`kmsg=console`・`login=console`・parameter 無し）は 1 段で今までの順序。判定のため kernel に sysctl `kern.boot.kmsg`（`include/uapi/sysctl.h` の KERN_BOOT_KMSG）を足した。disk の待ちの短縮（kernel の 5 秒の待ち）。compositor の STARTUP の行を細かくした。文書は `docs/reference/init-services.md`・`docs/reference/kernel-boot-parameters.md`。main 4e3ded77f・d454d4bd3・cf638a523。
- QEMU（Venus、NVMe）: 起動から desktop まで約 9 s → 約 4 s。遅い USB（100 IOPS に絞る）では 11.9 → 10.3 s。
- 5330（T1-703、main 1f22728f5、warm 2 回）: desktop shown 1 回目 24.7 s（dd の直後の最初の起動、VFS の初期化 13 s）、2 回目 6.7 s（VFS 0.7 s、kernel→init 1.9 s）。2 回目の残りは compositor の wallpaper 1.8 s・glyphs-fonts 1.6 s（USB の読み込み、BUG-327）、networking 約 10 s（desktop の後）。
- desktop がどうしても依存する物（ユーザーへの報告）: kernel の GPU の device（sessiond は attach の間 最大 120 秒待つ）、input の device、mount -a・swapon、syslogd、homes。後回しでも動く物: networkd・audiod・bluetoothd（Bluetooth の keyboard・mouse は bluetoothd が立つまで使えない）・volumed ほか。

## 制限・移管

- 2026-11-02（BUG-394、Q1 の判断）: debug の起動は `kmsg=console` が明示された時だけになり、`login=console` だけの起動と kmsg= の無い起動（Pi）も login を先に・late を後にする 2 段の起動に置き換わった。
- 2 段目（networkd の notify は最大 10 秒、networking の oneshot）の間、init は子の回収と control socket の応答をしない。
- 5330 の 1 回目の起動の VFS の初期化（13 s）と USB の読み込みの遅さは BUG-327 の側。
- 規約の全文の見直し（p002）は WS の外の一括の見直しへ（2026-10-08 ユーザー「規約の整形（各 WS の全文規約の Phase）はベータ3」、q1608 の扱い）。

## Phase 一覧

| Phase | 目的 | 結果 |
| --- | --- | --- |
| ws253-p001 | 測定・原因の特定・init の service の順序の変更（通常は sessiond を先に、debug は今のまま）・依存の一覧 | in-progress のまま WS を閉じた（実装は main、QEMU の測定済み、5330 は T1-703 で 2 回測った） |
| ws253-p002 | 規約の全文の見直し | 未実施（WS の外の一括の見直しへ） |

## 記録の所在

Phase の計画・結果・試験（`plan/ws253/phase001`、`plan/ws253/tests/`）は削除する。git の commit `fb1f6afee` の `plan/ws253/` にある。T1-703 の出力は `/home/awe/zedBSD-worktrees/t1/build/t1703-out/`（git の外）。Queue の履歴は [plan/history](../history/index.md)。
