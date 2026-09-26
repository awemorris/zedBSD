<!-- awesome-plan project=zedbsd record=past-log -->

<!-- awesome-plan-current:start -->
Active Queue: なし
Last finished Queue: q482（ws035-p064 cleared。追従するドッキングの解除）
<!-- awesome-plan-current:end -->

# Past Log

## 最新: q443〜q482（2026-09-26）

[q482](queue-q482.md): ws035-p064 cleared。docked の題名を引くと窓が指に追従して縮み、140 px の手前で離すと戻り、越えると元の大きさで外れる。

[q481](queue-q481.md): ws035-p065 cleared。仮想デスクトップ 4 つ（窓はデスクトップごと、バーの絵・Ctrl+Alt+←/→・左右の端の swipe で横に移動）。

[q480](queue-q480.md): ws035-p071 cleared。App Home のページング（drag・ホイール・キー・ドット）、起動した icon から窓が育つ、左上への drag で閉じる、Tab。

[q479](queue-q479.md): ws035-p070 cleared。App Home に X terminal と Gears、`/usr/libexec/zdesktop-x11` が Xzed --rootless を必要なときに 1 つ起動。demo image にも。

[q478](queue-q478.md): ws069-p005 cleared。固定機能の GL 1.x（行列・光源・material・glBegin/glEnd・client 配列・display list、flat の provoking vertex）を libGL の中に。zgears の歯車が Venus の rootless の X の窓で回る（約 4.7 fps）。

[q477](queue-q477.md): ws069-p004 cleared。GLX の核（Xzed の QueryExtension と GLX の問い合わせ、libGL.so = GLES の変換層＋GLX＋libX11、pbuffer に描いて XzedPutImageRGB24）。Venus の zwl＋Xzed rootless で glxtest の窓が GL で描かれ、docked でも追従。

[q476](queue-q476.md): ws068-p010 cleared。EGL の pbuffer（offscreen の color と depth、frame をまたいで中身を保つ、swap で submit と解放）。Venus で 600 frame と 2048x1536 の readback が一致。pbuffer でも 1 frame 約 95 ms（F-021 に追記）。

[q475](queue-q475.md): ws068-p008 cleared。GLES 2.0 の描画の核（SPIR-V の shader binary、gl_Position の書き換え、buffer・texture・blend・depth・stencil・cull・glReadPixels、strip と fan の展開、dynamic uniform）。Venus の display 直接・zwl の窓・docked で画面と glReadPixels が一致。frame を重ねるのは p009。

[q474](queue-q474.md): ws069-p003 cleared。Xzed の rootless（X の top-level ごとに zwl の窓、top-level ごとの合成、enter で raise と focus、configure で resize、close で client を切る）。libX11 は閉じた接続で exit。Venus で X の zterm が Wiseman の窓になり、ドッキングで resize、× で終了。

[q473](queue-q473.md): ws069-p002 cleared。Xzed の Wayland backend（rootful: X の screen を zwl の 1 つの窓に、wl_shm、pointer・keyboard、libtruetype の glyph）。Venus の zwl で X の zterm に打った command の出力が見える。

[q472](queue-q472.md): ws069-p001 cleared。zwl で X11 の app を動かす設計（新 WS069）: 既存の Xzed に Wayland backend（rootful → rootless）、GLX は EGL/GLES の上で DRI3/Present に当たる buffer の受け渡し。

[q471](queue-q471.md): ws068-p002 cleared。EGL 1.5 の核（Wayland・display 直接・surfaceless、config、window surface と swapchain、context、swap）、libwayland-egl、clear だけの libGLESv2、Khronos の header と出典、egltest。Venus で zwl の窓と全画面の clear。

[q470](queue-q470.md): ws068-p001 cleared。EGL/GLES の設計（plan/ws068/design.md）: library の構成、EGL（Wayland・display 直接）、GLES の方式の比較と推奨（B: 自前の変換層＋glslang）。方式の選択はユーザーの判断待ち（p003 の前提）。

