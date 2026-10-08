<!-- awesome-plan project=zedbsd record=ws188 -->

# WS188: app の OS の操作を libkeiland → compositor → backend へ移す（Settings の残り）と境界の検査の強化

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG007
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ2）
Queue: なし
Resume point: 2026-10-08 q902 P1 の照合: p001〜p004 は全部 cleared（Q1）。WS の完了の判定は Q1（残る PENDING の audiod の 3 行は WS191 の範囲、取り出しの後の Devices の消え方は未実施）。ID の注意: 表の初めの p004（preview の spawn、canceled、directory 無し）と下の p004（Files の mount の一覧、phase004/ の実体、cleared）は同じ ID の 2 つの記録。番号は振り直さず、Q1 が扱いを決める。
<!-- awesome-plan-current:end -->

## 由来（2026-10-08 ユーザー）

「SettingsのBluetooth関連の操作は、libkeilandで抽象化して、zedBSD/Linux/FreeBSDで同じインタフェースで使えるようにします。libkeiland --> compositor --> コンポジタのlibkeiland-backend --> プラットフォームのデバイス操作 、です。コンポジタを通しているのは、libkeilandをプラットフォーム独立にするためです。サウンドやWiFiも同様です。libkeilandにプラットフォーム固有の操作がもし入っていれば、それはコンポジタ経由に移したいです。同様に、Settingsのディスプレイ関連の操作も、libkeilandで抽象化します。」（Guardrail の「Bluetooth と Display も compositor 経由」）

## 監査の結果（2026-10-08 Q1 の依頼の読み）

- libkeiland: 違反なし（daemon・device・system の file・ioctl・OS の ifdef 無し。残りは app 自身の file と POSIX）。
- Display: Settings → libkeiland（kl_system_displays_*）→ compositor → backend（backlight-zedbsd.c、display-zedbsd.c）で境界どおり。Linux・FreeBSD の明るさは unsupported。
- Bluetooth: 経路が無い（Settings の頁は「coming soon」）。作るのは [WS143](../ws143/ws.md) p006（kl_system_bluetooth_v1・kl_system_bluetooth_*・compositor の bluetooth-shell・kl_backend_bluetooth_*、zedBSD は /run/bluetoothd.sock、Linux は BlueZ の D-Bus）。
- **Settings が OS に直に触っている所**（この WS で移す）:
  - about.c: /etc/os-release・/usr/lib/os-release、uname・sysconf・gethostname、x86_64 の cpuid → system の情報の拡張（backend の monitor_info）。
  - look.c の statvfs（"/"・"/home"・"/usr"・"/var"・"/tmp"・"/boot"、Storage の volume）→ kl_system_devices か storage の要求。
  - page-users.c の getpwent・getgrnam（wheel・network、uid≥1000・nologin の規則）→ kl_system_account の利用者の一覧。page-sharing.c・welcome.c の getpwuid → kl_system_account に名前・full name。
  - page-languages.c の KEILAND_SYSCONFDIR/keiland/language の読み → kl_settings か kl_system_account。
  - preview/{zedbsd,linux,freebsd}/spawn.c（壁紙の縮小画像の sandbox の起動、Files も同じ）→ ユーザーの判断（compositor・backend の preview の口にするか、app の側の例外にするか）。
  - main.c の posix_spawn(KEILAND_BINDIR "/files") → compositor の起動の要求（任意）。
- 境界の検査（plan/tools/keiland-os-boundary/check.sh）が見逃す物: /dev・/proc・/sys・/run・/var・/etc の literal、sockaddr_un と daemon の socket の名前、sysctl、getpw*・getgr*、statvfs、fork・exec・posix_spawn。C1・C2 は libkeiland と compositor だけを見る。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| p001 | 設計: 移し先の拡張（system の情報・storage の volume・利用者の一覧・system の言語）、protocol の version、3 OS の backend、design-reviewer | cleared | — |
| p002 | 実装: Settings の About・Storage・Users・Sharing・Welcome・Languages を libkeiland 経由に | cleared（T1-427） | p001 |
| p002a | Files の Today の空き容量（statvfs）を machine の FILESYSTEMS へ（2026-10-08 Q1 の判断） | cleared（T1-428） | p002 |
| p003 | 境界の検査の強化（app と libkeiland の literal・socket・getpw*・statvfs・spawn、許可の表） | cleared | p002（先に入れると FAIL） |
| p004（旧、canceled、directory 無し。phase004/ は下の行の Files の mount） | （取りやめ）preview の spawn は app の側の例外（2026-10-08 ユーザー）。p003 の検査の許可の表に preview/*/spawn.c を載せる。Files の起動（posix_spawn）は p001 で compositor の起動の要求にするかを決める | canceled | — |

## ユーザーの決定（2026-10-08）

preview の sandbox の起動（preview/{zedbsd,linux,freebsd}/spawn.c）は「app の側の例外として残す」。境界の検査の許可の表に載せる。

## ユーザーの決定（2026-10-08 昼）

- Files の mount の表（files/mntent/mounts-mntent.c・files/freebsd/mounts-freebsd.c、Places と各 volume の .Trash）: **kl_system に移す**（kl_system_machine に mount の一覧を足す）→ p004。
- 再生の音（videoplayer・music・libmedia が audiod に直に流す）: **libkeiland の audio stream の口に移す**（compositor・backend が OS ごとに、zedBSD は audiod、Linux は PipeWire、FreeBSD は OSS）→ 大きいので [WS191](../ws191/ws.md)。
- Q1 の判断: B3 の既存の FAIL は P2 の案どおり（printd の Makefile の comment の行を見ない、sessiond は system の service なので B3 の userland/base/net/ の規則から外す）。

| p004 | Files の mount の一覧を kl_system_machine の要求へ（Places・.Trash の発見）、許可の表の PENDING の行を外す | cleared（T1-429〜431） | p003 |
