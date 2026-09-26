<!-- awesome-plan project=zedbsd record=master -->

<!-- awesome-plan-current:start -->
Active Queue: なし。直近: q465（ws035-p066 cleared: Intel GPU（i915）で Wiseman Mode。GPU の client は F-022）。直近: q464（ws035-p063 cleared: Wiseview。WM の名は Wiseman）。直近: q463（ws035-p062 cleared: タイトルバーのドッキング）。直近: q462（ws035-p061 cleared: 絵の壁紙と透ける窓）。直近: q461（ws035-p060 cleared: mview を glass の窓で Vulkan 描画）。直近: q460（ws035-p059 cleared: `zwl --glass` の浮いたタイトルバーとすりガラス）。直近: q459（ws035-p054 cleared: acquire fence。受け入れ 3 はユーザーの判断で読み替え）。直近: q458（ws035-p053 cleared: `wl_shm` と cursor）。直近: q456（ws062-p003 cleared: amd64 の既定を native・2 GiB に、CI は gzip）。直近の終了: q453（ws065-p003 cleared: sh の builtin の bash 拡張）
Current Focused Goal: fg010 — Wayland デスクトップ（2026-10-17 の OSC Tokyo Fall のデモ）。fg011（expat の configure と compile を Linux と同等に）は達成して終了（2026-09-26 ユーザー「パフォーマンス問題はいったん終了しましょう」。configure 7.1〜7.5 秒・host 10.7 秒、`make -j1` 9.9〜10.4 秒・host 15.5 秒、`make -j4` 4.0〜4.1 秒・host 5.1 秒、`cc t.c -o t` 75〜85 ms・host 83〜85 ms）。`ld.so` の最適化は WS066（後で）
Next: 2026-09-26 ユーザー指示の順: BUG-054（WS067）→ disk image の既定を native に（ws062-p003）→ WS035（fg010）。sh・make・vfork・mutex・性能・journal の規約の Phase（ws065-p004・ws064-p003・ws061-p011・ws063-p002）は作業中に問題が出たらすぐ対応、出なければ後回し。以前の残り: ws062-p003（既定を native に、q439 提案）、WS061 の `cc t.c -o t` の計測と規約の Phase、判断待ちの F-015（UFS の delayed write）・F-016（並列の make）。BUG-052（tmpfs 32 MiB）。判断待ち: ws046-p014 の check（bash が無い）。WS056 p001 の判断（BUG-046）は継続。WS060（journal）・BUG-036・039・041・WS055 は fg011 の後。RPi4 実機の HDMI1・serial の確認はユーザー待ち、RPi4 の USB（WS048）は後回し。fg010 の合成（ws035-p052〜p057）は p051 の設計の承認待ち
<!-- awesome-plan-current:end -->

# zedBSD Master