[q469](queue-q469.md): ws035-p069 cleared。App Home の PoC（desktop の層が右下へずれて明るい Home が現れる、launcher と左上の角からの drag、6 列の icon、打つと検索、起動、Esc と角で閉じる）。Venus と i915 実機で terminal と mview を起動。実機用の demo image の build script。

[q468](queue-q468.md): ws035-p068 cleared。zdesktop-terminal（zterm の VT100 を移し拡張、libtruetype の等幅 font の atlas、Vulkan の cell 描画、US 配列、key repeat、forkpty、resize）。Venus と i915 実機で shell が動く。Venus の image に git 外の font と壁紙を入れる build script。i915 の 1 回の画面停止は BUG-056（tracking）。

[q467](queue-q467.md): ws031-p050 cleared。i915 の実行器の session の close で、application が破棄しなかった Vulkan の object（command pool と buffer、descriptor pool と set、pipeline と kernel、fence、allocation、その他）を解放。descriptor pool の破棄でその set も解放。host の fixture に残したまま close する試験。実機の zdesktop の scenario に描画中に殺す Vulkan の窓と、mview を × で閉じる段を足し、mview は `reason=closed` で終わり zwl は合成を続けた。

[q466](queue-q466.md): ws035-p067 cleared。5330 の i915（VFIO）で zdesktop に mview（Vulkan の client）の窓を合成し、ドッキング（1920x1042 で描き直し）と Wiseview を capture で確認。i915 の実行器の object 表と blob の対応を session ごとの key に（wire の id が process の間で衝突していた）、allocation の import、libvulkan は allocation の共有の無い node で画像の import へ。F-022 を promoted。

[q465](queue-q465.md): ws035-p066 cleared。5330（10.0.30.3）の i915 を VFIO で渡した実機の GPU で Wiseman Mode（壁紙・すりガラス・文字・浮いたタイトルバー・ドッキング・Wiseview）を capture で確認（wl_shm の窓）。zwl の shader を native compiler に合わせ、fence の fd 無し・descriptor set の再利用・triangle list、libvulkan は画像共有だけの node で外部 memory、i915 の GGTT の窓を 64 MiB・1 GiB に。GPU の client の窓は F-022。

[q464](queue-q464.md): ws035-p063 cleared。Wiseview（ユーザーの設計 [wiseman-design.md](../ws035/wiseman-design.md)、Wiseman = WM、全画面 = ゲームモード、窓 = Wiseman Mode）: 下端からの drag で開き、窓がタイルのグリッドへ、選択・閉じる・背景で閉じる。

[q463](queue-q463.md): ws035-p062 cleared。最大化 = タイトルバーのシステムバーへのドッキング（ダブルクリック・上への drag・button）、解除（バーの題名のダブルクリック・⧉・下への pull で引きずり出す）、220 ms の遷移、仮想デスクトップのハリボテ。shell.c を glass.c から分けた。

[q462](queue-q462.md): ws035-p061 cleared。ユーザーの絵を抽象化した壁紙（`--wallpaper`、PPM、git 外）と、すりガラスで透ける窓（`--window-opacity`）。10 %・60 % の画面。

[q461](queue-q461.md): ws035-p060 cleared。mview に `--windowed`・`--size`。`zwl --glass` の窓で 3D model を Vulkan で描き、drag で回転、最大化で描き直し。spin は窓 8.80 fps・全画面 8.59 fps（Lavapipe 律速）。

[q460](queue-q460.md): ws035-p059 cleared。`zwl --glass`: 本体から離れて浮いたすりガラスのタイトルバー（題名・3 つの button、drag・最大化・閉じる・hover）、角丸と影、上部のバー（ハリボテ）、CPU で描く壁紙と縮小ぼかし（壁紙だけを透かす）、Inter（Google Fonts、git 外）を libtruetype で atlas に。p052〜p054 の回帰 PASS。

[q459](queue-q459.md): ws035-p054 uncleared。`zed_gpu_buffer_v1` version 2 の `set_acquire_fence`（1 commit に 4 つまで）、zwl は fence を poll して終わった commit だけを採る、WSI は fence を付けて先に commit。QEMU で保留の間も他の窓と合成が進み、hold 6000 ms に対し 5991〜6034 ms 待った。`vkQueuePresentKHR` は変更前と同じ 31〜35 ms（元から完了を待たない）で、受け入れ 3 はユーザーの判断で読み替えて cleared（2026-09-26）。libc の `setvbuf(…, NULL, _IOLBF, 0)` を直した（BUG-055）。

