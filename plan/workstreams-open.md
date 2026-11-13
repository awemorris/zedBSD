<!-- awesome-plan project=zedbsd record=workstreams-open -->

# 未完の WS

状態の本体は各 ws.md、ここは写し（[record-audit/status.py](tools/record-audit/status.py) が照合する）。完了・取り下げは [workstreams-closed.md](workstreams-closed.md) へ移す。
今の Focus と優先順位は [master.md](master.md) の「今の状態」。
（2026-10-27 に P3（q1608）が精査した: [ws-audit.md](agents/P3/ws-audit.md)）

### 進めている・次の WS

| WS | Primary | 内容 | 状態 | 再開点 |
| --- | --- | --- | --- | --- |
| [WS299](ws299/ws.md) | MG004 | 5330 の NVMe の読み書きを速くする（512 MiB の崖、転送・queue・readahead）（2026-10-27 ユーザー） | incomplete（2026-11-13 q2044: 今の main で測り直し。Debian 比 95 % 未満は direct の 4 MiB の読み 92 % だけ（p6 の raw の書きは同じ形で 104〜117 %）。乱読みの後の連続の読みを buf.c で直した（97 %）。p006 cleared、p004 は QEMU と他の機種の build が残る、QEMU の image の作り方を p007 で直す） | p001〜p003・p009 cleared（raw の読み 583 MB/s ≒ Debian の 50%）。受け入れは Debian の 95% 以上、HMB はやる（ユーザー）。q1634（P1）で p004 raw の先読み → p005〜p008 2026-10-30 ユーザー「WS299はRealtek 2.5Gのあとにスケジュールします。」→ WS315 の後 |
| [WS300](ws300/ws.md) | MG003 | NVMe と VMD の低電力（S0 idle の VMD の suspend・NVMe の状態、APST・ASPM・LTR）（2026-10-27 ユーザー） | incomplete（p001 cleared、p002〜p005 実装済み・試験未実施、branch agent/p6-ws300 は 5330 の試験の後に merge） | p006（5330、WS052 p014 と一緒）。Q300-6（_OSC 無しの ASPM）はユーザーの判断待ち |
| [WS334](ws334/ws.md) | MG006 | libbrowser の Web ブラウザ OmniGrammaton を Windows に移植（libkeiland の Win32 の実装、.exe）（2026-10-31 ユーザー） | incomplete | 設計 p002〜p004 cleared、実装の第 1 群（p005・p009・p010・p013・p020） |
| [WS335](ws335/ws.md) | MG002 | libssl-compat・libcrypto-compat: OpenSSL の libssl・libcrypto と互換の自前の TLS と暗号の library（libbrowser と自前の ssh が使う部分、標準廉価実装手順）（2026-11-01 ユーザー） | incomplete | p001 設計（P6 q1789） |
| [WS336](ws336/ws.md) | MG005 | 自前の sshd・ssh（OpenSSH と互換の protocol、ed25519 と鍵の交換 1 つ、WS335 の暗号の口を使う、標準廉価実装手順）（2026-11-01 ユーザー） | incomplete | incomplete（p001 設計 cleared、p002 から実装） |
| [WS337](ws337/ws.md) | MG002 | shangrila: RISC-V 64 で x86_64・arm64 の binary を動かす単体の repository の OSS（事前の変換と cache、interpreter の予備、binfmt。goal は Linux の商用 software と Windows の game、先に Linux で開発、zedBSD は package）（2026-11-01 ユーザー） | planning | ユーザーとの質疑の途中、p001（設計）はまだ |
| [WS302](ws302/ws.md) | MG006 | デスクトップスレッドバランサ DTB（CONFIG_DTB、sched への hint）（2026-10-27 ユーザー） | planning | p001 設計済み（main 896d3e3db、問い Q302-1〜7 待ち）。実装は約 22 LW、WS333 p011・p013 に依る |
| [WS304](ws304/ws.md) | MG003 | Raspberry Pi 5 の外の PCIe（pcie1、M.2）と NVMe（2026-10-27 ユーザー、実機の試験はまだ） | incomplete | p001・p002 cleared（e616b4128、host 試験・build のみ）、p003 の実機は新しい SwitchBot の後、HAT の型番をユーザーに確認 |
| [WS305](ws305/ws.md) | MG006 | デスクトップの GPU の passthrough の試験環境（Linux＋VFIO＋QEMU、素の起動と使い分け）（2026-10-27 ユーザー） | incomplete（再開待ち） | P4 はラップアップ（2026-10-27）。script と guest の image は用意済み、host（10.0.10.3）では未着手。再開は ws.md の「状態と再開点」 |
| [WS306](ws306/ws.md) | MG006 | GPU の driver の枠組みの「ゲーム最適化」の UAPI（ON/OFF と option の文字列、GPU の名前と driver の版の取得）（2026-10-27 ユーザー） | incomplete | p001 cleared（2026-11-01）、実装は担当の割当の後（Q306-7・9 は推しで決定） |
| [WS052](ws052/ws.md) | MG003 | 電源管理（S0ix を 5330 で初めて動かす、modern standby、/dev/system） | incomplete | 再開点 p014（WS300 p002 の後）、並べて p015・p016、約 31 LW |
| [WS068](ws068/ws.md) | MG006 | EGL と OpenGL ES（と desktop GL 3.0〜4.6）を Vulkan と display 拡張の上に実装する（Wayland とディスプレイ直接の両方）（2026-09-26・27 ユーザー指示） | incomplete・継続（空き時間だけ。受け入れは Venus のみ、2026-10-29 ユーザー） | 次は p045（sample shading、Venus の試験だけ）→ p047〜p068（GL 4.x を分けた物）→ p007 規約 |
| [WS074](ws074/ws.md) | MG006 | zedBSD の Web ブラウザ `userland/base/zdesktop-browser`（HTML5 の layout engine → 最適化にこだわらない JavaScript engine の接続 → CSS の準拠と Chrome との比較で目標値を段階的に上げる。JS と Wasm の実行 engine を共通化。画像は libpng-compat・新しい libjpeg-compat、TLS は当面 OpenSSL）（2026-09-27 ユーザー指示） | incomplete・継続（Claude のクラウドのセッション、2026-11-01〜、成果は origin/browser を Q1 が merge） | phase260 の自走（WPT の固定の corpus で 3,125 / 3,472 前後、100% まで） |
| [WS217](ws217/ws.md) | MG006 | 画面の回転（Settings の Display、縦長で Notes を使う）（2026-10-13 ユーザーの UAT） | incomplete（2026-10-28 ユーザー「WS217 を開き直す」） | p009-1（Intel の PRM）・p009〜p011（scanout の 180°）・p005（複数 head）・p006（pen・touch と display の結び付け）・p012（game の session）→ p007 確かめ・p008 規約。p002a・p002b・p003・p004 は済み |
| [WS221](ws221/ws.md) | MG005 | Jumbo frame（Ethernet の L2 の大きな MTU、device ごとの上限、IP・TCP の MSS）（2026-10-13 ユーザー） | incomplete（すぐ取り組む物、2026-10-27 ユーザー「Jumbo Frameはすぐやるものの一つにします。」） | p001 の判断から（p008・p009 は WS220 の旧 p006 tcp_device_mss・p007 を移した物、2026-10-28 ユーザー「WS221 へ移す (推し)」） 2026-10-30 ユーザー「WS221はRealtek 2.5Gのあとにスケジュールします。」→ WS315 の後 |
| [WS223](ws223/ws.md) | MG006 | 全画面は compositor の合成、game mode は Wayland の拡張で求められた時だけ（Video Player は game mode をやめる）（2026-10-13 ユーザー） | planning（すぐ進める、2026-10-27 ユーザーの回答「推しで決めて実装」、D1〜D6 は推し） | p001 の D1〜D6 を推しで確定 → p002 |
| [WS226](ws226/ws.md) | MG006 | Khronos の Vulkan 1.3 の header をそのまま使い、libvulkan で Vulkan 1.3 の API を実装しきる（不整合の整理）（2026-10-13 ユーザー） | planning（WS290 の後、全 GPU driver で Vulkan 1.3 相当を capability の flag とともに、2026-10-27 ユーザー） | WS290 の後に p001 |
| [WS242](ws242/ws.md) | MG006 | nvrtx の RTX 20/30/40 の全モデル（2026-10-15 ユーザー） | incomplete（すぐ取り組む物、2026-10-27 ユーザー「すぐ取り組むものの一つです。」） | p001 の再開点 1〜4、GTX 16・RTX 30・40 の既定の有効（ユーザーの決定、未実装） |
| [WS250](ws250/ws.md) | MG006 | nvrtx の終わらない GPU の仕事の取り消しと復帰、system は動き続ける（2026-10-15 ユーザー） | incomplete（すぐ取り組む物、2026-10-27 ユーザー「すぐ取り組むものの一つです。」） | p001 の設計（草稿は main 64f05b2c7） |
| [WS290](ws290/ws.md) | MG006 | zedBSD の GHCI の driver を GHCI 1.0（amaterrace-gpu の docs/ghci.md 89529de）だけに書き直す。試験は host だけ | incomplete・保留（2026-10-28 ユーザー: FPGA 側の設計が終わるまで P3 終了、再開は明示の指示。p017 は実機待ち） | q1636（P3）で p001 の design 版 4 → p002〜（host 試験、実機は VF2＋KC705 の用意の後） |
| [WS291](ws291/ws.md) | MG006 | GPU の driver の描画の速さ（desktop 60 fps・重い 3D の model 50 fps）: AMD RDNA（P2）→ NVIDIA（P2、GPU を入れ替え）→ PowerVR（VF2、P3） | incomplete・ラップアップ（2026-11-01 ユーザーの指示。p027 B-U は回帰の後に main に merge 済み。再開はユーザーの指示で） | q1635（P2）で p006 の続き → p008〜p009、Linux 以上の後に WS306 |
| [WS309](ws309/ws.md) | MG006 | Keiland の compositor の thread の構成を整理し main loop が待たないようにする。ゲームモードは Vulkan display の仕様の中で | incomplete（p001〜p007・p019 cleared、P1 q1662-i05） | p008 の残り（fence 付きの task）から |
| [WS310](ws310/ws.md) | MG004 | ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる（WS301 の Q301-3、2026-10-29 ユーザー「最終的には呼ぶが、別なWSにして…今は呼ばない。」） | incomplete（p001 設計済み、問い Q310-1〜6 待ち） | p001（WS301 の後） |
| [WS313](ws313/ws.md) | MG008 | ODROID-C5（Amlogic S905X5M）の BSP（bsp-s905x5m）と Mali の GPU で、起動して desktop を表示する（2026-10-29 ユーザー） | incomplete（全 block の code を揃えた、実装済み・実機未試験。2026-10-29 夜にユーザーが実機で試験） | 夜の試験（plan/ws313/tests/README.md）→ 直し、Mali の desktop（render pass の LOAD など） |
| [WS316](ws316/ws.md) | MG003 | 5330 の USB4 と AQC の 10GbE（2026-10-30 ユーザー） | planning | p001 の調べ（5330 の Debian で CM・AQC の見え方・IOMMU） |
| [WS317](ws317/ws.md) | MG005 | hotplugd: hot-plug の device（PCIe・USB・ディスク）の承認と driver の開始・終了、volumed を置き換え（2026-10-30 ユーザー） | planning（今は取り組まない、ユーザー） | p001 の設計から |
| [WS318](ws318/ws.md) | MG006 | libmedia の作り直し: Vulkan の device の共有・codec の問い合わせと指定・encoder（H.264・H.265・AAC-LC）・カメラ・libavcodec の内部の fallback、app を libmedia だけに（2026-10-30 ユーザー） | planning | p001 設計から。2026-10-30 ユーザー「WS318はH.264デコーダのあとにすぐスケジュールします。」→ WS326 の completed の直後 |
| [WS319](ws319/ws.md) | MG006 | i915（5330、Alder Lake） の Vulkan Video: H.264 encode（VDEnc、HuC の firmware）、H.265 decode・encode | incomplete | 2026-11-02 12:03: H.265 decode（HEVC_v1 Main の全部が正しい）と Video Player の H.264・H.265 の GPU の道（1080p30 late 0）を merge。残り: 1080p60 の late、encode |
| [WS320](ws320/ws.md) | MG006 | Pi 4（BCM2711） の Vulkan Video: H.264 encode（VideoCore の MMAL video_encode を Vulkan Video の encode として、WS236 と同じ道）、H.265 decode（HEVC の block） | planning | WS318 の completed の後 |
| [WS321](ws321/ws.md) | MG006 | Pi 5（BCM2712） の Vulkan Video: H.265 decode（HEVC の block を Vulkan Video として）。H.264 は hardware に無く libavcodec | incomplete | 2026-11-02 ユーザー: P3 が着手、Video Player の GPU の道の再生まで |
| [WS322](ws322/ws.md) | MG006 | VisionFive 2（JH7110） の Vulkan Video: H.264・H.265 decode（Wave511）、H.265 encode（Wave420L）を PowerVR の Vulkan の executor の Vulkan Video として。H.264 encode は hardware に無く独自の software（WS329） | incomplete | p001・p002 cleared（設計、VF2 の実機で Wave511 の decode が ffmpeg と一致）、p003（P2 q1863）。2026-11-02 ユーザー: 今日は decode（p006）までで終了、encode は次に再開（incomplete） |
| [WS323](ws323/ws.md) | MG006 | ODROID-C5（Amlogic S905X5M） の Vulkan Video: H.264・H.265 の decode と encode（Amlogic の VPU、encode は hardware の有無の確認から）を Mali の Vulkan の executor の Vulkan Video として | planning | WS318 の completed の後 |
| [WS324](ws324/ws.md) | MG006 | RTX 2070（nvrtx） の Vulkan Video: H.264 encode（NVENC）、H.265 decode（NVDEC）・encode（NVENC） | incomplete | p001 cleared（設計、約 213 LW）、p006a H.265 の解析の設計（P1 q1864）。NVENC の関門は上限なし、libmedia の確かめは WS318 へ（ユーザー）。2026-11-02 ユーザー: 今日は NVDEC（p007）までで終了、encode は次に再開（incomplete） |
| [WS325](ws325/ws.md) | MG006 | RDNA（RX 5600 XT、VCN 2.0） の Vulkan Video: H.264・H.265 の decode と encode（VCN） | planning | WS318 の completed の後 |
| [WS328](ws328/ws.md) | MG006 | libbrowser のベクトル描画（path・CSS の角丸・gradient・影）と SVG の描画（2026-10-30 ユーザー）。WebP・AVIF は WS326 の結果の後 | planning（WS074 と調整してから） | p001 設計から |
| [WS341](ws341/ws.md) | MG006 | 仮想 GPU の driver の枠組み（user の program が作る scanout だけの /dev/gpuN、他の GPU が描き、Vulkan Video で H.264・H.265 に encode して USB・network の遠隔の display・RDP へ）（2026-11-01 ユーザー） | planning | p001 調べと設計（担当は未割当） |
| [WS342](ws342/ws.md) | MG006 | Windows 版の libvulkan（userland/desktop/libvulkan-win32、vulkan-1.dll への橋渡し、handle の共有）と Keiland の Windows の backend の compositor（userland/desktop/libkeiland-backend-win32、DWM の上）（2026-11-02 ユーザー） | planning | 着手しない（議論の後） |
| [WS343](ws343/ws.md) | MG005 | base の TLS と ssh を自前に（CI の config から OpenSSH・OpenSSL・curl・libcurl を外し、base に自前の ssh・sshd・libssl.so・libcrypto.so、fetch の https、ca-certificates は packages に残す）（2026-11-02 ユーザー） | incomplete | p002（passkey の OpenSSL の関数を libcrypto-compat に、P2 q1990、20:46） |
| [WS345](ws345/ws.md) | MG006 | ブラウザの thread の分け方: JavaScript を窓の event と別の thread に、通信を独立した thread に（2026-11-02 ユーザー） | incomplete | P8 が p001 の設計（q1959） |
| [WS346](ws346/ws.md) | MG006 | libbrowser の style と layout の差分の再計算（layout cache）。DOM の変更の記録・style の無効化・layout の部分木の再利用、全体の作り直しとの照合（2026-11-03 ユーザー、設計は Q1、実装は Sonnet） | completed | 全 Phase (p001〜p009) cleared。Q1 が直に実装、既定 cache=on、bench 14〜17 倍、実サイト verify 差異 0、Windows cross-build PASS（[design.md](ws346/design.md)） |
| [WS347](ws347/ws.md) | MG006 | VF2＋KC705 で Amaterrace GPU を試験・デバッグする。KC705 に minimal・200 MHz の bitstream を Vivado で書き、GHCI の driver を最新の ghci.md に追従（2026-11-11 ユーザー）。RTL は Gemini、Claude は監査（[amaterrace.md](amaterrace.md)） | incomplete | Gemini の修正（ad23fe01）の監査 → 依頼 03、bitstream の書き込み（p001）、driver の追従（p003、P2 の worktree に未 commit） |
| [WS348](ws348/ws.md) | MG003 | Raspberry Pi 500 の dtb を image に入れる（そのまま Pi 500 で起動）（2026-11-13、Pi 500 の外部の報告から） | incomplete（p001 cleared: zedBSD-firmware 2670dc1 と mk f949ade4f2、FAT に 3 つの dtb。Pi 500 の実機は未確かめ） | p001 |
| [WS349](ws349/ws.md) | MG006 | Raspberry Pi 5 の HDMI の音（2026-11-13、Pi 500 の外部の報告から） | incomplete（p001 設計の第一版、design-reviewer 未実施） | p001 |
| [WS350](ws350/ws.md) | MG003 | Raspberry Pi 500 を動作の確かめた機種として記録する（2026-11-13、Pi 500 の外部の報告から） | incomplete（p001 cleared: README と tools.md に Pi 500） | p001 |
| [WS340](ws340/ws.md) | MG006 | Pi 4・Pi 5・VF2 の graphical boot（logo を出し log を出さない、PC/AT と同じ起動を menuconfig の ZEDBSD_GRAPHICAL_BOOT で選べる）（2026-11-01 ユーザー） | planning | p001 調べと設計（担当は未割当） |
| [WS339](ws339/ws.md) | MG004 | x86 の PC の UEFI の起動の画面の解像度を monitor（EDID）に合わせる、標準の設定の video=640x480 を外す（2026-11-01 ユーザー） | planning | p001（担当の割当はユーザーの指示で） |
| [WS338](ws338/ws.md) | MG006 | libmedia の H.264 の software の decoder の速さの続き（WS326 から分けた、2026-11-01 ユーザー） | planning | p001 設計（担当の割当はユーザーの指示で） |
| [WS329](ws329/ws.md) | MG006 | libmedia の H.264 の software の encoder（標準廉価実装手順、decoder と共通化）（2026-10-30 ユーザー） | planning | WS326 の completed の後に p001 設計（Opus） |
| [WS330](ws330/ws.md) | MG006 | libmedia の H.265 の software の decoder と encoder（標準廉価実装手順、H.264 と共通化）（2026-10-30 ユーザー） | planning | WS329 の completed の後に p001 設計（Opus） |
| [WS292](ws292/ws.md) | MG006 | GHCI の session の保護（PROT）を zedBSD の driver に（WS290 の後） | planning | p001 設計（WS290 と G29 の回答の後） |
| [WS295](ws295/ws.md) | MG006 | Intel の xe の driver（Meteor Lake 以降の iGPU と discrete の Arc、GuC の submission、i915 の使える所は写して重複） | planning | p001 調査（担当の起動はユーザーの指示の時） |
| [WS296](ws296/ws.md) | MG001 | src/drivers（gpu/ を除く）の comment から開発の管理情報を消し、XXX の規約の当て直し（usb など 11 file） | planned | 18 LW、担当の起動はユーザーの指示の時 |

