<!-- awesome-plan project=zedbsd record=ws188 -->

# WS188: app の OS の操作を libkeiland → compositor → backend へ移す（Settings の残り）と境界の検査の強化

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc §2）: p001〜p004 cleared）
Primary Milestone: MG006
Related Milestones: MG007
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ2）
Queue: なし
<!-- awesome-plan-current:end -->

## 由来（2026-10-08 ユーザー）

「SettingsのBluetooth関連の操作は、libkeilandで抽象化して、zedBSD/Linux/FreeBSDで同じインタフェースで使えるようにします。libkeiland --> compositor --> コンポジタのlibkeiland-backend --> プラットフォームのデバイス操作 、です。コンポジタを通しているのは、libkeilandをプラットフォーム独立にするためです。サウンドやWiFiも同様です。libkeilandにプラットフォーム固有の操作がもし入っていれば、それはコンポジタ経由に移したいです。同様に、Settingsのディスプレイ関連の操作も、libkeilandで抽象化します。」（Guardrail の「Bluetooth と Display も compositor 経由」）

## 結果

- Settings の About・Storage・Users・Sharing・Welcome・Languages が OS に直に触らず、libkeiland（`kl_system_machine_v1`）→ compositor → libkeiland-backend（zedBSD・Linux・FreeBSD）で読む（p001 の設計 第 3 版、p002、T1-427）。
- Files の Today の空き容量（p002a、T1-428）と mount の一覧（Places と各 volume の .Trash の発見、p004、T1-429 host-model・T1-430 QEMU の USB の mount・Trash・Empty Trash・T1-431 FreeBSD の build）も同じ経路へ。
- 境界の検査（`plan/tools/keiland-os-boundary/check.sh`）が app と libkeiland の /dev・/proc・/run などの literal、daemon の socket、getpw*・statvfs・spawn を見る。許可の表は `plan/tools/keiland-os-boundary/app-allow.tsv`（p003、main c671551fe で PASS）。
- 設計の詳細（D1〜D6）は `plan/ws188/phase001/phase.md`（source の注釈が参照している。消した後は git の履歴）。

## 制限・移管

- 境界の検査の PENDING の残り: audiod の 3 行 → [WS191](../ws191/ws.md)（再生の音を libkeiland の audio stream の口へ）。
- preview の sandbox の起動（preview/{zedbsd,linux,freebsd}/spawn.c）はユーザーの決定（2026-10-08）で app の側の例外として許可の表に載せた。
- Bluetooth の経路は [WS143](../ws143/ws.md) p006。
- 未実施: USB の volume を取り出した後に Files の Devices から消える様子。

## Phase

| Phase | 目的 | 結果 |
| --- | --- | --- |
| p001 | 設計: 移し先の拡張（system の情報・storage の volume・利用者の一覧・system の言語）、protocol の version、3 OS の backend、design-reviewer | cleared（2026-10-08 Q1、第 3 版） |
| p002 | Settings の About・Storage・Users・Sharing・Welcome・Languages を libkeiland 経由に | cleared（T1-427） |
| p002a | Files の Today の空き容量を machine の FILESYSTEMS へ | cleared（T1-428） |
| p003 | 境界の検査の強化と許可の表 | cleared（check.sh PASS） |
| p004（旧、canceled） | preview の spawn を移す案。ユーザーの決定で app の側の例外にしたので取りやめ。directory は無い | canceled |
| p004 | Files の mount の一覧を kl_system_machine の要求へ（Places・.Trash の発見）。旧い p004 の取りやめの後に同じ ID で立てた。番号は振り直さず注記で区別（2026-10-08 Q1） | cleared（T1-429〜431） |