[q443](queue-q443.md): ws063-p001 cleared。journal の大きさを mkfs で記録し、mount で `.ufs-journal`（extent）を再利用・再確保・作成する。
[q444](queue-q444.md): ws061-p008 cleared。libc の同期の system call を減らし、make（直列）12.8〜13.1 秒（host `-j1` 15.2 秒）。
[q445](queue-q445.md): ws061-p009 uncleared（着手の前に中断: ユーザーの優先の変更）。
[q446](queue-q446.md): ws061-p010 cleared。system call の入口を `syscall`/`sysret` に（入口の費用 4.65 → 1.54%）、libc の lock の adaptive spin。
[q447](queue-q447.md): ws061-p009 cleared。loader の symbol の探索（hash を 1 度に、予約の名前、再配置の segment の検査）、`cc`・`ld` を link に、`spin_trylock` の TTAS。`cc t.c -o t` 75〜85 ms（host 83〜85 ms）、configure 9.1〜9.4 秒（host 10.7 秒）。
[q448](queue-q448.md): ws064-p001 cleared。base の make の `-j`（歩きの更新、job の表、GNU 互換の jobserver、`.WAIT`・`.NOTPARALLEL`）。差分試験 100/100、guest の expat `make -j4` 7.3〜7.8 秒（host 5.06 秒）。
[q449](queue-q449.md): ws064-p002 cleared。kernel の mutex の速い道、`vfork`（system call と libc）と vfork の posix_spawn、make・sh の posix_spawn、fork・destroy の VM の大域の lock の保持の短縮、逆写像の O(1)。`make -j4` 4.94〜5.00 秒（host 5.14〜5.18 秒）、configure 8.2〜8.9 秒、直列 11.3 秒。
[q450](queue-q450.md): ws064-p004 cleared。sh の pipeline・command substitution・`unset` の subshell を fork せずに（posix_spawn、shell の中の `echo`・`printf`）。fork: make 1417 → 214、configure 966 → 423。configure 7.1 秒、直列 10.7 秒、`make -j4` 4.2〜4.4 秒。
[q451](queue-q451.md): ws065-p001 cleared。sh に POSIX が未規定とする bash の構文（`$'...'`、`[[ ]]`、`function`、`(( ))`、`for (( ))`、`\|&`、`<<<`、`>& file`、`<( )`）。BUG-054 を記録。
[q452](queue-q452.md): ws065-p002 cleared。sh に bash の展開（`${v:o:l}`、`${v/p/r}` の類、`${v^^}`・`${v,,}` の類、`${!v}`）。bash の参照 21/21、dash との差は dash に無い `${x//}` の 1 件だけ増えた。
[q453](queue-q453.md): ws065-p003 cleared。sh に bash の builtin（`source`、`let`、`test ==`、`declare`・`typeset`、`printf -v`・`%q`、`builtin`、`pushd`・`popd`・`dirs`）。静的 link の効果を測り F-020 に記録（expat で 0〜3%）。
[q454](queue-q454.md): ws067-p001 cleared。`/dev/fd` を呼んだ process の descriptor に（一覧・lookup・stat）、diff が pipe を中身で比べる。BUG-054 resolved（QEMU）。
[q455](queue-q455.md): ws067-p002 cleared（規約）。WS067 completed。
[q456](queue-q456.md): ws062-p003 cleared。amd64 の既定の image を native（ESP・UFS root・swap partition、各 1 GiB の 2 GiB）に、CI は gzip で公開。CI の失敗（toolchain の smoke の消失）と clang の libc.so の依存の退行を直した。
[q457](queue-q457.md): ws035-p052 cleared。zdesktop（zwl）の 2 つのモード: Vulkan の合成（ウィンドウモード）と全画面の直接 scanout、切替。libvulkan の OPAQUE_FD の import を画像の fd に対応。
[q458](queue-q458.md): ws035-p053 cleared。`wl_shm`（libwayland の client にも追加）、cursor（矢印・client・非表示）、frame の予定。