[GitHub Project](https://github.com/users/awemorris/projects/2) ·
[Queue](queue.md) · [Guardrail](guardrail.md) · [Future Work](future-work.md) ·
[Bug Board](known-bugs.md) · [Past Log](history/index.md) · [設定](config.md)

## 目的・利用者・最終成果

- **目的**: 寛容なライセンスで企業が自由に使える UNIX 互換 OS を、GPL の Linux kernel に依存せずに作る。
- **利用者**: OS を組み込んで独自のディストリビューションを作る開発者・企業と、デスクトップ・ラップトップ・SBC で使う個人。
- **最終成果**: Linux/Android を置き換えられる水準のカーネルとユーザランド、最小の HAL による移植契約、用途別に構成・配布できる仕組み。
- **範囲**: kernel、HAL、driver、libc、base の userland、デスクトップ（zdesktop）、外部 package のクロスビルド、インストーラ、文書。
- **範囲外**: Linux の kernel ABI・DRM の互換、Mesa 流の user mode driver、正式な UNIX 認証・Vulkan CTS 認証の取得（主張しない）。
- **制約**: HAL の変更は差分ごとの事前承認（[Guardrail](guardrail.md)）。独立実装とライセンスの境界（[設計方針](master-design-policy.md)）。

## Objectives

- **O1**: 寛容なライセンスで企業が自由に使いやすい UNIX 互換システムを、GPL の Linux kernel に依存せず、Linux/Android を置き換え可能な水準で提供する。
- **O2**: デスクトップ、ラップトップ、SBC、タブレット、モバイルなど様々な規模で動くカーネルとユーザランドを提供し、開発者が独自ディストリビューションを自由にカスタマイズ・リブランディング・配布できるようにする。
- **O3**: UNIX/BSD/Linux の遺産から現代のシステムに必要なエッセンスを抽出し、networkd、netconf、service などをシンプルで一貫した仕組みとして再実装する。
- **O4**: ページベース MMU を備える 32bit/64bit コンピュータへ UNIX 互換 OS を確実に移植できる、明確で最小限の HAL を定義し、人類の共有知とする。
- **O5**: AI 時代の OSS のあり方を、大規模な AI 活用開発を通じて探索し、成果・失敗・人間の判断を再利用可能な知見として共有する。

## Milestone Goals

Milestone の達成は所属 WS の完了数ではなく、到達点の証拠で判定する。現時点で completed の Milestone は無い。

| Milestone | Objective | 受け入れの核 | 進捗 | Primary WS |
| --- | --- | --- | --- | --- |
| **MG001** 継続開発できる基盤 | O4, O5 | 文書化した環境で build でき、設計境界・規約・試験・制限を追跡できる | toolchain（WS021）・build tool（WS010）・x86 HAL の規約（WS023）は完了。文書（WS009）と試験資産の整理（WS026）が残る。vmunix の LTO（WS053）は完了 | WS009, WS010, WS021, WS023, WS026, WS047, WS053 |
| **MG002** UNIX アプリケーションの実行基盤 | O1 | process・memory・libc・loader/TLS の対応範囲を互換性台帳と代表アプリで確認できる | TLS（WS022）と外部 package の導入（WS032）は完了。base の utility の POSIX 化（WS043）は完了。POSIX 台帳（WS001）、アプリ導入（WS034）、sh（WS042）が進行中 | WS001, WS022, WS032, WS034, WS042, WS043, WS045, WS046, WS061 |
| **MG003** 対象機へ導入して単独起動 | O2, O4 | 合意した機種・媒体でインストール後の単独起動と login を確認できる。実機と QEMU の証拠を分ける | インストーラ（WS019）と Intel Mac（WS020）は完了。4 機種の実機受け入れ（WS028）が残る | WS003, WS004, WS019, WS020, WS028 |
| **MG004** データの保持とメモリ/ストレージの実用 | O1, O2 | 永続化、低メモリ時の進行、媒体世代、既定構成の性能を確認できる | swap（WS016）、UFS（WS024）、I/O・cache（WS025）は完了。実機の性能の一部は未測定。UFS の directory は 12 block まで育つ（WS054、完了） | WS016, WS024, WS025, WS054, WS057, WS058, WS059, WS060 |
| **MG005** 一貫したネットワーク/サービス管理 | O1, O2, O3 | networkd・netconf・service の責務・設定・操作が一貫し、永続化と失敗後の復旧を確認できる | サービス（WS002）、net console（WS011）、service console（WS012）は完了。有線 LAN の常駐管理（WS005・WS033）が残る | WS002, WS005, WS011, WS012, WS033 |
| **MG006** グラフィカルな操作環境 | O2 | 入力・描画・ウィンドウ・端末・GUI ツールの一連の操作を確認できる | 入力（WS006）、Noct/BeUI（WS008）、標準 Vulkan（WS030）、即時起床（WS041）は完了。**Wayland デスクトップ（WS035）が fg010 の中心** | WS006, WS007, WS008, WS014, WS017, WS029, WS030, WS031, WS035, WS037〜WS039, WS041 |
| **MG007** 用途別の独自ディストリビューション | O1, O2 | 第三者が用途別に構成し、独自ブランドで build・配布できる | 担う作業は一部だけ（WS013・WS015 は Future Work に保留）。未充足 | WS013, WS015 |
| **MG008** 最小 HAL の移植契約と異種機での実証 | O4 | HAL 契約・移植手順と異種/レトロ機での実証を公開する | source の所有の整理（WS018）と時間の単位（WS040）は完了。他 platform への反映（WS036、aarch64 を含む）と PowerPC（WS027）、rpi4 の開発環境（WS044）が残る | WS018, WS027, WS036, WS040, WS044 |
| **MG009** AI 活用 OSS 開発の知見の公開 | O5 | 設計権限・レビュー・変更追跡・失敗からの回復の事例と根拠を公開する | 担う作業が未定義 | なし |

## Current Focused Goals

| Goal | 当面の成果 | Milestone | 担当 | 出典 |
| --- | --- | --- | --- | --- |
| **fg010** | **2026-10-17 の Open Source Conference Tokyo Fall のデモに向けて、Wayland デスクトップ（zdesktop）を完成させる** | MG006 | [WS035](ws035/ws.md)（zdesktop・合成・タスクバー）。GPU の土台は [WS014](ws014/ws.md)・[WS031](ws031/ws.md) | 2026-09-24 ユーザー指示 |

デモの platform は amd64（QEMU と実機）と想定している（仮定。ユーザーの確認が要る）。以前の focus（fg004 インストーラの実機、
fg005 有線 LAN、fg007 HAL の可読性、fg009 PowerPC）は定義を残すが、現在は優先しない。

### fg010 に必要な判断

- **合成の設計（ws035-p051、[compositing-design.md](ws035/compositing-design.md)）の承認**。p052〜p055・p057 は承認待ち。
  2026-09-24 の 2 回のレビュー（2 つのモード、D1 の swapchain、wl_shm の CPU copy、Vulkan 経路の最適化）は反映済み。

## Workstream registry

| WS | Primary | 内容 | 状態 | 再開点 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | MG002 | POSIX.1-2024 準拠 | incomplete | 準拠性の台帳の残件 |
| [WS002](ws002/ws.md) | MG005 | システムサービス | completed | — |
| [WS003](ws003/ws.md) | MG003 | 旧実機 bring-up（終了・再利用禁止） | completed（ユーザー判断で終了） | 未完了は WS027・WS028・F-004 へ |
| [WS004](ws004/ws.md) | MG003 | ハードウェア拡張 | incomplete | NVMe 実機・転送・driver 共通化 |
| [WS005](ws005/ws.md) | MG005 | ネットワーク・WLAN | incomplete | 有線 LAN の常駐管理と起動時の待機（p013〜p017） |
| [WS006](ws006/ws.md) | MG006 | 入力と evdev | completed | — |
| [WS007](ws007/ws.md) | MG006 | グラフィックス・デスクトップ（旧） | incomplete | p004 の再現条件、amd64 の残件 |
| [WS008](ws008/ws.md) | MG006 | Noct と BeUI | completed | — |
| [WS009](ws009/ws.md) | MG001 | 文書 | incomplete | DOC-54（GPU の文書） |
| [WS010](ws010/ws.md) | MG001 | Noct の script と build tool | completed | — |
| [WS011](ws011/ws.md) | MG005 | ネットワーク設定 console | completed | — |
| [WS012](ws012/ws.md) | MG005 | サービス管理 console | completed | — |
| [WS013](ws013/ws.md) | MG007 | CPAR（container 分割） | incomplete（Future Work F-002 に保留） | 昇格まで再開しない |
| [WS014](ws014/ws.md) | MG006 | GPU framework・virtio-gpu・Wayland の土台 | incomplete | p004（最終 API と規約の確認） |
| [WS015](ws015/ws.md) | MG007 | μITRON リアルタイム領域 | planning（Future Work F-003 に保留） | 昇格まで再開しない |
| [WS016](ws016/ws.md) | MG004 | 実行時の swap 制御 | completed | — |
| [WS017](ws017/ws.md) | MG006 | LFB 描画の高速化 | planned | mmap・Xzed の高速描画 |
| [WS018](ws018/ws.md) | MG008 | kernel の source 所有と interface の統合 | completed | — |
| [WS019](ws019/ws.md) | MG003 | インストールとディスク管理 | completed | — |
| [WS020](ws020/ws.md) | MG003 | Intel Mac の UEFI 起動 | completed | — |
| [WS021](ws021/ws.md) | MG001 | x86 LLVM toolchain と sysroot | completed | — |
| [WS022](ws022/ws.md) | MG002 | ELF の TLS | completed | — |
| [WS023](ws023/ws.md) | MG001 | x86 HAL の規約準拠 | completed | — |
| [WS024](ws024/ws.md) | MG004 | 64-bit UFS の一本化 | completed | — |
| [WS025](ws025/ws.md) | MG004 | I/O・cache・物理メモリの再設計 | completed | — |
| [WS026](ws026/ws.md) | MG001 | 試験資産の整理 | planning | Phase 未定義 |
| [WS027](ws027/ws.md) | MG008 | PowerPC 移植 | planned | p001〜p007 |
| [WS028](ws028/ws.md) | MG003 | インストーラの実機動作（4 機種） | planning | NVMe の未動作の切り分け |
| [WS029](ws029/ws.md) | MG006 | i915 native GPU driver | incomplete | cold VFIO attach の間欠的な停止ほか |
| [WS030](ws030/ws.md) | MG006 | 標準 Vulkan 1.0 と直接表示 | completed | — |
| [WS031](ws031/ws.md) | MG006 | i915 native Vulkan 実行器 | incomplete | p015〜p048 planning |
| [WS032](ws032/ws.md) | MG002 | 外部 package のクロスビルド（clang・OpenSSL・OpenSSH） | completed | — |
| [WS033](ws033/ws.md) | MG005 | networking サービスと有線インタフェースの管理 | incomplete | 抜き差しの実機確認 |
| [WS034](ws034/ws.md) | MG002 | アプリケーション拡充と kernel・libc の是正 | incomplete | package の導入 |
| [WS035](ws035/ws.md) | MG006 | デスクトップ環境とアプリケーション | incomplete | **fg010**: zdesktop の合成・タスクバー |
| [WS036](ws036/ws.md) | MG008 | amd64 の成果を他 platform へ（aarch64 を含む） | incomplete | p021・p026（AArch64 の LLVM target）・p027（HAL 承認待ち） |
| [WS037](ws037/ws.md) | MG006 | NVIDIA GPU（予約） | planning | 番号のみ |
| [WS038](ws038/ws.md) | MG006 | Intel Arc dGPU（予約） | planning | 番号のみ |
| [WS039](ws039/ws.md) | MG006 | AMD RDNA GPU（予約） | planning | 番号のみ |
| [WS040](ws040/ws.md) | MG008 | 時間の単位を tick 周期から導く | completed | — |
| [WS041](ws041/ws.md) | MG006 | 起きた thread の即時実行 | completed | — |
| [WS042](ws042/ws.md) | MG002 | `/bin/sh` の POSIX 互換性 | completed | — |
| [WS043](ws043/ws.md) | MG002 | base の utility を POSIX に（sed・grep・awk ほか） | completed | — |
| [WS045](ws045/ws.md) | MG002 | base の text utility の GNU 拡張（sed・awk・grep ほか） | planning | WS043 が完了したので着手できる |
| [WS046](ws046/ws.md) | MG002 | GNU 互換の make（autotools の出力を実行できる範囲。並列・jobserver は WS064） | incomplete | p002〜p004・p006 cleared。p007 uncleared（BUG-033 の主因を直した）。p009・p012 cleared（BUG-033: configure 204〜252 → 91 秒、link 0.36 秒、file の fault 15 µs/page）。p013 cleared（libc の mount の一覧の API。coreutils の cross build が通った）。次は p014（p011 の当て直し）・p005 |
| [WS047](ws047/ws.md) | MG001 | build.sh と Noct による build system（TUI・kernel・base・packages を別の system に。Makefile は当面残す） | planning | p001 調査と設計 |
| [WS048](ws048/ws.md) | MG008 | Raspberry Pi 4 の USB（PCIe・VL805 の xHCI・USB キーボード） | planning | p001（調査と設計）。2026-09-24 ユーザー判断で後回し |
| [WS044](ws044/ws.md) | MG008 | rpi4 を開発に使える形に（console の font、FAT32 の boot、lldb） | incomplete | p001 font・p005 実機起動の準備 cleared、実機の結果待ち。p002・p003 は kernel の安定化の後 |
| [WS049](ws049/ws.md) | MG003 | kernel 内の ACPI AML interpreter | planning | p001 調査と設計（`hal_get_arch_handoff()` に ACPI の名前を足す差分に承認が要る） |
| [WS050](ws050/ws.md) | MG003 | USB-C の UCSI driver | planning | WS049 が前提 |
| [WS051](ws051/ws.md) | MG006 | USB-C の DisplayPort Alternate Mode | planning | WS050 と i915 の display が前提 |
| [WS052](ws052/ws.md) | MG003 | 電源管理（S0i3、modern standby、`/dev/system` で制御。S3・S4 は対応しない） | planning | WS049 が前提 |
| [WS053](ws053/ws.md) | MG001 | clang/LLVM の LTO を vmunix に安全に適用する（優先度高め） | completed | 4 platform の vmunix は既定で full LTO（HAL を含む）。実機はユーザー |
| [WS054](ws054/ws.md) | MG004 | UFS の directory を複数の block に育てる（BUG-038） | completed | 直接の 12 block まで。実機はユーザー |
| [WS055](ws055/ws.md) | MG001 | zedBSD の clang が link に `--undefined-version` を既定で渡す（F-009） | planned | p001 から。WS054 の後 |
| [WS056](ws056/ws.md) | MG002 | POSIX の試験と utility の不具合を直す（BUG-034・035・037、実行中に見つけた BUG-042〜044） | incomplete | p002 cleared（v3 の設計）、p003 実装中（q441）。p001 uncleared（BUG-034・035・037・042・043・044 resolved。残りは `POSIX-R2.ELF` の console の EINTR = BUG-046 だけ。ユーザーの判断待ち） |
| [WS057](ws057/ws.md) | MG004 | 仮想メモリの reserve と commit の分離と commit の swap の裏打ち（over commit 禁止）の確認と修正（design policy 10） | completed | 分離と拒否は実装済み、BUG-048 を修正。裏打ちは物理 + swap のまま（ユーザーの決定） |
| [WS058](ws058/ws.md) | MG004 | cache の大きさを現代の機械向けに見直す（主記憶 4 GB・swap 16 GB 前提、design policy 10） | completed | p001・p002 cleared。buffer 物理/8、page cache 物理/2、object cache 256、file 2048、inode 2048、overlay 4096、I/O pool 64 MiB。8192 級は F-013（動的確保と hash）の後 |
| [WS059](ws059/ws.md) | MG004 | disk の無い mount にも `st_dev` を与える（BUG-047） | completed | p001 cleared。`mount_device_number()`。`df` が全 mount を出す |
| [WS060](ws060/ws.md) | MG004 | UFS の journal の commit を batch にして名前の操作を速くする（BUG-040）。journal を既定にする前提（WS063） | incomplete | p003 cleared（v3: 200 の作成 20.6 → 0.36 秒、crash の試験 UFS OK）。規約は WS063-p002 |
| [WS061](ws061/ws.md) | MG002 | expat の configure と compile を Linux と同等の水準にする（fg011） | incomplete | 受け入れの計測は達成（q449 の後）: configure 8.2〜8.9 秒（host 10.7）、make（直列）11.3 秒（host `-j1` 15.5）、`cc t.c -o t` 76〜84 ms（host 83〜85）。残り: 規約の Phase ws061-p011（最後） |
| [WS062](ws062/ws.md) | MG004 | amd64 の disk image を ESP の vmunix・UFS の root partition・swap partition に（2026-09-25 ユーザー指示） | incomplete | p001・p002 cleared（q436・q437）。p003（既定の切り替え）q439 提案 |
| [WS063](ws063/ws.md) | MG004 | UFS の journal を既定にする（journal の無い image は mount の時に作る、`nojournal`）（2026-09-26 ユーザー指示） | incomplete | 既定の有効化・作成・`nojournal` は ws060-p003 で入れ、root の強制終了の試験は UFS OK。残り: p001（v2 の tail の volume）、p002（規約と回帰） |
| [WS064](ws064/ws.md) | MG002 | base の make の並列（`-j`）と、並列の make の時間を host と同等以上に（2026-09-26 ユーザー指示） | incomplete | p001・p002・p004 cleared（`-j`・jobserver、sh の posix_spawn。`make -j4` 4.18〜4.38 秒・host 5.14〜5.18 秒）。残り: 規約 p003（最後） |
| [WS065](ws065/ws.md) | MG002 | `/bin/sh` に POSIX が未規定とする bash 拡張を足す（2026-09-26 ユーザー指示） | incomplete | p001（構文）・p002（展開）・p003（builtin）cleared。p004（規約）は最後 |
| [WS067](ws067/ws.md) | MG002 | `/dev/fd` を呼んだ process の descriptor に合わせる（BUG-054、2026-09-26 ユーザー「最優先」） | completed | BUG-054 resolved（QEMU）。p001・p002 cleared |
| [WS066](ws066/ws.md) | MG002 | 動的 link の program の起動を速くする（`ld.so` の最適化）（2026-09-26 ユーザー「あとでやるリスト」） | planning | p001（費用の内訳と設計）。優先度は低い |

完了した WS の Phase の記録は 2026-09-24 に plan から削除した（git の履歴に残る）。

## WS の優先順位

依存による実行順とは別のもの。Queue の権限は変えない。

0. **WS067**（BUG-054、2026-09-26 ユーザー「最優先」）→ **WS062**（p003: disk image の既定を native に）。WS061・WS064・WS065・WS063 の規約の Phase は問題が出たときだけ（同日のユーザー指示「問題が生じなければ後回しでOK」）。
1. **WS035**（fg010: Wayland デスクトップ）。zdesktop の合成（p051 の承認 → p052〜p055）、タイトル・フレーム（p025）、タスクバー（p013）、
   文字の libtruetype 化（p027）。GPU の土台の問題は WS014・WS031 で直す。
2. **WS014・WS031**（デスクトップが使う GPU の土台。fg010 で必要になった分）。
3. **WS046**・**WS054**・**WS055**（2026-09-24 ユーザー決定「すべて承認します。」: WS054 の実装（p002〜p004）と、zedBSD の clang の既定に `--undefined-version`（WS055）。WS054 は WS046 の coreutils の build が待つ UFS の不具合 BUG-038 を直す。WS046 は 2026-09-24 ユーザー指示で優先度を上げた sh の互換性（WS042、完了）と POSIX の utility（WS043、完了）を実 package で使うための GNU 互換の make）。GNU 拡張（WS045）はその後。
4. **WS036**（aarch64 は主対象。LLVM の AArch64 zedbsd target と sysroot（p026）、起動 parameter（p027、HAL 承認待ち））。
5. **WS034**（アプリの導入）、WS005・WS033（有線 LAN）。
6. **WS049・WS050・WS051・WS052**（2026-09-24 ユーザー指示で追加: ACPI の AML interpreter、UCSI、DP Alt Mode、S0i3。WS049 が他の 3 つの前提。優先度はユーザーの指示を待つ）。
7. その他（WS001、WS004、WS007、WS009、WS017、WS026〜WS029）。WS037〜WS039 は番号の予約のみ。
8. **WS066**（`ld.so` の最適化。2026-09-26 ユーザー「あとでやるリスト」）。

## Upcoming Work Outlook

見込みであって、約束や実行許可ではない。

| 候補 | 理由 | 準備 |
| --- | --- | --- |
| ws056-p001 の clear か p002（console の `POSIX-R2.ELF` の EINTR、BUG-046） | 残りは 1 点 | ユーザーの判断 |
| ws046-p012 private の mapping の共有の設計（BUG-033 の残り） | configure が 96 秒でまだ host の数十倍 | p011 の差分と失敗の再現手順がある |
| ws046-p013 libc の mount の一覧の API | coreutils の cross build の残り | ioctl は kernel にある |
| ws035-p052 合成の 2 つのモードの核 | fg010 の中心 | **p051 の設計の承認待ち** |
| ws035-p025 タイトル・フレームの描画 | fg010 | 前提は揃っている |
| ws035-p013 タスクバーと WiFi | fg010 | p025 の後 |
| ws035-p027 文字の libtruetype 化 | fg010 | p025 の後 |
| ws036-p026 AArch64 の LLVM target | aarch64 は主対象 | 前提は揃っている |

## Tools

回帰と観察の道具は `plan/tools/` に置く。完了した WS の試験は、ここへ移したもの以外を削除した。Phase に固有の試験は各 WS の `tests/` にある。

| tool | 用途 | 使い方 |
| --- | --- | --- |
| [boot-test.sh](tools/boot-test.sh)（`boot-test.py`） | 起動の確認。OVMF の USB（amd64）か BIOS の IDE（i386）で起動し、画面を QMP で撮って login prompt を読む | `plan/tools/boot-test.sh [IMAGE]`。`OUTPUT`（既定 `build/boot-test`）、`BOOT_TIMEOUT`、`BOOT_MODE=uefi-usb` か `bios-ide` |
| [pc98-boot.py](tools/pc98-boot.py) | pc98 の起動の確認（`boot-test.sh` に PC-98 の mode が無いため）。PC-98 fork の QEMU で起動し、text VRAM で login prompt を読み、root で login して `uname -a`。画面を text と PNG で残す。WS053 から移した | `pc98-boot.py ~/qemu-pc98/build/qemu-system-i386 IMAGE OUTPUT`（`clock/pc98-sleep.py` の `Guest` を使う） |
| [guest/guest.sh](tools/guest/guest.sh) | SSH による guest の操作（USB CDC-ECM、KVM）。コマンドの実行・file の送受・lldb・kgdb・画面 | `start IMAGE`・`wait`・`run CMD`・`put`・`get`・`lldb`・`kgdb`・`screenshot`・`stop`。image は `extra-files` の出力を eval して作る。`GUEST_RUNTIME=<dir>` で別の guest を並べて動かせる（既定 `build/guest`） |
| [guest/serial.py](tools/guest/serial.py) | シリアルの console と対話する（sshd が上がる前。`CONFIG_PCAT_SERIAL_MIRROR=y`） | `serial.py --socket S run 'CMD'`（終了状態を返す）、`login` |
| [qmp.py](tools/qmp.py) | QMP の command を送る | `qmp.py SOCKET quit` など |
| [latency/](tools/latency/) | interactive の応答の測定（起床の遅れ、端末の echo）。WS041 から移した | `run-echo-qemu.sh`、`run-wakebench-qemu.sh`、`pc98-wakebench.py`、`config-*-bench.mk` |
| [clock/](tools/clock/) | guest の時計の進み（`sleep 5` の実時間）。WS040 から移した | `clock-check.py`、`pc98-sleep.py` |
| [ufs/](tools/ufs/) | UFS の directory の試験と volume の検査。`dir-grow.sh` は mount した volume で directory を 12 block まで育て（作成・削除・rename・rmdir・上限）、`verify` で確かめる（`LONG`・`SHORT`・`MOVE`・`GONE` で数）。`check-volume.py` は guest が書いた volume を host で fsck 相当に検査する。`crash-test.sh` は journal の volume の成長の途中で QEMU を止めて replay を確かめる。WS054 から移した | `sh dir-grow.sh DIR make\|verify`（guest）、`check-volume.py IMAGE`、`crash-test.sh IMAGE SECONDS...`（host）。作業の volume は `zedimage-host ufs SIZE EMPTYDIR IMAGE --inodes=16384 [--profile=journal-snapshot]` で作り、NVMe（`-device nvme`）でつなぐ |
| [kbench/](tools/kbench/) | kernel の microbenchmark（system call、pipe の往復、fork、exec、cached の read、anonymous と file の fault）。kernel の build（LTO・最適化）の比較に使う。WS053 から移した | `kbench/build.sh BUILD OUTPUT`（amd64 の guest 用）で作って guest で `kbench [file]`。予熱の 1 回の後に数回走らせ、中央値で比べる |
| [driver-fragments/prepare.py](tools/driver-fragments/prepare.py) | 統合した driver の source から host 試験用の断片を切り出す（出力は `build/driver-fragments`）。WS025 から移した | WS004 の AX211・xHCI と WS001 の UFS の host 試験が呼ぶ |
| [packages/](tools/packages/) | 外部 package の試験: ライセンス監査、未解決 symbol、取得機構とクロスビルドの host 試験。WS032 から移した | `audit-licenses.sh`、`check-unresolved-symbols.py`、`run-external-host-test.sh`、`run-cross-host-test.sh` |
| [menuconfig-target-host-test.py](tools/menuconfig-target-host-test.py) | menuconfig の target の選択の host 試験。WS020 から移した | `make menuconfig-host-test` |
| [boot-parameter-image-tool.c](tools/boot-parameter-image-tool.c) | image の boot parameter の読み書きと、pc98 の text VRAM の解読（`decode-pc98-vram`）。WS003 から移した | WS005・WS013 の試験が compile して使う |
| [venus-console.c](tools/venus-console.c) | Venus の console の試験 client。WS030 から移した | WS014 p009 の試験が build する |
| [sync.py](tools/sync.py)（[README](tools/README.md)） | GitHub との同期（GitHub mode） | `plan/tools/README.md` |
| sh の試験（[tools/sh](tools/sh/)） | `/bin/sh` を dash と比べる（oils の spec と自前の case）。guest では 40 件ずつ。対話（serial console）と行編集（host の pty） | `build-host-sh.sh`、`sh-diff.py --shell build/ws042/host-sh`（`fetch-oils.sh` で oils を取得）。guest は `--export build/ws042/guest-export` の後 `guest-batches.sh`（中で `guest-diff.sh`）。対話は `sh-interactive.py SOCKET`、行編集は `vi-host.py build/ws042/host-sh` |
| utility の差分試験（[tools/utils](tools/utils/)） | base の utility を GNU（POSIX mode）と比べる（`cases/` の 484 件、guest へは `--export` と `plan/tools/sh/guest-diff.sh`）。実際の configure（expat・coreutils）を GNU の道具と我々の道具で走らせて生成物を比べる。libc の浮動小数の書式を glibc と比べる | `build-host-utils.sh`、`util-diff.py`、`configure-diff.sh`、`float-format.c`。書き直しの前後の ls の比較は `ls-compare.sh OLD NEW` |
| 規約の検査（[style-check.py](tools/style-check.py)） | `plan/coding-style.md` のうち機械的に確かめられる規則（条件の中の呼び出し、閉じ括弧の後の空行、段落の comment、入れ子の宣言、条件演算子、goto、前方宣言、comment の形、名前、複数行の本体の括弧） | `python3 plan/tools/style-check.py FILE... [--summary] [--rule NAME]` |

QEMU の不具合は log を読まずに、QEMU のデバッグ機能で解析する:

- **gdbstub**: `-S -gdb tcp::<port>` で止めて起動し、host の `gdb` で `target remote :<port>`。`vmunix` は strip されていない。
- **map**: link で作る `$(BUILD)/vmunix.map` で address から関数を引く（`-g` は付けない）。
- **monitor/QMP**: `info registers`・`info mem`・`info tlb`・`x/`・`xp/`・`pmemsave`。
- **trace**: `-d int,cpu_reset,guest_errors -D <file>`（例外と reset だけ）。

pc98 は QEMU の PC-98 fork（`~/qemu-pc98/build/qemu-system-i386`、`-M pc9821,pegc=off,coregraph=on`）で起動し、
`pmemsave 0xa0000 0x2000` で取り出した text VRAM を `boot-parameter-image-tool decode-pc98-vram` で読む。
回帰試験では GPU を使わず、標準 VGA の framebuffer で login prompt だけを確かめる。

guest の memory（2026-09-24 ユーザー決定「ゲストのメモリはamd64とarm64では8GBでテストしましょう」）: amd64 は 8 GiB（`plan/tools/guest/guest.py` の既定と
`boot-test.sh` の `uefi-usb`）。arm64 の QEMU raspi4b は board の model が 2 GiB しか受け付けない（`Invalid RAM size, should be 2 GiB`）ので 2 GiB（2026-09-24 ユーザー決定「raspi4bは2GBでOKです。」）。i386 は変えない。

## プロジェクト固有の情報

エージェントの守る規則は [AGENTS.md](../AGENTS.md) の「プロジェクトの規則」節にある。ここには計画に要る事実と決定を置く。

### 対象 platform（2026-09-24 ユーザー決定）

| platform | 位置付け | tick 周期 |
| --- | --- | --- |
| amd64 | **主対象**。デスクトップ・GPU・アプリケーション。fg010 のデモ | 1000 Hz |
| aarch64（rpi4 ほか） | **主対象** | 1000 Hz |
| i386（pcat・pc98） | デモ用のおまけ。基本のコマンドと Xzed が動けばよく、性能は考えない | 100 Hz |
| sparcv9（sun4u）、m68k（x68k） | サポート外。コードは残す | 100 Hz |

tick 周期は `include/hal/arch/<arch>.h` の `HAL_TIMER_FREQUENCY`。時間の計算は `kern_ms_to_ticks()`・`kern_ticks_to_ms()`・
`KERN_MS_TO_TICKS()` で行い、tick の数を数字で書かない（WS040）。

### 2026-09-24 のユーザーの判断（有効なもの）

| 項目 | 決定 | 記録先 |
| --- | --- | --- |
| autotools の package | package ごとに patch する | WS034 |
| audiod | unix socket の interface。`shm_open` の直後に `shm_unlink` した fd を SCM_RIGHTS で渡す共有メモリ。`/dev/dsp` の OSS の mmap に対応 | ws035-p050・p049・p009 |
| デスクトップの合成 | 設計を出し、ユーザーが微調整して承認する。2 つのモード（全画面は scanout、ウィンドウは Vulkan で合成）、D1 は swapchain、wl_shm は CPU copy で補助 | ws035-p051 |
| epoll・timerfd・signalfd | POSIX の範囲で Wayland を作れるか調べる | ws034-p050 |
| git の package | `NO_RUST=1` でよい | ws034-p009 |
| `FD_SETSIZE` | 1024 | ws034-p048 |
| HDA の実機確認 | ユーザーが後で USB boot のベアメタルで試す | ws035-p008 |
| 動かない試験 | 書き直さず削除する | ws034-p049 |
| `/bin/sh` の互換性 | 優先度を上げて徹底的に直す | WS042・WS043（完了） |

### 主な依存関係

- WS046（make）→ guest での expat の build（WS042 の残り）。
- ws035-p051（承認）→ p052〜p055・p057（合成）→ fg010。
- WS014・WS031（GPU の土台）→ WS035 の合成とアプリ。
- WS049（AML）→ WS050（UCSI）→ WS051（DP Alt Mode、i915 の display も要る）。WS049 → WS052（S0i3）。
- WS036 p026（AArch64 の LLVM target）→ aarch64 の userland と package。

### 参照資料

- [設計方針・決定の参照資料](master-design-policy.md): 独立実装・ライセンス境界、module の設計、toolchain、個別の設計判断。
- [コーディング規約](coding-style.md)、[Guardrail](guardrail.md)、[Awesome Plan の設定](config.md)。
