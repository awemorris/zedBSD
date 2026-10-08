<!-- awesome-plan project=zedbsd record=ws160 -->
# WS160: su・sudo・passwd

Status: incomplete（2026-10-08 q910 P2 の照合: p001・p002 は cleared。WS の完了を Q1 が判定）（2026-10-05 P1: p001 を実装、QEMU は T1 に依頼。**ベータ1**）
Master: [master](../master.md)
Primary Milestone: MG002
Related: WS129（ベータ1 の release、U3・U10）
Queue: q721

## 由来

ベータ1 の release の image は root を lock し（U10）、kei の password を公開する（U3）。image に su・doas・sudo・passwd が無く、kei は wheel に居ないので、管理の作業も password の変更もできなかった（ws129-p004・p013 の P2 の発見）。
- ユーザー（2026-10-05 未明）「su, sudoを実装してください。」
- ユーザー（同）passwd について「passwd を実装する」。
- ユーザー（同）「パスワードはSettingsのUsers画面でもGUI実装します。passwdも実装します。」→ Settings の Users の頁でも password を変えられる（WS089 の Users の頁、passwd と同じ核を使う）。
- 同じ回答で: release でも自動 login を保つ、nightly の CI の config は今のまま。

## 目標と受け入れ

1. `passwd`: 利用者が自分の password を変える（今の password を確かめ、SHA-512 crypt で /etc/shadow を安全に書き換える。root は他の利用者の password を設定できる）。
2. `su`: 目標の利用者の password で切り替える（既定は root。root が lock なら拒む）。
3. `sudo`: wheel の利用者が自分の password で root として command を実行する（最小の /etc/sudoers 相当の規則、一定時間の認証の記憶は任意）。kei を wheel に入れる（base の group と release の config）。
4. setuid の扱いが kernel と rootfs の install で正しいこと、環境の変数の消毒、失敗の記録。
5. host の試験と QEMU（T1）: kei で passwd を変え、新しい password で SSH に入れる。sudo で root の command が動き、wheel でない利用者は拒まれる。su の成功と失敗。

6. Settings の Users の頁で自分の password を変える GUI（今の password・新しい password・確認）。passwd と同じ検証と書き換えの核（library か setuid の helper）を使い、Settings に root の権限を持たせない。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws160-p001](phase001/phase.md) | 設計（既存の login・crypt・shadow の扱い、setuid の kernel の対応、規則の file の形、Settings からの変更の経路）と passwd・su・sudo の実装・試験 | cleared（2026-10-05 Q1、T1-127） | なし |
| [ws160-p002](phase002/phase.md) | Settings の Users の頁の password の変更（GUI）。Settings → libkeiland の kl_system_account → compositor → libkeiland-backend → zedBSD の `passwd -s`（Settings に権限を持たせない） | cleared（2026-10-05 Q1、T1-121） | p001 |