## 前: q439〜q442（2026-09-26）

[q439](queue-q439.md): ws061-p006 cleared。UFS を write cached の既定に、`mount -o writethru` で write-through（2026-09-26 ユーザー指示）。configure（`/root`）17〜20 → 12.3 秒。journal の無い volume は電源断で漏れが残ると確かめた。
[q440](queue-q440.md): ws060-p002 cleared。batch の redo journal（v3）の設計（2026-09-26 ユーザー指示「journal を既定に、無い image は mount の時に作る、`nojournal`」の前提）。
[q441](queue-q441.md): ws060-p003 cleared。v3 の実装（pin した metadata、1 秒ごとの commit、2 slot の交互、`/.zedjournal`、superblock の locator、replay）、journal の既定化と `nojournal`。200 の作成 20.6 → 0.36 秒、強制終了の試験 4 時点と root で UFS OK、configure 13.0 秒。規約の指摘約 100 件は WS063-p002。
[q442](queue-q442.md): ws061-p007 cleared。readdir の読み（1 項目ごとに 8 KiB を 2 回 → sector だけ 1 回）と fault の待ちの全 page の走査を直し、configure（`/root`、journal）11.5 秒、host 10.9〜11.2 秒。

## 前: q438（2026-09-25）

[q438](queue-q438.md): ws061-p005 cleared。process の生成と終了の固定費用。新しい thread を作った CPU に置き idle の CPU が盗む、解放した page の LIFO の stack、HAL の `rep stos`/`movs`、`mutex_owned`・`space_op_enter` の lock の除去、destroy の unmap の省略に加え、thread の移動で表に出た race（current task の 2 段の読み、shootdown の送り手、preempt count・exit・switch の CPU の読み、移動の完了と wakeup）と、全 poll を起こす descriptor の通知、reaper と親の VM の mutex の奪い合い、readahead の全 worker の起床、生まれたばかりの子の盗みを直した。page の stack が逼迫時に DMA の確保を失敗させる退行も直した。`true` 3.4 → 1.1 ms、configure（`/root`）35 → 17〜20 秒（host 11.2 秒）、tmpfs 13 秒、make（直列）20〜25 秒（host `-j1` 15.2 秒）。並列の make（F-016）と UFS の delayed write（F-015）はユーザーの判断。amd64 の `defs.h` の未使用の `CLOCK_HZ 100` を削除（tick は 1000 Hz）。QEMU だけ。

## 前: q437（2026-09-25）

[q437](queue-q437.md): ws062-p002 cleared。NVMe の起動の harness、4 GiB の root・swap の image、BUG-053（RAM を超える anonymous memory）の修正（450・900 MiB）、`kern_free` の O(1)。configure（`/root`）の tmpfs との差は UFS の write-through と flush（F-015）。

## 前: q436（2026-09-25）

[q436](queue-q436.md): ws062-p001 cleared。amd64 の native の layout（GPT: ESP に `BOOTX64.EFI`・`vmunix`・`zedbsd.cfg`、UFS の root partition、swap partition、`PARTLABEL=` で指す）を `zedimage-host` と `ZEDBSD_VARIANT=native` で作れるようにし、検査器を足した。QEMU で起動し、root は `/dev/sda2` の UFS（rw）、swap は partition、SSH の harness が通る。kernel と loader は変えていない。

## 前: q435（2026-09-25）

[q435](queue-q435.md): ws061-p003 uncleared。fault-around（object が既に持つ隣の page を 16 page の窓で map）と、amd64 の TLB の invalidation の縮小（map の後の shootdown を削除、小さい範囲は `invlpg`）。file fault 3.2 → 1.4 µs、`clang --version` の fault 5862 → 1891（目標 1/8 は未達）、`cc t.c -o t` 0.21〜0.24 秒、configure 31 秒（tmpfs）・55 秒（overlay）。configure の時間は子の system 23 秒が主（fault は 1〜2 秒）。BUG-051（sshd の子の SIGSEGV 1 回、未再現）、BUG-052（tmpfs 32 MiB）を記録。ユーザー指示で disk の layout を ESP の vmunix・UFS の root partition・swap partition にする WS062 を立てた。回帰 make 91/91・sh 1388/1425・COW・SMP。

