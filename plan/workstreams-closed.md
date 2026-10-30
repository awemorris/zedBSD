# 完了・取り下げた WS

completed・canceled・closed の WS の一覧。未完の WS は [master.md](master.md)。状態の本体は各 ws.md、ここは写し。
WS が完了・取り下げになったら、行を master.md からここへ移す（番号の順）。完了した WS の Phase の記録は plan から削除してあり、git の履歴に残る。

| WS | Primary | 内容 | 状態 | 再開点・結果 |
| --- | --- | --- | --- | --- |
| [WS002](ws002/ws.md) | MG005 | システムサービス | completed | — |
| [WS003](ws003/ws.md) | MG003 | 旧実機 bring-up（終了・再利用禁止） | completed（ユーザー判断で終了） | 未完了は WS027・WS028・F-004 へ |
| [WS004](ws004/ws.md) | MG003 | ハードウェア拡張 | completed（2026-10-27 精査 q1608） | 実機の確認は未実施。p034（QEMU の ECM、test-wait）、p035（deferred）・p039（任意の共通化） |
| [WS005](ws005/ws.md) | MG005 | ネットワーク・WLAN | completed | 2026-10-08 ユーザーの判断（記録のミス、完了済み） |
| [WS006](ws006/ws.md) | MG006 | 入力と evdev | completed | — |
| [WS007](ws007/ws.md) | MG006 | グラフィックス・デスクトップ（旧） | canceled（2026-10-05） | p004 の再現条件、amd64 の残件 |
| [WS008](ws008/ws.md) | MG006 | Noct と BeUI | completed | — |
| [WS010](ws010/ws.md) | MG001 | Noct の script と build tool | completed | — |
| [WS011](ws011/ws.md) | MG005 | ネットワーク設定 console | completed | — |
| [WS012](ws012/ws.md) | MG005 | サービス管理 console | completed | — |
| [WS014](ws014/ws.md) | MG006 | GPU framework・virtio-gpu・Wayland の土台 | closed（2026-10-13 ユーザー） | p004（最終 API と規約の確認） |
| [WS015](ws015/ws.md) | MG007 | μITRON リアルタイム領域 | canceled（2026-10-05 ユーザー） | 昇格まで再開しない |
| [WS016](ws016/ws.md) | MG004 | 実行時の swap 制御 | completed | — |
| [WS017](ws017/ws.md) | MG006 | LFB 描画の高速化 | canceled（2026-10-05 ユーザー） | mmap・Xzed の高速描画 |
| [WS018](ws018/ws.md) | MG008 | kernel の source 所有と interface の統合 | completed | — |
| [WS019](ws019/ws.md) | MG003 | インストールとディスク管理 | completed | — |
| [WS020](ws020/ws.md) | MG003 | Intel Mac の UEFI 起動 | completed | — |
| [WS021](ws021/ws.md) | MG001 | x86 LLVM toolchain と sysroot | completed | — |
| [WS022](ws022/ws.md) | MG002 | ELF の TLS | completed | — |
| [WS023](ws023/ws.md) | MG001 | x86 HAL の規約準拠 | completed | — |
| [WS024](ws024/ws.md) | MG004 | 64-bit UFS の一本化 | completed | — |
| [WS025](ws025/ws.md) | MG004 | I/O・cache・物理メモリの再設計 | completed | — |
| [WS027](ws027/ws.md) | MG008 | PowerPC 移植 | canceled（2026-10-05 ユーザー） | p001〜p007 |
| [WS028](ws028/ws.md) | MG003 | インストーラの実機動作（4 機種） | canceled（2026-10-05 ユーザー） | NVMe の未動作の切り分け |
| [WS029](ws029/ws.md) | MG006 | i915 native GPU driver | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | cold VFIO attach の間欠的な停止ほか |
| [WS030](ws030/ws.md) | MG006 | 標準 Vulkan 1.0 と直接表示 | completed | — |
| [WS031](ws031/ws.md) | MG006 | i915 native Vulkan 実行器 | completed（2026-10-27 00:09 ユーザーの判断、未完の Phase は取り下げ） | — |
| [WS032](ws032/ws.md) | MG002 | 外部 package のクロスビルド（clang・OpenSSL・OpenSSH） | completed | — |
| [WS033](ws033/ws.md) | MG005 | networking サービスと有線インタフェースの管理 | completed（2026-10-27 精査 q1608） | 実機の確認は未実施、規約の全文は一括の見直しへ |
| [WS034](ws034/ws.md) | MG002 | アプリケーション拡充と kernel・libc の是正 | canceled（2026-10-05） | package の導入 |
| [WS035](ws035/ws.md) | MG006 | デスクトップ環境とアプリケーション | completed | 2026-09-30 ユーザーの判断で閉じた（目標が 2026-09-23 のまま古く、ゴールが不明確）。p001〜p138: compositor・sessiond・greeter・lock・Keiland の app・audiod・起動の短縮ほか。Chromium は取り消し（ユーザー「独自にBrowserを書いているから」）。デモまでの仕上げは WS099。`plan/ws035/tests/` は共有の道具として残す（plan/tools への移動は後の整理） |
| [WS036](ws036/ws.md) | MG008 | amd64 の成果を他 platform へ（aarch64 を含む） | completed | 2026-09-27 完了（p021 全 platform の回帰と規約、p026〜p029、p027 は案 A: boot の parameter の parser を緩めた）。実機は未実施。toolchain の cache（zedbsd8）は 2026-09-27 に rev-0 へ upload 済み |
| [WS037](ws037/ws.md) | MG006 | **nvrtx**: NVIDIA RTX 2000 以降（Turing〜Blackwell、GTX 16xx を含む）の GPU driver。WS141 の vc4 と同じ進め方（文書が先、GPL の作業の文書は commit しない、定数の一括の改名、最後に類似の監査、GSP の firmware は userland/firmware）。Pascal 以前は範囲の外（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | P4m の間欠の失敗（T1-691、手当ては実機未確認）、`nvrtx.disp.own` の既定は未決、p009 規約は一括の見直しへ |
| [WS038](ws038/ws.md) | MG006 | Intel Arc dGPU（予約） | canceled（2026-10-27 00:18、WS295 の xe に含める） | — |
| [WS039](ws039/ws.md) | MG006 | AMD RDNA GPU（amdrdna） | completed（2026-10-27 精査 q1608） | p018（BUG-331）は ticket で、p019 の速さは WS291、p010 規約は一括の見直しへ |
| [WS040](ws040/ws.md) | MG008 | 時間の単位を tick 周期から導く | completed | — |
| [WS041](ws041/ws.md) | MG006 | 起きた thread の即時実行 | completed | — |
| [WS042](ws042/ws.md) | MG002 | `/bin/sh` の POSIX 互換性 | completed | — |
| [WS043](ws043/ws.md) | MG002 | base の utility を POSIX に（sed・grep・awk ほか） | completed | — |
| [WS044](ws044/ws.md) | MG008 | rpi4 を開発に使える形に（console の font、FAT32 の boot、lldb） | closed（2026-10-13 ユーザー） | p001 font・p002 FAT32 の boot partition（QEMU）・p005 cleared。p003（lldb）ほかは WS036 の agent。実機は未実施 |
| [WS045](ws045/ws.md) | MG002 | base の text utility の GNU 拡張（sed・awk・grep ほか） | completed | 2026-10-05 完了（GNU の差分 518/518・POSIX 1080/1080・amd64 の boot、試験は plan/tools/gnu-utils） |
| [WS046](ws046/ws.md) | MG002 | GNU 互換の make（autotools の出力を実行できる範囲。並列・jobserver は WS064） | completed（2026-10-27 精査 q1608） | 規約の全文は一括の見直しへ |
| [WS047](ws047/ws.md) | MG001 | build.sh と Noct による build system（TUI・kernel・base・packages を別の system に。Makefile は当面残す） | canceled（2026-10-05 ユーザー） | p001 調査と設計 |
| [WS048](ws048/ws.md) | MG008 | Raspberry Pi 4 の USB（PCIe・VL805 の xHCI・USB キーボード） | closed（2026-10-13 ユーザー） | p001〜p003 cleared（FDT、brcmstb の PCIe、firmware の mailbox と VL805 の firmware。host 試験と QEMU の起動、実機は未実施）。p004 cleared（承認済みの hal.h の差分 `hal_pmem_map_uncached` を適用、実機は未実施）。p005 は config の有効化が残り uncleared。2026-09-27 サブエージェント、main へ merge |
| [WS049](ws049/ws.md) | MG003 | kernel 内の ACPI AML interpreter | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001〜p006・p010〜p015 cleared（p006 kernel への統合: 承認済みの `acpi.rsdp` の差分を適用、amd64 の既定で ACPI の driver が起動、guest の `/dev/acpi` が host の dump と一致。2026-09-27 merge）。次は p007。ACPI はデスクトップが片付くかリミットが余るとき（2026-09-27 方針） |
| [WS050](ws050/ws.md) | MG003 | USB-C の UCSI driver | completed（2026-10-27 精査 q1608） | 規約の全文は一括の見直しへ |
| [WS051](ws051/ws.md) | MG006 | USB-C の DisplayPort Alternate Mode | completed（2026-10-27 精査 q1608） | S0ix の口（WS052 と一緒、ベータ3） |
| [WS053](ws053/ws.md) | MG001 | clang/LLVM の LTO を vmunix に安全に適用する（優先度高め） | completed | 4 platform の vmunix は既定で full LTO（HAL を含む）。実機はユーザー |
| [WS054](ws054/ws.md) | MG004 | UFS の directory を複数の block に育てる（BUG-038） | completed | 直接の 12 block まで。実機はユーザー |
| [WS055](ws055/ws.md) | MG001 | zedBSD の clang が link に `--undefined-version` を既定で渡す（F-009） | completed | 2026-09-27 完了: zedBSD の clang の linker に `--undefined-version` を既定で（LLVM の patch を zedbsd8 へ）。main の toolchain を zedbsd8 に切り替えた |
| [WS056](ws056/ws.md) | MG002 | POSIX の試験と utility の不具合を直す（BUG-034・035・037、実行中に見つけた BUG-042〜044） | completed | 2026-09-27 完了（p001・p002 cleared。BUG-046 は console の `POSIX-R2.ELF` 10 回連続 status 0 で閉じた、BUG-068・069 は WS073）。試験は plan/tools/posix/ |
| [WS057](ws057/ws.md) | MG004 | 仮想メモリの reserve と commit の分離と commit の swap の裏打ち（over commit 禁止）の確認と修正（design policy 10） | completed | 分離と拒否は実装済み、BUG-048 を修正。裏打ちは物理 + swap のまま（ユーザーの決定） |
| [WS058](ws058/ws.md) | MG004 | cache の大きさを現代の機械向けに見直す（主記憶 4 GB・swap 16 GB 前提、design policy 10） | completed | p001・p002 cleared。buffer 物理/8、page cache 物理/2、object cache 256、file 2048、inode 2048、overlay 4096、I/O pool 64 MiB。8192 級は F-013（動的確保と hash）の後 |
| [WS059](ws059/ws.md) | MG004 | disk の無い mount にも `st_dev` を与える（BUG-047） | completed | p001 cleared。`mount_device_number()`。`df` が全 mount を出す |
| [WS060](ws060/ws.md) | MG004 | UFS の journal の commit を batch にして名前の操作を速くする（BUG-040）。journal を既定にする前提（WS063） | completed | 2026-09-27 完了（規約は WS063-p002 で）。p001 は p002・p003 に置き換えて canceled |
| [WS061](ws061/ws.md) | MG002 | expat の configure と compile を Linux と同等の水準にする（fg011） | completed（2026-10-05 ユーザー「clearedにする：WS061」） | 受け入れの計測は達成（q449 の後）: configure 8.2〜8.9 秒（host 10.7）、make（直列）11.3 秒（host `-j1` 15.5）、`cc t.c -o t` 76〜84 ms（host 83〜85）。残り: 規約の Phase ws061-p011（最後） |
| [WS062](ws062/ws.md) | MG004 | amd64 の disk image を ESP の vmunix・UFS の root partition・swap partition に（2026-09-25 ユーザー指示） | completed | 2026-09-27 完了（p004: 規約の全文。zedimage-host の出力が同じ） |
| [WS063](ws063/ws.md) | MG004 | UFS の journal を既定にする（journal の無い image は mount の時に作る、`nojournal`）（2026-09-26 ユーザー指示） | completed | 2026-09-27 完了（p002: 規約の全文と回帰、crash の試験 v3・v2・root）。v2 の tail の journal は v2 のまま（2026-09-27 ユーザーが案 A で確定）。制限: transaction ごとの解放 block の追跡は 8192 まで |
| [WS064](ws064/ws.md) | MG002 | base の make の並列（`-j`）と、並列の make の時間を host と同等以上に（2026-09-26 ユーザー指示） | completed | 2026-09-27 完了（p003: 規約の全文、make・sh・kernel の lock・vmspace の fork・libc の posix_spawn。guest の make-diff 100/100、fork・vfork・posix_spawn の試験、expat の configure が同じ。時間は未測定） |
| [WS065](ws065/ws.md) | MG002 | `/bin/sh` に POSIX が未規定とする bash 拡張を足す（2026-09-26 ユーザー指示） | completed | 2026-09-27 完了（p004: 規約の全文、host の sh-diff と guest の expat の configure が同じ） |
| [WS066](ws066/ws.md) | MG002 | 動的 link の program の起動を速くする（`ld.so` の最適化）（2026-09-26 ユーザー「あとでやるリスト」） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | p001（費用の内訳と設計）。優先度は低い |
| [WS067](ws067/ws.md) | MG002 | `/dev/fd` を呼んだ process の descriptor に合わせる（BUG-054、2026-09-26 ユーザー「最優先」） | completed | BUG-054 resolved（QEMU）。p001・p002 cleared |
| [WS069](ws069/ws.md) | MG006 | zdesktop で X11 の app を動かす（単体の `zdesktop-x11server`、rootless、GLX）（2026-09-26・27 ユーザー指示） | completed | 2026-09-27 完了（q492）。zdesktop-x11server（rootless、窓は Vulkan、GLX）、BUG-057 の修正、Xzed はレトロ用に戻した。残りは F-021・F-024・F-030 |
| [WS070](ws070/ws.md) | MG006 | zdesktop の System Menu: client がメニューの意味を渡し、zdesktop が浮いたタイトルバーとシステムバーに描く（`xdg_toplevel_menu_v1`、libzdesktop で包む）（2026-09-27 ユーザー指示） | completed | completed（2026-09-27）: System Menu と Titlebar（MENU・CONTROLS・TABS）。残りは Future Work（F-042・F-043・F-045）、i915 実機は WS075 |
| [WS071](ws071/ws.md) | MG006 | zedBSD File Manager: Finder 風で zedBSD らしいファイルマネージャ（ホームのダッシュボード、サイドバー、タグ、Quick Look、System Menu）（2026-09-27 ユーザー指示、仕様案は ws071/spec.md） | completed | completed（2026-09-27）: zdesktop-files の最初の版（すりガラスの付箋の pane、タブ、titlebar の CONTROLS、context menu、PNG の thumbnail、DnD、configure_bounds）。残りは Future Work（F-032〜F-041・F-044）、i915 実機は WS075、窓の外への DnD は WS035 |
| [WS072](ws072/ws.md) | MG004 | write cached の UFS の format の lease（BUG-060）と、NVMe の timeout の後の回復で root の mount が ETIMEDOUT になる（BUG-059）（2026-09-27、サブエージェント） | completed | 2026-09-27 完了（p001 BUG-060: write cached の format の lease、p002 BUG-059: NVMe の timeout の後の再発行） |
| [WS073](ws073/ws.md) | MG002 | Bug Board のbug解消（2026-09-27の対象境界を保持）。2026-10-02のP8はWS073に限らず、mainが各bugの既存handling WS/Phaseを照合して配属 | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.3）で閉じた） | p045（BUG-135、UFS の namespace_lock と journal の commit の待ち）を修正（停止 10→2 回）。残りは WS131 p005・p006 の後 |
| [WS075](ws075/ws.md) | MG006 | i915 の高度化: 今日のデスクトップ（zdesktop の glass・backdrop のぼかし・タブ）とグラフィックス（GLES 2/3、GL 3.0〜3.2）を Latitude 5330 の i915 のネイティブ実行器で動かす（compiler の inlining・F-022・F-023 の不足、性能と安定）（2026-09-27 ユーザー「OpenGL 3.2が問題なければ、それ以降のOpenGLはいったん保留して、i915の高度化に進んでください。」） | completed（2026-10-27 精査 q1608） | p006・p007a/b（中断）、p017・p018・p022・p031（速さ）、p010 規約は一括の見直しへ |
| [WS076](ws076/ws.md) | MG002 | libc の libm を自前で正しく書き直す（src/libc、誤差 1 ulp 以内、fmod 等は正確）（2026-09-28 ユーザー「libmは独自に書いてください。libcのツリーに入れてください。」） | completed | 2026-09-28 完了（`src/libc/math/`、群 B は全件で正しく丸め、BUG-078 解決）。F-046・F-047 へ移管 |
| [WS077](ws077/ws.md) | MG001 | PC-98 の PCI を有効にする（BUG-024、2026-09-28 ユーザー「Bug024は、PCIを有効にします。」） | canceled（2026-10-05 ユーザー） | **優先度を下げた（2026-09-28 ユーザー「Bug024は優先度を下げます。」）**。p001（調査と設計）。HAL の差分は承認が要る。PC-98 の試験が要るので着手の前に確認 |
| [WS078](ws078/ws.md) | MG006 | Kei Operating System への名前の移行（2026-09-28 ユーザーの決定: OS の名前 Kei、カーネルの内部名 zedbsd、Keiland、`/bin/wayland`・`/bin/xserver`・`/bin/browser`、`KERN_` の接頭辞、ロゴは Kei の 3 文字） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | 2026-09-28: p002・p003・p006・BUG-080・retro への移動は済み、p004 はほぼ済み。残り: 注釈と log の名前、p005 |
| [WS079](ws079/ws.md) | MG006 | 手書きノート（Notes、筆圧 4096 段階の USB のペンタブレット、PDF に保存し編集の metadata を持つ）と PDF Viewer（scroll と page の swipe）、上の右端から左下へのスワイプで Notes を起動・最前面・全画面（2026-09-28 ユーザー） | completed（2026-10-27 精査 q1608） | L3 の実機のペン（機材なし、未実施） |
| [WS081](ws081/ws.md) | MG006 | touch の操作の質: 慣性のある scroll と、低い fps の安い touch panel の数式による補間・予測。HID の driver・compositor・ブラウザ（と Keiland の app）にまたがる計画をこの 1 か所で（2026-09-28 ユーザー） | completed（2026-10-27 精査 q1608） | p017・p007（L3）、p009 規約は一括の見直しへ |
| [WS083](ws083/ws.md) | MG006 | Vulkan Video の拡張（`VK_KHR_video_queue`・`video_decode_queue`・`video_decode_h264`）と i915 の VCS・MFX の対応、最初の目標は H.264 の decode（2026-09-28 ユーザー。OSC のデモには必須ではない） | completed（2026-10-27 精査 q1608） | 規約の全文は一括の見直しへ |
| [WS084](ws084/ws.md) | MG006 | i915 の firmware の画面の引き継ぎ（素の実機の UEFI の起動で GOP が点けた pipe を N1 で止めて driver のものにし、デスクトップを出す）（2026-09-29 ユーザー、main が実装） | completed（2026-10-27 精査 q1608） | — |
| [WS085](ws085/ws.md) | MG006 | Windows版WINQ-EMUのVenusでデスクトップを表示する（2026-09-29 ユーザー） | completed（2026-10-06 夜 ユーザー「WS085、WS088、WS114はComplete、はすでにできており、Completeにしてください） | p001 実行中。mapped blob scanoutでデスクトップを表示。SDL→仮想USB HIDタッチを実装しQMP 2指注入でメニューを確認。Files起動停止の共有画像通信を修正し開閉・再起動とTerminal同時起動を確認。物理タッチと表示所有者切替は未試験 |
| [WS086](ws086/ws.md) | MG002 | ls の出力を GNU ls と同じにする（端末なら既定で列、端末の幅）（2026-09-29 ユーザー） | completed | 2026-09-29 完了（端末で GNU と同じ列、GNU ls 9.7 と host 3362 件・guest 180 件で差 0、名前は常に UTF-8）。残りは F-055 |
| [WS087](ws087/ws.md) | MG002 | /bin/sh の対話の行編集: 矢印キーの履歴（BUG-103）と Tab の補完（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU・host）: 履歴の file（~/.sh_history）、矢印の履歴の上限、PS/2 の E0 の key の capability（矢印が届かなかった原因）、prompt の ~、Tab の補完（GNU Readline の名前で libedit に）。実機の確認と BUG-103 の resolved は実機の後 |
| [WS088](ws088/ws.md) | MG006 | Windows で動く Kei-nightly.zip を CI で配布する（元の zip を clang の cache と同じ Release `rev-0` に置いて再利用し、CI が hdd-image.img を入れる）（2026-09-29 ユーザー） | completed（2026-10-06 夜 ユーザー「WS085、WS088、WS114はComplete、はすでにできており、Completeにしてください） | 2026-09-29: p001 は draft（整理した元の zip 56 MB、LICENSES・THIRD-PARTY、DLL は MSYS2 と一致）。fork の commit と zip の中身の確認がユーザー待ち。次: p003 の準備 |
| [WS089](ws089/ws.md) | MG006 | 設定のアプリ（Settings: 左に項目の pane、右に設定、浮いたすりガラスの pane）（2026-09-29 ユーザー、デモの優先事項） | completed（2026-10-27 精査 q1608） | p018 の規約の全文は一括の見直しへ（2026-10-08 ユーザー「規約の整形…はベータ3」） |
| [WS090](ws090/ws.md) | MG006 | widget・control の共有 library（少なくとも慣性の smooth scroll、独自の部品）（2026-09-29 ユーザー） | completed（2026-10-27 精査 q1608） | p026 の層 3（任意の速さ）、p012 の規約は一括の見直しへ |
| [WS091](ws091/ws.md) | MG006 | 画像 viewer（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU）: `/bin/imageview`、PNG・JPEG（EXIF の向き）・GIF（動く）、fit・拡大・pan・pinch・慣性、前後の画像、全画面。Files からの起動は WS093、実機の確認は残り |
| [WS092](ws092/ws.md) | MG006 | text editor（simple）（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU）: `/bin/textedit`、共有の file chooser（libkeiland、KEILAND_VERSION 12）、touch・PRIMARY・clipboard・Files からの起動。実機は未実施。touch の drag での選択は無し |
| [WS093](ws093/ws.md) | MG006 | Files から app の起動（画像・text の double click、file の種類と app の対応）（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU）: Files の double click・Enter・double tap で png・jpeg・gif → Image Viewer、text 系 → Text Editor、html → Browser、pdf → PDF Viewer。Always Open With と Use System Default（`~/.config/keiland/open-with`）。実機は未実施 |
| [WS094](ws094/ws.md) | MG006 | desktop の file の icon（`~/Desktop`）（2026-09-29 ユーザー） | completed（2026-10-27 精査 q1608） | p009・p015（速さ、F-072）、p007 の規約は一括の見直しへ |
| [WS095](ws095/ws.md) | MG006 | IME（Wayland の input-method-v2・text-input-v3、単一の IME・複数言語、まず日本語、REmacs の辞書）（2026-09-29 ユーザー） | completed（2026-10-27 精査 q1608） | p010（条件つき）、p011 の規約は一括の見直しへ |
| [WS096](ws096/ws.md) | MG002 | Qt6（core・gui・widgets）の互換の書き下ろし（API の interface だけ、zlib）（2026-09-29 ユーザー、デモの後） | canceled（2026-10-05） | デモの後 |
| [WS097](ws097/ws.md) | MG002 | GTK4 の互換の書き下ろし（API の interface だけ、zlib）（2026-09-29 ユーザー、デモの後） | canceled（2026-10-05） | デモの後 |
| [WS099](ws099/ws.md) | MG006 | Keiland の compositor（zdesktop）のデモの基準: 窓の操作・App Home・Wiseview・全画面と最大化の解除・greeter から Log Out と Shut Down・すりガラスの上の文字の contrast・回帰の試験の全通過（2026-09-30 ユーザー、WS035 の後継。基準は ws.md） | completed（2026-10-27 精査 q1608） | p006（案）、p022 の規約は一括の見直しへ |
| [WS100](ws100/ws.md) | MG006 | system bar の音量: 右上の通知領域の音量の icon、音量の slider と mute、変えたときの確かめの音（2026-09-30 ユーザー。動画の再生はデモの後） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | 2026-09-30: L1 がそろった（A1〜A6、Settings の Sound の頁）。L2 は 5330 の実機（A7、ユーザー）。L3 の p008 cleared: 確かめの音の遅れは QEMU の guest の中で中央値 31〜37 ms（≤ 50 ms、合否は実機で）。kernel の fragment を小さくする直しは実機で 50 ms を超えたとき |
| [WS101](ws101/ws.md) | MG006 | GPU の compute: i915 の Vulkan の compute（dispatch・shared memory・barrier・atomic）、libglesv2 の GLES 3.1 の compute、Noct の自動並列化（accel_opengles）が 5330 の GPU で動く（2026-09-30 ユーザー、10/17 のデモまで、最優先ではない） | completed（2026-10-27 精査 q1608） | p017（3 倍）・p012・p018（素の 5330）、p013 の規約は一括の見直しへ |
| [WS102](ws102/ws.md) | MG006 | スクリーンキーボード: 右下の角の swipe で右側に flick の panel（英字・記号・日本語）、左下の角の swipe で下側に QWERTY と手書き（認識は stub）。compositor に直接（2026-09-30 ユーザー） | completed（2026-10-27 精査 q1608） | p011・p012（IME と組む変換）・p013（Windows の QEMU の touch）、p014 の規約は一括の見直しへ |
| [WS103](ws103/ws.md) | MG006 | compositor を libvulkan だけにする（GPU の UAPI の直の ioctl を無くす）（2026-09-30 ユーザー「規則にして今移す」、規則は Guardrail） | completed | 2026-10-01 完了（p001〜p007、q508〜q514）: V1〜V4 を満たす（QEMU の Venus と 5330 の passthrough、単独の実機の起動は未実施）。Linux・FreeBSD の backend は F-065。試験は plan/tools/gpu-boundary |
| [WS104](ws104/ws.md) | MG006 | Keiland の OS の境界の整理: desktop の公開の header を `userland/desktop/include/` へ、libkeiland と compositor の OS の部分を `zedbsd/` の module へ、install の path を macro に。zedBSD の振る舞いは変えない（2026-10-01 ユーザー「Linux移植を進めます」、WS105 の準備） | completed | q515〜q522 / A1〜A6 verified。全文規約と全必須回帰 PASS、Linux は WS105 へ |
| [WS105](ws105/ws.md) | MG006 | Keiland を Linux で動かす（`/opt/keiland`）: `make keiland-linux`、libvulkan-compat（独自の WSI から system の libvulkan へ chain）、compositor の Linux の module（KMS・evdev・linux-dmabuf・logind）、主な app、gdm、wpa_supplicant・ALSA（2026-10-01 ユーザー、F-065 の Linux の分） | completed | L1〜L9/最終source conformance verified、q538 finished。Linux host/ownDebian13guest・zedBSD回帰、BUG-125/127は未修正trackingのユーザー許可。GitHub publication pending、次の実装なし |
| [WS106](ws106/ws.md) | MG001 | base/desktop の test/probe/demo 30件を userland/tests/ へ移し、package/config/install と既存の動作を維持 | completed（2026-10-27 精査 q1608） | p003 の規約の全文・最終 boot は WS の外の一括の見直しへ |
| [WS107](ws107/ws.md) | MG006 | engine の source を libbrowser に所属させ、Wayland無し・標準Vulkan/抽象入力の component と browser shell を整備 | completed | B1〜B5 verified / q544、API v2/public Vulkan client/最終boot。GitHub deferred（2026-10-05 Q1 の担当。p172 で browser2 を取り込み済み） |
| [WS108](ws108/ws.md) | MG007 | CI で Debian13/Ubuntu26.04 の Linux Keiland .deb を別々に作成/検証/artifact保存 | completed | P1〜P5 / q549、2OS native deb＋QEMU runtime、CI/release定義。remote未実施 |
| [WS109](ws109/ws.md) | MG006 | Linux版の共通描画を利用した native FreeBSD15 Keiland、audio/network/WiFi backend | completed | q574 native実機build/install+全文規約、p008 user「完璧に動作しました」でF6受け入れ合格 |
| [WS110](ws110/ws.md) | MG006 | 通常compositor起動を既定にし--testingで試験用有限modeを明示 | completed | 2026-10-05 完了（引数なしで通常の session、試験は --testing、試験の script 233 file を置き換え、試験は plan/tools/compositor） |
| [WS111](ws111/ws.md) | MG006 | Linux/FreeBSD共通console keiland-desktop、GDMはdirect維持 | completed | q575/q576: 共通console launcher/両native install/全source確認、GDMdirect不変。--login検討のみ |
| [WS112](ws112/ws.md) | MG007 | Linux5種類のbinary packageを指定make/CIで作成しreleaseへ添付 | completed（2026-10-27 精査 q1608） | p007 の規約の全文は WS の外の一括の見直しへ |
| [WS113](ws113/ws.md) | MG006 | zedBSD i915 hotplug/Vulkan Displayから複数画面・Settings/libkeiland・窓の全体移動 | completed（2026-10-27 精査 q1608） | p010（後回し、ユーザー）、p009 の規約の全文は一括の見直しへ |
| [WS114](ws114/ws.md) | MG006 | Linux標準GTK4互換性を調査し機能表レビュー後にXDG-shell/portal等を選択改善 | completed（2026-10-06 夜 ユーザー「WS085、WS088、WS114はComplete、はすでにできており、Completeにしてください） | p007（Linux 本物の GTK4 の CSD）・p008（KDE の server decoration、宣言の無い窓は SSD）cleared |
| [WS118](ws118/ws.md) | MG003 | Latitude 5320 で Kei を動かす（LCD の制御の不具合、sshd の遠隔 log 用 image、ユーザーと実機） | closed（2026-10-13 ユーザー） | p001（遠隔 log の image A は QEMU で SSH・collect まで）。実機は RTL8156 の USB の LAN でユーザーと |
| [WS120](ws120/ws.md) | MG006 | 音楽アプリ（fg019） | completed（2026-10-13 ユーザー「完了の記録だけが残る物: WS120・WS168・WS178・WS187 →closeしてください。」） | — |
| [WS121](ws121/ws.md) | MG006 | Web ブラウザでのアクセラレーションつきのビデオ再生（fg019） | completed（2026-10-27 精査 q1608） | p003（Range の loader）は後回しのまま（browser 本体は WS074 の Codex のセッション） |
| [WS122](ws122/ws.md) | MG006 | 動画プレーヤアプリ（fg019） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | p001 |
| [WS123](ws123/ws.md) | MG006 | VA-API のライブラリ | canceled（2026-10-02 user、アプリが Vulkan Video を直接使う） | — |
| [WS124](ws124/ws.md) | MG002 | GNU Emacs の package（fg019） | canceled（2026-10-27 精査 q1608） | — |
| [WS125](ws125/ws.md) | MG002 | vim の package（fg019） | canceled（2026-10-27 精査 q1608） | — |
| [WS127](ws127/ws.md) | MG006 | Files のベータ1 のブラッシュアップ（最重点）（fg019） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001（棚卸し）cleared、p002（BUG-140・Move To・New Window・重ね表示の scroll bar・spring-loaded・PDF の thumbnail）は eject を除き済み。eject は WS132。標準 app の開発は WS131 の後 |
| [WS128](ws128/ws.md) | MG006 | 標準アプリ全般のベータ1 のブラッシュアップ（fg019） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001（棚卸し、候補 C1〜C15）・p002（Notes の Open・Save As）・p003（Text Editor の Replace・Open Recent）cleared。標準 app の開発は WS131 の後 |
| [WS129](ws129/ws.md) | MG007 | ベータ1 のリリース作業（版・release notes・既知の問題・CI の release・最終回帰）（fg019） | completed（2026-10-27 精査 q1608） | — |
| [WS130](ws130/ws.md) | MG005 | IPv6 の network stack（ベータ1 は計画だけ、実装はベータ2 以降。DHCPv6 は `dhcpc -6`） | completed（2026-10-27 精査 q1608） | p009 の規約の全文は一括の見直しへ（2026-10-08 ユーザー「規約の整形…はベータ3」） |
| [WS131](ws131/ws.md) | MG006 | libkeiland を GUI toolkit 兼 desktop 機能の抽象化層にする（app の窓の作成を含む GUI の構築を共通化） | completed（2026-10-27 精査 q1608） | p024 の規約の全文と 3 OS の回帰は一括の見直しへ |
| [WS132](ws132/ws.md) | MG006 | /dev/system の電源管理と PnP の通知（subscriber が事象を指定）、自動 mount、Files の eject | completed（2026-10-27 精査 q1608） | p008 の蓋の自動の動作は WS052（Sleep、ベータ3）と一緒、p006 の規約は一括の見直しへ |
| [WS133](ws133/ws.md) | MG003 | 安定版 S1 の実機試験（安定版の image を実機で起動し SSH で複数の試験を詰め込む。最初の項目は ws005-p020・p024 から移した WiFi） | completed（S1、2026-10-03。2026-10-04 の UAT は WS の外で [uat.md](history/uat-20261004.md)・[uat/](uat/) に記録） | 安定版 S1 の内容と試験の一覧はユーザーと決める |
| [WS134](ws134/ws.md) | MG006 | システムモニターのアプリ（Analytic Spatial UI、中央の状態コア、層構造、2026-10-03 ユーザー） | completed（2026-10-27 精査 q1608） | p010 の規約の全文は一括の見直しへ |
| [WS135](ws135/ws.md) | MG006 | 設定の読み書きを libkeiland に一本化（libkeiland が直接か compositor の拡張で解決、監視と通知の API、desktop.conf は compositor の内部で session の開始・終了だけ読み書き。BUG-162、2026-10-03 ユーザー） | completed（2026-10-04） | — |
| [WS136](ws136/ws.md) | MG007 | 試験の image を「WS の tests/ の config.mk ＋個別の file の複写」に揃え、過去の build/ を入力にしない（2026-10-04 ユーザー） | completed（2026-10-04） | — |
| [WS137](ws137/ws.md) | MG006 | FreeBSD の試験の VM（QEMU+KVM）を T1・T2 で使えるようにし、libkeiland-backend の FreeBSD の build と試験を流す（2026-10-04 ユーザー） | completed（2026-10-04） | — |
| [WS138](ws138/ws.md) | MG006 | 背景の画像を PPM から PNG に（F-071。起動の decode は prefetch の thread へ） | completed | 2026-10-05 完了（PNG・JPEG の背景、PPM の読みを削除、試験は plan/tools/wallpaper） |
| [WS139](ws139/ws.md) | MG006 | desktop の性能の台帳と改善（F-072。開発の host の Venus は lavapipe の CPU、E2 の 5330 の測定は未実施） | closed（2026-10-13 ユーザー） | — |
| [WS140](ws140/ws.md) | MG002 | ld.so の依存・object・handle の上限を動的に（F-070。main は 32・16 のまま） | completed | 2026-10-05 完了（ld.so の上限を動的に、amd64、試験は plan/tools/rtld） |
| [WS141](ws141/ws.md) | MG006 | Raspberry Pi 4 のグラフィックス driver（VideoCore VI: HVS・pixelvalve・HDMI と V3D 4.2）。Linux の vc4・v3d の初期化の順と command の順を先に文書にし、i915 の書き換えを手本に我々の interface へ。framebuffer に段の印。Linux の vc4・v3d は GPL なので code は写さない（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | 描画の速さの残り（p013・p015・p016・p018・p019）は WS291、p017 の線の目視と Pi 4 実機は未実施、p001〜p006 は in-progress のまま閉じた |
| [WS142](ws142/ws.md) | MG006 | デスクトップのアプリの切り替え: Windows キーでアプリの一覧、タッチパッドの端からの 2 本指（Wiseview・仮想デスクトップ）、上部のバーのアプリの一覧とプレビュー、3 本指のタップ・Alt+Tab の切り替え（2026-10-04 ユーザーの要望） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001〜p006 cleared（QEMU）。5330 の UAT 待ち（実機は第 1 段の外） |
| [WS143](ws143/ws.md) | MG006 | Bluetooth（Settings の stub の頁の実体、HCI・daemon・desktop の経路）。ベータ2 の実装の項目、時期は未定（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | p003・p005 は in-progress のまま閉じた（実機は p008 で確かめた）、p009 規約は一括の見直しへ。A2DP・PAN は F-082 |
| [WS145](ws145/ws.md) | MG006 | 印刷: printer の daemon（IPP・LPD で PDF）、libkeiland の印刷の口と printer の一覧（compositor 経由、backend が daemon を起動）、Settings の Printers の頁（IP・port・protocol）、後に PostScript・vendor の filter（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | FreeBSD は build・install まで（印刷は未実施）、p006 規約は一括の見直しへ |
| [WS148](ws148/ws.md) | MG006 | Settings の Privacy の頁の検討（要らなければ頁を削除、要るなら設計と実装）（2026-10-04 ユーザー） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | — |
| [WS149](ws149/ws.md) | MG006 | Settings の Security の頁の検討（要らなければ頁を削除、要るなら設計と実装。WS148 の Privacy と項目を分け合う）（2026-10-04 ユーザー） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | — |
| [WS151](ws151/ws.md) | MG006 | Settings の Accessibility の頁の検討（要らなければ頁を削除、要るなら設計と実装。見る・聞く・操作・読み上げ）（2026-10-04 ユーザー） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | — |
| [WS154](ws154/ws.md) | MG006 | Settings の Languages の頁で IME を選ぶ（日本語・SKK・なし=英語）と、SKK の IME の新しい実装（辞書は Emacs の物）（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | SKK の >・/・#・Tab・注釈は F-079、規約の全文は一括の見直しへ |
| [WS155](ws155/ws.md) | MG006 | Keiland の app: カレンダー・スケジューラ・オーガナイザ（まず簡単な物）（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | p005 の規約の全文は一括の見直しへ |
| [WS156](ws156/ws.md) | MG006 | app の通知: 画面の下の中央を流れる headline の popup（右から中央、3 秒、左へ fade-out、× で消す）と hotkey の ring の log（すべて消去、個別に消した物は残さない）（2026-10-04 ユーザー） | completed（2026-10-27 精査 q1608） | 規約の全文は一括の見直しへ。FreeBSD の D-Bus の通知は範囲の外 |
| [WS157](ws157/ws.md) | MG006 | Keiland の app: 写真の管理（p001 は要件の検討）（2026-10-05 ユーザー） | completed（2026-10-27 精査 q1608） | p011〜p014・p016 は in-progress のまま閉じた（実装は main、UAT は OK）、Phone の picker の上の入力は未判定（T1-584） |
| [WS159](ws159/ws.md) | MG006 | native のタッチパッド（LPSS I2C・I2C-HID・HID の digitizer、evdev の MT）と compositor のタッチパッドの層（tap・tap-drag・押し込み・2 本指のスクロール）（2026-10-05 ユーザー「ACPI AMLを実装したあと、I2C-HIDを実装しましょう。compositorのtouchpad層も作りましょう。」） | completed（2026-10-27 精査 q1608） | p005 の規約の全文は一括の見直しへ |
| [WS160](ws160/ws.md) | MG002 | su・sudo・passwd（2026-10-05 ユーザー「su, sudoを実装してください。」・passwd も実装） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | なし |
| [WS161](ws161/ws.md) | MG006 | YubiKey のサポート（最初は USB の FIDO2、目標は NFC の CTAP2）（2026-10-05 ユーザーの追加） | completed（2026-10-27 精査 q1608） | NFC の実機は未実施、Linux・FreeBSD の FIDO2 は入れない（ユーザーの決定）、規約の全文は一括の見直しへ |
| [WS162](ws162/ws.md) | MG006 | FIDO2 の login（greeter・lock の画面で security key で login）（2026-10-05 ユーザーの追加） | completed（WS172・WS199 で達成）（2026-10-27 精査 q1608） | — |
| [WS163](ws163/ws.md) | MG006 | 数字 6 桁の login（PIN、greeter・lock の画面）（2026-10-05 ユーザーの追加） | completed（WS172 に吸収、WS172 p002 の PASS） | — |
| [WS164](ws164/ws.md) | MG006 | OS の起動時の Welcome の画面（既存の Start で足りるかの検討から）（2026-10-05 ユーザーの追加） | completed（2026-10-27 精査 q1608） | p003 の規約の全文は一括の見直しへ |
| [WS165](ws165/ws.md) | MG006 | 手書きの入力（goal の設定が難しいので段階化する）（2026-10-05 ユーザーの追加） | completed（2026-10-27 精査 q1608） | p004 の規約の全文は一括の見直しへ |
| [WS166](ws166/ws.md) | MG006 | IME の予測変換（2026-10-05 ユーザーの追加） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | — |
| [WS167](ws167/ws.md) | MG006 | GPU の command の protocol を Venus の番号の流用から独自の名前と番号に（Venus と一致する内容から始め、Venus の番号を再利用したことを header に書き、Google の著作権の表示を外せるようにする）（2026-10-05 ユーザーの追加） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | — |
| [WS168](ws168/ws.md) | MG006 | プレビュー（縮小表示）を作る隔離された専用の command（chroot・他の file の open・network・fork の禁止、fd 0・1 だけ）（2026-10-05 ユーザーの追加） | completed（2026-10-13 ユーザー「完了の記録だけが残る物: WS120・WS168・WS178・WS187 →closeしてください。」） | — |
| [WS169](ws169/ws.md) | MG006 | メーラの app と compositor のメールの API（許可された app が受信の通知を受ける、browser の認証 code の自動入力、IMAP4・SMTP から Gmail・Outlook へ）（2026-10-05 ユーザーの追加） | completed（2026-10-27 精査 q1608） | OAuth2 はユーザーの決定で作らない、規約の全文は一括の見直しへ |
| [WS170](ws170/ws.md) | MG006 | Phone の app（連絡先・SMS/MMS/RCS・VoIP を統合したタイムライン、compositor のメッセージの API、モデム・スマホの bridge の backend。最初は連絡先からタイムラインの表示まで）（2026-10-05 ユーザーの追加） | completed（2026-10-27 精査 q1608） | 本物の SMS・通話は WS197、規約の全文は一括の見直しへ |
| [WS173](ws173/ws.md) | MG006 | AAT（Agent Acceptance Test）: /dev/input-inject のマウスとキーボード、試験の image だけの画面の撮影、SSH の host の道具で、エージェントが素の 5330 を操作して受け入れを確かめる。UAT はデバイス系と使用感に絞り AAT の後に遅らせる（2026-10-05 ユーザー） | completed（2026-10-27 精査 q1608） | p005（素の 5330 の AAT）は未実施のまま閉じた |
| [WS174](ws174/ws.md) | MG003 | 起動時に Ctrl で kernel の message を console に、Shift で console の login に（UEFI の bootloader だけ、BIOS は後日）（2026-10-05 ユーザー） | completed（2026-10-27 精査 q1608） | p004 BIOS（後日、ユーザー）、実機の確認は未実施 |
| [WS175](ws175/ws.md) | MG006 | Notes で PDF の画像と文字を編集（移動・大きさ・差し替え・挿入、文字の編集・削除・挿入・font、複数頁）（2026-10-06 ユーザー） | completed（2026-10-27 精査 q1608） | 目視の確認は未実施、p011 の規約は一括の見直しへ |
| [WS177](ws177/ws.md) | MG006 | ベータ2 積み残し（準正常系・異常系） | completed（2026-10-27 精査 q1608） | p019 の残り（Browser、WS074 と一緒） |
| [WS178](ws178/ws.md) | MG006 | OpenGL を Desktop へ、GLX を xserver へ | completed（2026-10-13 ユーザー「完了の記録だけが残る物: WS120・WS168・WS178・WS187 →closeしてください。」） | — |
| [WS179](ws179/ws.md) | MG006 | UI のアクセントカラーを選べるように | completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）） | — |
| [WS181](ws181/ws.md) | MG006 | 窓の docked・floating・最小化の状態、App Home の独立のモード、gesture、整列のメニューと整列モード（UAT 2026-10-07） | completed（2026-10-27 精査 q1608） | p012 の 3 回目（page の送りの量）の QEMU は未実施 |
| [WS182](ws182/ws.md) | MG006 | 電源ボタンで Log Out・Shut Down などのメニュー（ベータ2 の最後、2026-10-07 ユーザー） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001 設計 |
| [WS183](ws183/ws.md) | MG003 | I2C HID の Extended Interrupt と TGL の GPIO の group（5320 の touchpad を割り込みで、ベータ2・後回し） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001 |
| [WS187](ws187/ws.md) | MG006 | ロック画面（大きな時計、下部から上へのスワイプ・wheel での解除、自動の lock の猶予、Password・PIN・Hardware Key の選択、2026-10-08 ユーザー） | completed（2026-10-13 ユーザー「完了の記録だけが残る物: WS120・WS168・WS178・WS187 →closeしてください。」） | — |
| [WS188](ws188/ws.md) | MG006 | app の OS の操作を libkeiland → compositor → backend へ（Settings の About・Storage・Users・Languages の残り）と境界の検査の強化（2026-10-08 ユーザー） | completed（2026-10-08 Q1 の判定（sweep-beta2-rc §2）: p001〜p004 cleared） | p001 設計 |
| [WS189](ws189/ws.md) | MG006 | app の間の drag and drop（画像、受け入れの見た目、dock の spring-loaded、desktop に file、画面をまたぐ）（2026-10-08 ユーザー） | completed（2026-10-27 精査 q1608） | T1 の残りの項目（across-displays・pdfviewer の drag-out・photo-to-notes 3〜4）は未確認 |
| [WS190](ws190/ws.md) | MG006 | 文字の欄と Text Editor の指での選択（ダブルタップ・端の drag・コピー・切り取り・貼り付け・すべて選択の popup）（2026-10-08 ユーザー、WS189 の後） | completed（2026-10-27 精査 q1608） | — |
| [WS191](ws191/ws.md) | MG006 | 再生の音を libkeiland の audio stream の口へ（compositor・backend、zedBSD audiod・Linux PipeWire・FreeBSD OSS）（2026-10-08 ユーザー） | completed（2026-10-27 精査 q1608） | — |
| [WS192](ws192/ws.md) | MG006 | 右上の状態の島を tap で開く glass の操作パネル（WiFi・音量・IME などを大きく、タブレット向け）（2026-10-09 ユーザー、ベータ2） | completed（ベータ2 の範囲、2026-10-10 Q1） | p001 設計と実装 |
| [WS193](ws193/ws.md) | MG006 | make menuconfig のメニュー階層の作り直しと Build boot image（進捗の bar、-j$(nproc)）（2026-10-09 ユーザー、ベータ2） | completed（2026-10-27 精査 q1608） | p003 の規約の全文は一括の見直しへ |
| [WS194](ws194/ws.md) | MG006 | make keiland-linux（apt・yum・pacman）・keiland-freebsd（pkg）の必要な package の確認と導入、build 後の install の確認（2026-10-09 ユーザー、ベータ2） | completed（2026-10-27 精査 q1608） | p003 の規約の全文は一括の見直しへ |
| [WS195](ws195/ws.md) | MG006 | zedBSD でも userland/desktop を /opt/keiland/ に、account-admin を base から Keiland へ（2026-10-09 ユーザー、ベータ3） | canceled（2026-10-13 ユーザー） | p001 設計 |
| [WS196](ws196/ws.md) | MG002 | useradd・usermod・userdel（2026-10-09 ユーザーの問い、ベータ3 以降の提案） | completed（2026-10-14） | p001 範囲 |
| [WS199](ws199/ws.md) | MG006 | Settings のセキュリティキーの独立の頁とウィザード（2026-10-10 ユーザーの UAT、ベータ2） | completed | p001 |
| [WS200](ws200/ws.md) | MG006 | Settings の Users のパスワード変更のウィザードと認証方式の選択（2026-10-10 ユーザーの UAT、ベータ2） | completed（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」） | p001 |
| [WS202](ws202/ws.md) | MG006 | libavcodec なしで libmedia だけで mp4（H.264 は Vulkan Video・AAC-LC は自前）と .m4a を再生（Video Player・Music）（2026-10-11 ユーザー、設計だけ、実装は別のセッション） | completed（2026-10-27 精査 q1608） | BUG-300（スライダー）は Bug で、p014 の規約は一括の見直しへ、実機の全項目は未確認 |
| [WS203](ws203/ws.md) | MG005 | Raspberry Pi 4 内蔵 Ethernet (BCM2711 GENET v5) | completed（2026-10-27 精査 q1608） | 実機で link・IPv4・SSH は確認（2026-10-11）、DHCP の lease の出所と抜き差しは未確認 |
| [WS206](ws206/ws.md) | MG005 | Raspberry Pi 4 の WiFi（2026-10-11 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認（scan・WPA2・DHCP）は未実施、p007 の規約は一括の見直しへ |
| [WS207](ws207/ws.md) | MG006 | Raspberry Pi 4 のオーディオ（再生・録音）（2026-10-11 ユーザー） | completed（2026-10-27 ユーザーの回答「completed」） | 3.5 mm の音（p002）は実装・host まで、実機は未確認。録音（p003）・HDMI（p004）は未実装 |
| [WS208](ws208/ws.md) | MG008 | arm64 の HAL の SMP（Pi 4 の 4 core）（2026-10-11 ユーザー） | completed（2026-10-27 精査 q1608） | p005 の reboot 10 回は未実施、p006 の規約は一括の見直しへ |
| [WS210](ws210/ws.md) | MG006 | Keiland の app の描画を GPU へ（libkeiland の canvas・文字・画像を Vulkan で）（2026-10-12 ユーザー） | completed（2026-10-27 ユーザー「これはすでにVF2での最適化で完了しています。completedにしてください。」） | VF2 での最適化で完了（ユーザー）。中間の p013・p014 は main、GPU の canvas の核（p003〜p012）は未着手 |
| [WS211](ws211/ws.md) | MG006 | Pi 4 の compositor の描画の削減（App Home の遷移のクロスフェード、draw の束ね）（2026-10-12 ユーザー） | completed（2026-10-27 精査 q1608） | p001・p003 の段 2 以降は未着手で終了、p004 の規約は一括の見直しへ（p002 のクロスフェードは main 4c3a56cf9） |
| [WS213](ws213/ws.md) | MG006 | compositor の thread 化（入力・cursor と描画の分離、重いコマの間も入力と cursor が止まらない）（2026-10-12 ユーザー） | completed（2026-10-27 精査 q1608） | p006 の規約は一括の見直しへ、box_lag の数値は未確認 |
| [WS214](ws214/ws.md) | MG006 | 窓をディスプレイの重ね合わせ（HVS の plane）に載せる（Pi 4、drag・dock の間 GPU が窓を描き直さない）（2026-10-12 ユーザー「ぜんぶやりましょう。」） | completed（2026-10-27 精査 q1608） | p005・p006（案、未投入）は終了、p002 の規約と類似の監査は一括の見直しへ |
| [WS215](ws215/ws.md) | MG005 | Raspberry Pi 4 の Bluetooth（内蔵の BCM43455、UART の HCI）（2026-10-12 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施。p008（SCO と HFP）は WS197 の HFP と一緒に（ベータ3 の段は 2026-10-27 に取り消し） |
| [WS216](ws216/ws.md) | MG006 | Notes の描く道具（筆・鉛筆・領域の塗りつぶし・水彩）（2026-10-13 ユーザーの UAT） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施、p008 の規約は一括の見直しへ |
| [WS218](ws218/ws.md) | MG005 | Intel WiFi（AX211）driver の license の表示を実態に合わせる（OpenBSD・Intel の license の削除の監査）（2026-10-13 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施 |
| [WS219](ws219/ws.md) | MG006 | Venus の protocol の op の流用の整理（定数名の付け直し、Venus の著作権表示を外し引用の明記に留める）（2026-10-13 ユーザー） | completed（2026-10-27 精査 q1608） | — |
| [WS220](ws220/ws.md) | MG005 | TCP の転送の速さの安定（継続の転送ですぐ 1 MB/s 未満に落ちる、window の制御の直し）（2026-10-13 ユーザー） | completed（2026-10-27 精査 q1608） | p002 の QEMU の確かめは未実施。p006・p007 は WS221 の p008・p009 へ移した（2026-10-28 ユーザー）、SACK は行き先なし。tests/ は残す |
| [WS224](ws224/ws.md) | MG006 | Vulkan の header の著作権表示の形（zedBSD の file header の下に Khronos の表示を別の block で、派生物、解説は入れない）（2026-10-13 ユーザー） | completed（2026-10-13） | Q1 |
| [WS227](ws227/ws.md) | MG002 | menuconfig の Base・Desktop の表示名の整理、画像と PDF の library を Desktop へ、Linux・FreeBSD に zedBSD 独自の base の命令（2026-10-14 ユーザー） | completed（2026-10-14） | — |
| [WS228](ws228/ws.md) | MG002 | 独自実装の簡単な vi を base に足す（2026-10-14 ユーザー、ベータ2 の最後の機能） | completed（2026-10-14） | — |
| [WS229](ws229/ws.md) | MG006 | i915 を Gen9・Gen9.5（Core の 6〜10 世代の iGPU）に対応（GOP のままで GT と EU → 最後に display controller）（2026-10-14 ユーザー、ベータ2 に入る、既定で有効） | completed（2026-10-27 精査 q1608） | 6・7 世代の実機は未確認、display controller は WS237・WS260 |
| [WS230](ws230/ws.md) | MG006 | i915 の GuC 対応（2026-10-14 ユーザー、ベータ3） | canceled（2026-10-27 04:13 Q1: 2026-10-27 ユーザー「i915のGuCは実装せず、xeドライバを開発するときにGuC対応を実装することにします。」、WS295 へ） | 未割当 |
| [WS231](ws231/ws.md) | MG002 | AHCI（SATA）の driver（2026-10-14 ユーザー、ベータ2 に入る） | completed（2026-10-27 精査 q1608） | NCQ・TRIM・hotplug・ATAPI は入れていない（行き先は未定）、実機の SATA は未確認 |
| [WS232](ws232/ws.md) | MG002 | menuconfig に i386（PC-9800・PC/AT）と Retro の menu（Xzed・zwm・zshell・zterm）（2026-10-14 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施、pc98 の QEMU の login の結果は記録に無い |
| [WS233](ws233/ws.md) | MG002 | firmware を awemorris/zedBSD-firmware から取る | completed（2026-10-27 精査 q1608） | p002 の license の画面の端末での目視は未実施 |
| [WS236](ws236/ws.md) | MG006 | Raspberry Pi 4（VideoCore VI）の動画の再生の hardware 支援 | completed（2026-10-27 精査 q1608） | 720p30 で shown 289/300（T1-690）、1080p・60 fps は範囲の外、seek・長時間は未実施 |
| [WS237](ws237/ws.md) | MG006 | i915 の Gen9・Gen9.5（第 6〜10 世代 Core）の display controller の制御 | completed（2026-10-15 ユーザー「Gen9は残りの安全策の作業をマージしたら終了です） | — |
| [WS238](ws238/ws.md) | MG004 | amd64 の native の image を ESP（loader だけ）・FAT32（kernel と boot の設定）・UFS の root・swap の構成にする | completed（2026-10-27 精査 q1608） | 実機の確認は未実施 |
| [WS239](ws239/ws.md) | MG002 | Wayland（compositor）の起動の失敗時に text console の login へ戻す | completed（2026-10-27 精査 q1608） | p002 の QEMU の確かめは未実施 |
| [WS241](ws241/ws.md) | MG006 | nvrtx（RTX）の描画の速さ（2026-10-15 ユーザー） | canceled（WS291 へ）（2026-10-27 精査 q1608） | WS291 p010・p011 |
| [WS243](ws243/ws.md) | MG006 | nvrtx の NVDEC で H.264（Vulkan Video）（2026-10-15 ユーザー） | completed（2026-10-30、P5 q1677-i02。RTX 2070 で H.264 の hash 4 本一致、Video Player の 1080p60 全画面を GPU の道で 584/596・late 12、hang の非退行） | 制限: i915・amdrdna の実機の Video Player は未実施、BUG-353 の glass の時間は未計測（ws.md の「制限・移管」） |
| [WS244](ws244/ws.md) | MG006 | i915 の Linux 由来の code の整理（Gen9 の display を guardrail に、GT・compiler・header・license）（2026-10-15 ユーザー） | completed（2026-10-27 01:54、ws.md を完了の形に、5330 の自動の回帰で判定） | — |
| [WS245](ws245/ws.md) | MG006 | i915 の Gen12 系（display 12・13、Skylake〜Raptor Lake Refresh の方針で 5330 を特別扱いしない）の display の書き直し（2026-10-15 ユーザー） | completed（2026-10-27 01:54、ws.md を完了の形に、5330 の自動の回帰で判定） | — |
| [WS246](ws246/ws.md) | MG006 | nvrtx の compiler を sm75.h・sm75.c にまとめて refactor（2026-10-15 ユーザー）、2026-10-22 ユーザーの指示で compiler/sm75/ の 14 file に分け直した（d73ca1f65） | completed（2026-10-27 精査 q1608） | p002 の latency の表への直しは残す、p003 の規約は一括の見直しへ |
| [WS247](ws247/ws.md) | MG006 | nvrtx の comment・log から開発の管理情報と出典の名前を除き、出典を block ごとに（2026-10-15 ユーザー） | completed（2026-10-15） | — |
| [WS248](ws248/ws.md) | MG005 | Intel AX211 の driver の出典の表記（block ごとの referenced:）と規約の整理（2026-10-15 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施 |
| [WS249](ws249/ws.md) | MG005 | BCM43455（Pi 4 の WiFi）の driver の出典の表記（block ごとの referenced:）と規約の整理（2026-10-15 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施 |
| [WS252](ws252/ws.md) | MG006 | i915 の RKL・ADL-S/RPL-S・ICL の GT の対応（2026-10-15 ユーザー、WS245 §7 #20） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施 |
| [WS253](ws253/ws.md) | MG001 | 起動の時間の短縮: 通常の起動は sessiond（Wayland）を先に、network・audio・sshd などは後に。debug の起動は今のまま（2026-10-16 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施、p002 の規約は一括の見直しへ |
| [WS254](ws254/ws.md) | MG001 | networkd: DHCP の失敗の後に 1 分おきに取り直す（2026-10-16 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施、p002 の規約は一括の見直しへ |
| [WS255](ws255/ws.md) | MG001 | 試験機（5330・RDNA 機）の zedBSD の試験の image を静的な IP に（2026-10-16 ユーザー） | canceled（2026-10-27 ユーザーの回答「取り下げ」、WS254 の DHCP の取り直しで足りる、BUG-328 は Bug に残す） | BUG-328 の直し |
| [WS256](ws256/ws.md) | MG008 | StarFive VisionFive 2（JH7110、RISC-V）への移植（2026-10-18 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認（音・coherency・PERST）は未実施、p018 の ssh |
| [WS258](ws258/ws.md) | MG001 | 全ドライバの出典の表記を guardrail に（file の先頭から block へ）（2026-10-18 ユーザー） | completed（2026-10-18、残りは WS244・WS259） | — |
| [WS259](ws259/ws.md) | MG005 | RTL8822B の表（rtl8822b-tables.inc）を値の引用に書き換える（2026-10-18 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施（license の package の directory は削除済み） |
| [WS260](ws260/ws.md) | MG006 | i915 の Gen9 の display（gen9-*.c）を xd と同じく自前で書き直す（2026-10-18 ユーザー） | completed（2026-10-27 精査 q1608） | 実機の確認は未実施 |
| [WS261](ws261/ws.md) | MG008 | include/kern/boot.h から PC-98・Raspberry Pi の固有の構造体を除く（2026-10-18 ユーザー） | completed（2026-10-18） | — |
| [WS262](ws262/ws.md) | MG001 | boot loader（bootloader/）をコーディング規約の全文に合わせる（2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS263](ws263/ws.md) | MG008 | riscv32 の virt（QEMU）への移植、framebuffer の console・VFS の段まで（2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS264](ws264/ws.md) | MG008 | ppc32 の QEMU の machine（「virt」相当）への移植、framebuffer の console・VFS の段まで（2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS265](ws265/ws.md) | MG008 | ppc64 の QEMU の machine（「virt」相当）への移植、framebuffer の console・VFS の段まで（2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS266](ws266/ws.md) | MG008 | m68k（68030）の virt（QEMU）への移植、framebuffer の console・VFS の段まで（2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS267](ws267/ws.md) | MG008 | sun4u（sparcv9）を再び build できるようにし、QEMU で動かす（2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS268](ws268/ws.md) | MG008 | x68k（X68000）を再び build できるようにする（起動は任意、2026-10-18 ユーザー） | completed（2026-10-19） | — |
| [WS269](ws269/ws.md) | MG008 | VisionFive 2 の GPU（PowerVR BXE-4-32）の driver（2026-10-19 ユーザー） | completed（2026-10-27 精査 q1608） | Phase ごとの実機の確かめは未実施（板の通しで代えた）、長時間の安定性・p017 の段階 B・C は未、速さは WS291（PowerVR の model 50 fps） |
| [WS270](ws270/ws.md) | MG008 | VisionFive 2 で Keiland の desktop を動かし、OS を完全に動かす（2026-10-19 ユーザー） | completed（2026-10-27 精査 q1608） | 重い 3D の速さは WS291（PowerVR）。VF2 の画面なしの実機（p004）は未実施 |
| [WS271](ws271/ws.md) | MG008 | MIPS32（mipsel）の QEMU malta への移植、framebuffer の console・VFS の段まで（2026-10-19 ユーザー） | completed（2026-10-19、QEMU malta の MIPS32 で VFS の段まで、T1-725・728・730・734） | — |
| [WS272](ws272/ws.md) | MG008 | MIPS64（mips64el）の QEMU malta への移植、framebuffer の console・VFS の段まで（2026-10-19 ユーザー） | completed（2026-10-19、QEMU malta の MIPS64R2-generic で VFS の段まで、512M の high の alias も、T1-731・733・734） | — |
| [WS274](ws274/ws.md) | MG001 | 仮想メモリの code（vm.c・vmspace.c など）を src/kern/vm/ に分けて整理する（2026-10-19 ユーザー） | completed（2026-10-19、src/kern/vm/ の 26 file、T1-741 まで PASS） | — |
| [WS276](ws276/ws.md) | MG008 | m68k（x68k・m68k-virt）の静的な program の TLS を kernel と libc で支える（2026-10-19、q1301 の所見） | completed（2026-10-19、m68k の静的な TLS、T1-741） | — |
| [WS277](ws277/ws.md) | MG008 | VisionFive 2 の DC8200 の display driver（zero copy の scanout）（2026-10-20 ユーザー） | completed（2026-10-27 精査 q1608） | U0〜U11 の行の確認は未実施 |
| [WS278](ws278/ws.md) | MG008 | Raspberry Pi 5 対応（config/ci/config-rpi5.mk、必要なら GPU・WiFi）（2026-10-21 ユーザー） | completed（2026-10-27 精査 q1608） | Phase ごとの実機の確かめは未実施（UAT の通しで合格）、BT（無効）、BUG-345 |
| [WS279](ws279/ws.md) | MG008 | Raspberry Pi 5 の GPU（V3D 7.1）と display（HVS6）（2026-10-21 ユーザー） | completed（2026-10-27 精査 q1608） | GPU の速さ（Master の優先順「Pi 5 の残り: ユーザーの指示の時」）、p009 の license・類似の監査は未実施 |
| [WS280](ws280/ws.md) | MG006 | H.264 の software decoder（libh264dec、libmedia から dlopen、任意）（2026-10-21 ユーザー） | canceled（2026-10-30 ユーザー「WS280は取り下げます。」） | WS326 が代わり |
| [WS286](ws286/ws.md) | MG008 | Digilent ZYBO（初代、Zynq Z-7010）への移植: ARMv7-A の HAL と JTAG の起動、PL（FPGA）の block を動かすまで（USB は外す） | completed | 2026-10-23 完了: 実機の ZYBO で JTAG の起動・UART の login・`fpgaload`/`fpgareg` で PL の block（ID・SCRATCH・COUNTER・LED・IRQ 61）。残り（PL310/L2、SMP の CPU1、armv7 の struct stat の詰めた timespec、ptrace の user VA の icache）は ws.md |
| [WS288](ws288/ws.md) | MG008 | FPGA の GPU の IP（display controller・command processor・blitter・vertex fetcher・rasterizer・shader unit・texture unit・ROP・memory controller）、ZYBO で workflow、本命は KC705 | closed（2026-10-26 22:57、ユーザーの指示。amaterrace-gpu は他の担当へ、zedBSD の driver は WS290） | 2026-10-24 ユーザーの決定で目標の板は KC705 だけ、RTL を揃えるのを優先（板は p024、timing は p025）。cleared p002〜p005・p007・p008・p012・p017・p023、in-progress p001・p006（branch）・p009。引き継ぎ [handover-20261024-2230.md](ws288/handover-20261024-2230.md) |
| [WS289](ws289/ws.md) | MG008 | ZYBO の単独起動: SD（SDIO0）・LAN（GEM0 + RTL8211E、SSH）・SD からの起動（BOOT.BIN）。root を SD に。SDHCI・Cadence GEM の共通の部分を汎用の driver に分ける | completed | 2026-10-23 completed。実機の ZYBO が SD の BOOT.BIN → SPL Falcon → FIT kernel で単独起動し、root は SD、LAN 1000 Mbps・DHCP・SSH、`deploy.sh` で kernel 更新と reboot ができる。汎用の SDHCI・Cadence GEM の core を Pi 4/Pi 5/ZYBO の glue に分けた。100/10 Mbps は未確認 |
| [WS293](ws293/ws.md) | MG006 | i915 の Vulkan の executor を render/ から vk/ に揃える（動作は不変） | completed（2026-10-27 01:39） | — |
| [WS294](ws294/ws.md) | MG006 | i915 の GuC による submission | canceled（2026-10-27 00:18、GuC は WS295 の xe で） | — |
| [WS298](ws298/ws.md) | MG003 | Intel VMD の driver（5330 の NVMe を BIOS を変えずに使う） | completed（2026-10-27 08:33） | NVMe から zedBSD が起動 |
| [WS119](ws119/ws.md) | MG003 | インストーラの作り直し | canceled（2026-10-28 ユーザー、Keiland のアプリの新しいインストーラ WS307 へ） | — |
| [WS308](ws308/ws.md) | MG004 | read/write の syscall の lock の取り合いを無くす（NVMe の command_lock、heap、timecounter、cred の読み）と、見つけた前からの不具合 3 つ（retired_creds、cred の競争、memory 圧迫の ENOMEM）の直し | completed（2026-10-28、5330 の乱読み QD32 が Debian の 99.3%） | p005 は Future Work、fork の飢えは Future Work |
| [WS312](ws312/ws.md) | MG005 | IPv6 の fragment の受け取りと再組立て（2026-10-29 ユーザー） | completed（2026-10-29） | QEMU で大きい UDP・ping6 の fragment を受け取る |
| [WS235](ws235/ws.md) | MG006 | Realtek RTL8125 系の共通の Ethernet driver（PCIe の RTL8127 10GbE） | completed（2026-10-29） | 10.0.10.3 の RTL8127 で 10000Mbps・ssh・tcp-perf 両向き、driver は律速でない |
| [WS311](ws311/ws.md) | MG005 | TCP の CUBIC と既定の切り替え（2026-10-29 ユーザー） | completed（2026-10-29） | 既定を CUBIC に（遅延の経路で 1.158 倍、LAN は同じ） |

| [WS314](ws314/ws.md) | MG006 | libkeiland の API を component ごとの header に分け、プログラミングの解説（英語）を書き、fd と UNIX への依存を API から外す（2026-10-30 ユーザー） | completed（2026-10-30 ユーザー「completeでOKです。」） | header の分割・解説（docs/howto/keiland-programming.md）・kl_app_post・kl_process・kl_launch・kl_instance_bind・KL_APP_LANGUAGE・libbrowser の network の thread、fd の API を公開の API から外した。昇格の起動の API は F-WS314-elevate |
| [WS315](ws315/ws.md) | MG003 | Realtek の USB 2.5G・5G の NIC を独自の方式（vendor mode）の driver で速く（2026-10-30 ユーザー） | completed（2026-10-30 Q1、main 834d10d88） | 5330 RTL8156B で Linux 並み（send 2370・recv 2373・同時 2268/2359）、10GbE も落ちない。5G（RTL8157）は資料だけの実装。制限は ws.md |