### 保留の WS（何をやるかは Q1 とユーザーが整理中）

「元:」は以前の段（ベータ3・4 などは取り消し）。

| WS | Primary | 内容 | 状態 | 再開点 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | MG002 | POSIX.1-2024 準拠 | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p041〜p044 の guest 試験、p045 POSIX の header、p046〜p051、p040 mesg（BUG-067） |
| [WS013](ws013/ws.md) | MG007 | CPAR（container 分割） | incomplete・保留（Future Work F-002） | F-002（runtime の隔離・CLI・service container） |
| [WS026](ws026/ws.md) | MG001 | 試験資産の整理 | planning・保留（元: ベータ3、段は取り消し・整理中） | Phase 未定義 |
| [WS080](ws080/ws.md) | MG002 | `ld.coff`: Win64 PE32+ の動的ローダ（PE/COFF の mapping・relocation・DLL・import/export・Microsoft x64 ABI・最小の TEB/PEB・GS base）。NT の loader は再現せず `AddressOfEntryPoint` へ直接。互換の DLL は上に積む（2026-09-28 ユーザーの仕様 [spec.md](ws080/spec.md)） | incomplete・保留 | p002 は agent/p1 の a06d5d75d・6c7c991b1（未 merge）、p003〜p012 |
| [WS082](ws082/ws.md) | MG002 | Linux の `/dev/kvm` の移植の検討（eventfd 等の非 POSIX の fd の代わりに unix socket の message で MMIO・IRQ の通知。ioctl を直接の移植・別の仕組みでの代替・実装不能に分類）（2026-09-28 ユーザー） | planning・保留（要検討） | ユーザーの判断（study.md §10） |
| [WS098](ws098/ws.md) | MG006 | IME の変換のニューラル化: 辞書で候補を作り、小型のモデル（15 MB 未満）で同音異義語の選択（語の番号の並び）とひらがな列の形態素解析（語の境界と品詞、BiLSTM か小型の Attention）を評価する（2026-09-29 夜 ユーザー、IME の最後の仕上げ） | planning・保留（要検討・ブロック） | p001 設計（WS095 の後、corpus と license はユーザー） |
| [WS115](ws115/ws.md) | MG002 | upstream GTK4をzedBSD `packages/desktop/gtk4`へ移植し知見を記録 | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p010（GTK の起動、ld.so の上限は WS140 で解決済み）、p003 |
| [WS116](ws116/ws.md) | MG002 | upstream Qt6の範囲をGTK4移植後に検討し `packages/desktop/qt6`へ移植 | planning・保留（元: ベータ3、段は取り消し・整理中） | p001〜p005（WS117 の後） |
| [WS117](ws117/ws.md) | MG006 | Linux の本物の Qt6 を調査し、素の Qt6 アプリが動くよう compositor を改良（WS115/116 の前） | planned・保留（元: ベータ3 以降、段は取り消し・整理中） | p001〜p006 |
| [WS126](ws126/ws.md) | MG002 | Python 3 の package（fg019） | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p005 の guest の受け入れ（test-wait）、p006 規約 |
| [WS144](ws144/ws.md) | MG005 | VPN（bridge・tunnel などの汎用の network の基盤と、選んだ VPN の protocol、Settings の stub の VPN の頁の実体）。ベータ2 の実装の項目、時期は未定（2026-10-04 ユーザー） | planning・保留（元: ベータ4 以降、段は取り消し・整理中） | p001 から |
| [WS146](ws146/ws.md) | MG006 | SSH を使う独自のオンラインストレージ（server に agent を転送、file の木と metadata の database、差分の同期、手元に本体の無い on-the-fly のアクセス、OneDrive のような機能）。「あとで」の要望（2026-10-04 ユーザー） | planning・保留（元: ベータ4 以降、段は取り消し・整理中） | p001 から（WS150 の後） |
| [WS147](ws147/ws.md) | MG006 | Microsoft OneDrive の client（Settings の Sharing の頁から設定、OAuth・Graph の API・同期、on-the-fly は WS146 と共有）（2026-10-04 ユーザー「独立WSにします」） | planning・保留（元: ベータ4 以降、段は取り消し・整理中） | p001 から（WS150 の後） |
| [WS150](ws150/ws.md) | MG004 | userland の file system の kernel の枠組み（FUSE に当たる物）。クラウドストレージ（WS146・WS147）の前提（2026-10-04 ユーザー） | planning・保留（要検討・ブロック） | p001 から |
| [WS152](ws152/ws.md) | MG007 | system の更新（Settings の Updates の頁の実体）。ベータ3 の実装の項目、p001 は方式の検討（2026-10-04 ユーザー） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 の U1〜U12 の判断 |
| [WS153](ws153/ws.md) | MG007 | Settings の Apps の頁と third-party の app の repository（userland/packages とは別）。p001 は package の仕組みの検討（2026-10-04 ユーザー） | incomplete・保留（ユーザーが検討中） | U2〜U15 の決定の後に p002 以降 |
| [WS158](ws158/ws.md) | MG006 | Keiland 本体と Keiland の app の翻訳（英語が基準、日本語はベータ2、Settings の Languages の頁で選ぶ。F-068 を昇格）（2026-10-05 ユーザー） | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p002〜p005 の T1・ユーザーの日本語の review、p006 |
| [WS171](ws171/ws.md) | MG008 | hal.h の全ての関数に HAL v2 と同じ水準の契約の comment を書く（comment の差分もユーザーの review の後に当てる）（2026-10-05 ユーザー「このコメント、すべての関数につけてほしいです。今でなくていいので」） | incomplete・保留（優先度低め、ユーザーの review 待ち） | p002 のユーザーの review、p003 の適用 |
| [WS172](ws172/ws.md) | MG006 | passkey の認証の枠組み: sessiond は外部の `/sbin/passkey`（base）で password・PIN・FIDO2（将来はセキュリティチップ）を確かめる、root だけの `/etc/passkey`、PIN の失敗の回数は sessiond の memory（2026-10-05 ユーザー） | incomplete・保留（元: 実装はベータ3、段は取り消し・整理中） | p004b の TPM の設計、p005 OpenSSL の置き換え、p007・p011 の試験、p006 規約 |
| [WS176](ws176/ws.md) | MG006 | Canvas: pen と touch のイラスト制作・画像編集（layer・brush・素材集・CLIP STUDIO と PSD の互換）（2026-10-06 ユーザー、ベータ3） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 から |
| [WS180](ws180/ws.md) | MG006 | Emacs の拡張 — Emacs をベースにしたグラフィカルなエディタ（エージェント開発のための次世代のエディタ） | planning・保留（元: ベータ3 以降、段は取り消し・整理中） | Phase 未定義 |
| [WS184](ws184/ws.md) | MG006 | 左手デバイスの OSK（クリエイターモード: ダイヤル・ホイール・ボタン 2×5、左上の swipe で出す、ベータ2） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 設計から |
| [WS185](ws185/ws.md) | MG006 | ゲームパッドの OSK とゲームコンソールモード（両上隅の同時 swipe、Xbox の pad を模す、段 1 は mview、ベータ2） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 設計から |
| [WS186](ws186/ws.md) | MG003 | Realtek RTL8822CE（5320 の PCIe の WiFi、ベータ3、ベータ2 が早く終われば前倒し） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 調査と設計から |
| [WS197](ws197/ws.md) | MG006 | Bluetooth のスマホ連携: MAP（SMS・MMS）・Integration（WS170）・PBAP・HFP（通話、SCO）。ベータ2 の必須（2026-10-10 ユーザー） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p006・p007 HFP、p010 の MMS の送信、p014、p008 実機、p009 規約 |
| [WS198](ws198/ws.md) | MG002 | zedBSD の上で zedBSD を self-build（host の clang、2026-10-09 ユーザー、優先度低、ベータ2 の見込み） | incomplete・保留（元: ベータ3、段は取り消し・整理中） | p001 の判定から |
| [WS201](ws201/ws.md) | MG002 | /home の暗号化（UFS の先頭の key slot、FIDO2 の hmac-secret／PRF と回復のパスワード、master key は初回に /dev/random）（2026-10-10 ユーザー、ベータ3、今は検討しない） | planning・保留（元: ベータ3、段は取り消し・整理中） | p001 設計 |
| [WS204](ws204/ws.md) | MG006 | HD Audio の録音（内蔵マイク）と HFP 通話の音（2026-10-11 ユーザー） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p001 の U1・U2、p002〜p005 |
| [WS205](ws205/ws.md) | MG006 | Bluetooth audio（A2DP の SBC・AAC、headset で HFP 通話、libmedia の encoder）（2026-10-11 ユーザー） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p001 の D1〜D6、p004〜p011 |
| [WS209](ws209/ws.md) | MG006 | audiod の音の endpoint と route（通話・Bluetooth・modem）（2026-10-11 ユーザー Q12） | incomplete・保留（元: HFP はベータ3、段は取り消し・整理中） | p001 の再 review、p004〜p006 |
| [WS212](ws212/ws.md) | MG006 | Wiseview の作り直し: 仮想デスクトップ 3 枚の矩形と窓の一覧、drag で仮想デスクトップ・display をまたいで移動、multi display の縦の積み重ね、display のローテーション（タグ・Shift+Super+←/→、比率の保持）（2026-10-12 ユーザー） | planning・保留（着手はユーザーが決める） | p001〜p005 |
| [WS222](ws222/ws.md) | MG006 | 画像・PDF・字形の library を menuconfig の Desktop へ戻す（PNG・PDF・JPEG・TrueType）（2026-10-13 ユーザー、今の作業が全部終わってから） | planned・保留（後日） | p001 |
| [WS225](ws225/ws.md) | MG006 | Vulkan の header を仕様から導ける内容だけで書き直し、Khronos の著作権表示を外す（2026-10-13 ユーザー） | planning・保留（ブロック） | p001 |
| [WS234](ws234/ws.md) | MG006 | libkeiland の DLL 版（zedBSD 用と Windows 用）で、同じ .exe が zedBSD と Windows の両方で動く | planning・保留 | p001 |
| [WS240](ws240/ws.md) | MG006 | nvrtx の RTX 50（Blackwell）（2026-10-15 ユーザー） | planning・保留（開始しない） | — |
| [WS251](ws251/ws.md) | MG001 | source の tree 全体から開発の管理情報を削除する（2026-10-15 ユーザー） | incomplete・保留（作業対象になった時） | p001 の棚卸し、drivers の外の群 |
| [WS257](ws257/ws.md) | MG006 | i915 を Haswell・Broadwell（Gen7.5・Gen8）に対応（2026-10-18 ユーザー） | planning・保留（今は取り組まない） | — |
| [WS273](ws273/ws.md) | MG008 | Anbernic RG350（JZ4770、MIPS32）の実機への移植（2026-10-19 ユーザー） | incomplete・保留 | 再開は ws273/phase003/phase.md、実機 U26 |
| [WS275](ws275/ws.md) | MG008 | sun4u に /dev/graphics を実装し Xzed を動かす（2026-10-19 ユーザー） | incomplete・保留 | p006（BUG-337）、p007 |
| [WS281](ws281/ws.md) | MG002 | libc の malloc を速くする（first fit・1 本の list・1 つの lock の作り直し）（2026-10-22 ユーザー） | planning・保留（後日） | — |
| [WS282](ws282/ws.md) | MG006 | camera の driver の枠組み（/dev/camN）と USB Video Class の driver（CSI の rpicam は今回やらない） | incomplete・保留（設計だけ、実装は後日） | p002〜p011（設計 版 3 は済み） |
| [WS283](ws283/ws.md) | MG006 | bcm2711 の Vulkan の compute（Pi 4・Pi 5） | incomplete・保留（設計だけ、実装は後日） | p002〜p014 |
| [WS284](ws284/ws.md) | MG006 | YOLOX-Nano の推論（C と Vulkan compute、Apache-2.0） | incomplete・保留（設計だけ、実装は後日） | p002〜p008 |
| [WS285](ws285/ws.md) | MG006 | camera の object detection の demo の app（WS282〜WS284 の上） | planning・保留（後日） | p001 設計 |
| [WS287](ws287/ws.md) | MG008 | EHCI の full-speed・low-speed の device（split transaction、ZYBO の ChipIdea を含む） | planning・保留（後日） | — |
| [WS297](ws297/ws.md) | MG001 | completed の WS に残った「規約の全文の見直し」を一括で（WS296 と同じ形、2026-10-27 ユーザーの回答「1 つの WS にまとめる」） | planned（時期は整理で） | p001 対象の確定（候補 101 Phase・89 WS） |