## 前: q434（2026-09-25）

[q434](queue-q434.md): ws046-p014 uncleared（kernel の変更は完了）。private の file の mapping（ld.so が map する libLLVM・libclang-cpp の text など）で page cache の page を read-only で map し、書き込みで COW（p011 の差分の当て直し。p011 の失敗の原因の object の寿命は p012 で直っていた）。`mprotect` で書き込み可能にしたとき commit を取らない穴を直した。file fault 8.4 → 3.2 µs、`ffault` 10 → 3 µs/page、`cc t.c -o t` 0.35 → 0.25 秒、configure（tmpfs）35 → 30〜33 秒。expat の make status 0（8 GiB・512 MiB）、runtests 4932/4932。`make check` は base に bash が無く status 2（判断待ち）。回帰 boot・make 91/91（8 GiB・512 MiB）・sh 1388/1425・COW・SMP、3 platform の build。

## 前: q433（2026-09-25）

[q433](queue-q433.md): ws061-p002 uncleared（anon fault の目標は達成、file の fault 8.4 µs と `true` 4〜5 ms は目標に届かず、残りは p003・ws046-p014）。VM object の registry の inode hash（全 object の線形探索が configure の kernel 標本の首位だった）、VM metadata の slab の O(1) の解放、kcrt の word 単位の複写、amd64 HAL の page table の entry 数と direct map の変換の短縮、itimer の tick の 1 回走査。configure 87 → 59〜64 秒（overlay）、59 → 35〜38 秒（tmpfs）、`clang --version` 160 → 80〜100 ms。ユーザーが HAL の規則を「実装は承認不要、API だけ承認」に変えた。BUG-049 は計測の image の誤りで不具合ではない、BUG-050（strerror の穴）を記録。p004（overlay の同期書き）を立てた。回帰 boot・make 91/91・sh 1388/1425・SMP・COW・itimer。

## 前: q432（2026-09-25）

[q432](queue-q432.md): ws061-p001 cleared（fg011）。host の参照: expat の configure 11 秒、`cc t.c -o t` 83〜92 ms、link 48〜57 ms（guest は 91 秒・636 ms・361 ms、7〜8 倍）。guest の時間は page fault に律速（約 30 µs/fault: `clang --version` 5797 fault = 188 ms、`ld.lld` 4547、`true` 306 = 11 ms）。loader の仕事は libLLVM/libclang-cpp の RELATIVE 23 万・relocation の表 5.6 MB。configure 中の kernel 標本: fault 22%、exit の page table 解体 13%、USB の同期 flush 20%。次: p002（fault の固定費用と exit の解体）、p003（fault-around）、ws046-p014（直接 map）。

## 前: q431（2026-09-25）

[q431](queue-q431.md): ws060-p001（journal の commit の費用の実測と設計、BUG-040）を始めた直後にユーザーが再優先付け（「expat の configure と compile を Linux と同等の水準に」を直近の目標に。commit の裏打ちは物理 + swap で決定）したので撤回、uncleared。WS057 は完了。

## 前: q430（2026-09-25）

[q430](queue-q430.md): ws046-p012 cleared（BUG-033 の残り）。VM object cache の追い出しを LRU に、最後の unmap で mapping の object を cache に残す（次の process が同じ file を map すると page が残っている）、dirty も busy も無い object の最後の unmap で全 page の同期 walk を省く。`cc t.o -o t` 0.51〜0.68 → 0.36〜0.38 秒、`libLLVM.so` の file の fault 25 → 15.3 µs/page、configure 91 秒。`KERN_SYSTEM_DROP_CACHES` を `/dev/system` に追加。BUG-045（stress の baseline の間欠）の原因は非同期の後始末との競走と特定（probe: `waitpid` の直後 file +2・vmspace +1、1 秒で戻る）、試験を直して resolved。回帰 boot・sh 1389/1425（1 件改善）・make 91/91（8 GiB・512 MiB）・COW・SMP 6/6。p011 の当て直しは p014 へ。

## 前: q429（2026-09-25）

[q429](queue-q429.md): ws059-p001 cleared、WS059 完了。BUG-047: disk の無い mount（overlay の root・tmpfs・devfs）の `st_dev` が全て 0 だったのを、`mount_device_number()`（disk は disk の番号、bind は source、それ以外は `0x80000000 | (1 + slot)`）で直し、generic・tmpfs・devfs・overlay の getattr が使う。全 mount で `st_dev` が異なり、coreutils の `df` が全 mount を大きさ付きで出す。回帰 boot PASS・sh 1388/1425（同じ集合）・make 91/91・SMP 0。

## 前: q428（2026-09-25）

[q428](queue-q428.md): ws058-p002 cleared（design policy 10）。buffer cache 物理/16 → /8（hash 2^16）、page cache の target 物理/4 → /2、VM object cache 32 → 256、snapshot 192 → 1024、I/O pool 4 → 64 MiB、file 表 192 → 2048、inode cache 512/256 → 2048/512、overlay の inode の表 256 → 4096。最初の大きな値は 2 つの退行を起こし bisect で原因を特定: overlay の固定の表は VFS の inode cache 以上でなければならず、cache の VM object は自分の file handle を開くので file の表を消費する（F-013: 動的確保と hash）。回帰: boot PASS、8 GiB と 512 MiB で make 91/91、sh 1388/1425（同じ集合）、SMP 0。expat の configure 96 → 89 秒、`cc t.c -o t` 1.0 → 0.63 秒。WS058 完了。

## 前: q427（2026-09-25）

[q427](queue-q427.md): ws058-p001 cleared（design policy 10）。8 GiB の guest で実測: buffer cache は物理/16（512 MiB）、page cache の target は物理/4（2 GiB）で比例。固定で小さいのは VM object cache 32、system 全体の open file 表 192、inode cache 512、I/O pool 4 MiB、snapshot 192。p002 で buffer 物理/8、page cache 物理/2、object 1024、file 8192、inode 8192、I/O pool 64 MiB に。

## 前: q426（2026-09-25）

[q426](queue-q426.md): ws057-p002 cleared。BUG-048（`SYSCALL_PAGE_MASK` が 32 bit で mmap などの長さが 4 GiB で切り捨て）を kernel で直し（mask を `uintptr_t` に、vmspace の丸め 5 箇所も）、`MAP_NORESERVE` を定義して受け付けて無視（reserve は無制限、commit は厳密）。probe: 128 GiB の `PROT_NONE` の reserve が commit を増やさず成功、5 GiB の RW の mapping の offset 4 GiB + 4 KiB を触れる、9 GiB は ENOMEM、`mprotect` の長さ超過は失敗。回帰 boot PASS・sh 1388/1425（同じ集合）・make 91/91・SMP 0。残りは p003（commit の裏打ちを swap だけにするかの判断）。

## 前: q425（2026-09-25）

[q425](queue-q425.md): ws057-p001 cleared（design policy 10 の調査）。commit の会計は `vm_commit_reserve/release` にあり、`PROT_NONE` の mapping は課金せず（reserve）、accessible にした時・anonymous private・書ける file private・stack・brk・shared object・fork で課金し、上限超は ENOMEM（over commit しない）。上限は起動時の空き物理 + swap（「swap だけ」にするかはユーザーの判断待ち）。probe で **BUG-048** を発見: `SYSCALL_PAGE_MASK` が 32 bit で `mmap`・`munmap`・`mprotect` の長さが 4 GiB で切り捨てられ、9 GiB の mmap が 1 GiB になる。`MAP_NORESERVE` は未定義で EOPNOTSUPP。修正は p002。

## 前: q424（2026-09-25）

[q424](queue-q424.md): ws046-p013 cleared。libc に glibc 形式の `<mntent.h>`（`setmntent`/`getmntent`/`endmntent`/`hasmntopt`、kernel の `KERN_SYSTEM_GET_MOUNTS` を mtab の形の stream に）を足し、coreutils 9.12 の cross build が gnulib の要求で次々に止まった点を libc で埋めた: `<elf.h>`（System V ABI）と `<link.h>` の `ElfW`、`<stdio_ext.h>`（`__fpending` など）、`<utime.h>`、`fseeko`/`ftello` の関数化、`posix_spawn_file_actions_add{chdir,fchdir}_np`、`<stdio.h>` の `_STDIO_H`、errno の `EPFNOSUPPORT` の独立と POSIX の残り 13 個、`statvfs.f_basetype`（kernel が type 名を入れる）、gnulib の `getlocalename_l` の zedBSD 分岐（patch）。configure・make・install が通り 102 program、guest で `df`・`stat -f`・`ls`・`sort` などが動く。回帰 boot PASS・make 91/91・sh 1388/1425（同じ集合）、`POSIX-R2-REMAINING` 01-12 PASS。新しい bug: BUG-047（disk の無い mount の `st_dev` が 0 で GNU df が root と tmpfs を出さない）。ユーザーの指示（design policy 10: メモリ 4 GB・swap 16 GB の前提、VM の reserve/commit）を記録し WS057・WS058 を planning で作った。

## 前: q423（2026-09-25）

[q423](queue-q423.md): ws056-p001 uncleared（残りは 1 点）。BUG-034（`RTSIG_MAX` 33）、BUG-035（atomic の試験を 8 byte に）、BUG-037（pax の `x`・`g`・`L`・`K`。GNU tar の pax・gnu の archive を guest で展開して host と一致）を直した。`POSIX-R2` の試験が進んで見つけた BUG-042 は真因が kernel の `thread_create(2)` の signal mask の非継承で、kernel と libc（worker・reaper を全 block、wake signal 63 を `__libc_init` で block）で直した。試験の前提の誤り BUG-043・044 も直し `POSIX-R2-REMAINING.ELF` は 01-12 PASS。`POSIX-R2.ELF` は console で timer の試験が EINTR（BUG-046、未特定）。BUG-045（stress の baseline、間欠）を記録。

## 前: q422（2026-09-25）

[q422](queue-q422.md): ws046-p009 cleared。承認された HAL の差分（`amd64_percpu_current()` を `rdmsr` から `%gs:0` の load に）を適用: syscall 1340 → 450 ns、fork+exec 9.1 → 6.5 ms、expat の configure 129 → 96 秒。回帰は amd64 の boot、sh 1388/1425（同じ集合）、make 91/91、対話 41/41、SMP stress 0。BUG-033 の残りは p012 へ。

## 前: q421（2026-09-25）

[q421](queue-q421.md): ws046-p008 uncleared。coreutils を host で zedBSD 向けに cross build: libc の header の誤り 2 つ（`<locale.h>` の include の循環、`WINT_MIN`・`WINT_MAX` が `wint_t` と合わない）を直して進んだが、libc に mount の一覧の API が無く `mountlist.c` で止まった（p013）。

## 前: q420（2026-09-25）

[q420](queue-q420.md): ws046-p011 uncleared。private の file の mapping で page cache の page を map する実装は、compile と link を速くした（1.0 → 0.61 秒）が、expat の configure を 128 → 270 秒と遅くし make を失敗させたので戻した。object の寿命の費用と見て、p012 で設計を直す。

## 前: q419（2026-09-25）

[q419](queue-q419.md): ws046-p010 cleared。`MAP_PRIVATE` の file の region（共有 library）も `MAP_SHARED`・exec の snapshot と同じく object の page を read-only で map し、書き込みで COW にする設計。実装は p011。

## 前: q418（2026-09-25）

[q418](queue-q418.md): ws046-p009 uncleared。guest の configure の 1 check 約 1.3 秒は compile と link（1.2 秒）で、kernel の file の page の fault が主。
`find_page()` が region の全 page の list をたどっていた（大きな library の fault が 2 乗）のを index に、reclaim の queue の探索を双方向の list にした: file の fault 145 → 25 µs/page、expat の configure 204〜252 → 129 秒。
残りの最大は HAL の `rdmsr`（kernel の標本の約 24%、差分を plan に置いた、**承認待ち**）と file の fault の複写（p010、設計から）。回帰は前と同じ。

## 直近の 30 Queue

| Queue | 内容 | Status |
| --- | --- | --- |
| [q421](queue-q421.md) | coreutils の cross build（ws046-p008 の残り） | finished（2026-09-25。uncleared、p013 へ） |
| [q420](queue-q420.md) | private の file の mapping で page cache（ws046-p011） | finished（2026-09-25。uncleared、戻した） |
| [q419](queue-q419.md) | file の fault の複写の設計（ws046-p010） | finished（2026-09-25。cleared） |
| [q418](queue-q418.md) | guest の configure の遅さ（ws046-p009） | finished（2026-09-25。uncleared、原因 2 つを直した） |
| [q417](queue-q417.md) | guest で coreutils（ws046-p008 の再開） | finished（2026-09-25。uncleared、範囲の変更） |
| [q416](queue-q416.md) | WS054 の回帰と規約（ws054-p004） | finished（2026-09-25。cleared。WS054 completed） |
| [q415](queue-q415.md) | UFS の directory を複数 block に、journal（ws054-p003） | finished（2026-09-25。cleared） |
| [q414](queue-q414.md) | UFS の directory を複数 block に、journal 無し（ws054-p002） | finished（2026-09-25。cleared） |
| [q413](queue-q413.md) | UFS の複数 block の directory の設計（ws054-p001） | finished（2026-09-24。cleared） |
| [q412](queue-q412.md) | 実 package を guest で最後まで（ws046-p008） | finished（2026-09-24。uncleared、coreutils は WS054 を待つ） |
| [q411](queue-q411.md) | guest の clang の遅さ（ws046-p007 の再開） | finished（2026-09-24。uncleared、主因を直した） |
| [q410](queue-q410.md) | WS053 の規約と最後の回帰（ws053-p005） | finished（2026-09-24。cleared。WS053 completed） |
| [q409](queue-q409.md) | i386 の vmunix に LTO（ws053-p004） | finished（2026-09-24。cleared） |
| [q408](queue-q408.md) | arm64 の vmunix に LTO（ws053-p003） | finished（2026-09-24。cleared） |
| [q407](queue-q407.md) | amd64 に LTO を既定で適用（ws053-p002） | finished（2026-09-24。cleared） |
| [q406](queue-q406.md) | vmunix の LTO の調査と設計（ws053-p001） | finished（2026-09-24。cleared） |
| [q405](queue-q405.md) | guest の clang の遅さ（ws046-p007） | finished（2026-09-24。uncleared、WS053 を優先して中断） |
| [q404](queue-q404.md) | 実 package を guest で build（ws046-p004） | finished（2026-09-24。cleared、BUG-033 を p007 に引き継ぎ） |
| [q403](queue-q403.md) | make の GNU の機能（ws046-p003） | finished（2026-09-24。cleared） |
| [q402](queue-q402.md) | kernel の mkdir の `.`（ws046-p006、BUG-032） | finished（2026-09-24。cleared） |
| [q401](queue-q401.md) | POSIX make の核（ws046-p002） | finished（2026-09-24。cleared） |
| [q400](queue-q400.md) | make の調査と設計（ws046-p001） | finished（2026-09-24。cleared） |
| [q399](queue-q399.md) | WS042 の最後の回帰（ws042-p012） | finished（2026-09-24。cleared。WS042 completed） |
| [q398](queue-q398.md) | ls の規約（ws042-p011） | finished（2026-09-24。cleared） |
| [q397](queue-q397.md) | sh の規約（ws042-p010） | finished（2026-09-24。cleared） |
| [q396](queue-q396.md) | libedit の規約（ws042-p009） | finished（2026-09-24。cleared） |
| [q395](queue-q395.md) | vi mode の行編集（ws042-p008） | finished（2026-09-24。cleared） |
| [q394](queue-q394.md) | 対話の回帰（ws042-p007） | finished（2026-09-24。cleared） |
| [q393](queue-q393.md) | guest の sh の差分試験（ws042-p006） | finished（2026-09-24。cleared） |
| [q392](queue-q392.md) | configure を guest で（ws042-p005） | finished（2026-09-24。cleared） |

それより前: [全 Queue の一覧](index-all.md)。
