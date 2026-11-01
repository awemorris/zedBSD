<!-- awesome-plan project=zedbsd record=ws334-p026 -->

# ws334-p026 統合: libmedia の Windows（Vulkan の loader・mediafile・GPU の絵の fd・Raspberry Pi 4 の回避）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §2.8・§5（M2〜M9）。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1853-i01、P5）。前: planned（2026-11-01 p002 で作成）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: `libmedia.dll` と `.def` を作らず static library `libmedia.a`（p036 の形）。winpthreads は .exe の link で静的に入る。cross-build.sh は libmedia の object の `nm -u` の照合（依存の DLL は .exe の側で見る）。下の本文の `libmedia.dll` はこの通りに読み替える。
- 依存: p013（media-os）・p022（起こしの callback）。
- 担当の想定: Opus（phase-runner）を 1 回。使う約束は `libmedia/media-os.h`。zedBSD・Linux の挙動を変えない（M7 は arm64 の zedBSD の Raspberry Pi 4 で同じ結果になる形）。

## 変える file と中身

1. **M2 `vkvideo-runtime.c`**: `vkvideo_loader` の `dlopen` 2 回を `media_os_vulkan_open(&vkvideo_library)`（`static struct media_os_library *vkvideo_library`）、`dlsym(vkvideo_library, "vkGetInstanceProcAddr")` を `media_os_library_symbol`（`media_os_function` から `PFN_vkGetInstanceProcAddr` へは今の `memcpy` の形で）。`<dlfcn.h>` を外す。
2. **M4 `mediafile/mediafile.c`**: `opened->fd`・`open(O_CLOEXEC)`・`fstat`（:52・:60）を `media_os_file_open(path, &opened->file, &opened->size)`、`pread`（:288 の loop）を `media_os_file_read_at`、`close` を `media_os_file_close`。12 byte 未満の EINVAL は今のまま。`<fcntl.h>`・`<sys/stat.h>`・`<unistd.h>` を外す。`mediafile-private.h` の `int fd` を `struct media_os_file *file` に。
3. **M9 `picture.c:442`**: `close(slots->fd[index])` を `media_os_close_descriptor`。
4. **M7 `vkvideo-runtime.c:36-46`・`vkvideo.c:18-28`**: `#if defined(__aarch64__)` の `VKVIDEO_FLIGHTS`・`VKVIDEO_SKIP_FAILED` を実行時の値にする: device を選んだ時に `VkPhysicalDeviceProperties` の `vendorID` が 0x14e4（zedBSD の Raspberry Pi 4 の driver の値、`src/drivers/gpu/bcm2711/vk/vulkan-query.c:428`）の時だけ flights 4・失敗を飛ばす、他は 1・飛ばさない。値は runtime の構造体（`struct vkvideo_runtime` に `unsigned flights`・`int skip_failed`）に持ち、`VKVIDEO_FLIGHTS` を使う配列の大きさは最大の 4 で取る。最初に bcm2711 の Vulkan Video の queue がこの vendor の device にあることを source で確かめる（違えば止めて Q1 に）。comment の「only arm64 Vulkan Video device」を「the Raspberry Pi 4's firmware decoder, known by its vendor」に。
5. **起こし**: p022 で済み。
6. **sources.mk**: `LIBMEDIA_SOURCES` に `media-os-posix.c`。Windows の Makefile は `media-os-win32.c`。
7. **Windows の build**: `apps/browser-win32/Makefile` に `libmedia.dll`（`libmedia/exports.map` の glob の .def、`-lucrt`・winpthreads、Vulkan は media-os の実行時の読み込みなので import しない）。`cross-build.sh` に libmedia の `nm -u` と `objdump -p` の依存の DLL（kernel32・UCRT の api-ms-win-crt-*・libwinpthread-1.dll か静的）を足す。

## 試験（受け入れ）

- libmedia の host の試験が前と同じ件数（`plan/tools/h264sw/run-unit.sh all`、libmedia の他の host の試験を `plan/tools.md` で探す）。
- Wine: `plan/ws334/tests/media-main.c`（`media_engine_open` で同梱の小さな H.264 の mp4（試験の data は `plan/tools/h264sw/` の conformance の cache か tree の中の物を Q1 に確かめる）を開き、software の H.264 で数枚を取り出して大きさと最初の絵の hash を出す）が host の同じ program と同じ hash。Wine の Vulkan（lavapipe）は Vulkan Video を持たない見込みなので software の道を通る（Vulkan Video の道は実機の Windows で、p032 以後）。
- `run-unit.sh all`・`cross-build.sh` PASS。zedBSD amd64 の `libmedia.so`・arm64 の `libmedia.so` の build が warning 0（arm64 の command は p020 の amd64 の物の platform を config-arm64 に）。
- `git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。libbrowser（p022 で済み）。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA と結果を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-01、P5、q1853-i01、基点 96b67d64b）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| 70964bec9 | libmedia の picture.c（M9）・vkvideo-runtime.c/.h・vkvideo.c（M2・M7）・sources.mk（`LIBMEDIA_OS_SOURCES`）、mediafile の mediafile.c・mediafile-private.h（M4） |
| bbeb3edf7 | apps/browser-win32/Makefile（libmedia.a に libmedia の全部）、plan/ws334/tests の cross-build.sh（3c: libmedia.a の照合）・allowed-imports.txt・新しい media-main.c と media-wine.sh、host の試験の build に media-os-posix.c を 1 語（plan/tools/media の 2 本、plan/tools/h264sw/backend の 2 本、plan/ws074/tests/host-build.sh。Q1 が範囲に入れた） |

前後の比較（前は `git archive 96b67d64b` を `build/tmp/p026-base` に展開して流した。後は worktree）:

| 確かめ | 前（96b67d64b） | 後 |
| --- | --- | --- |
| `stage1.sh plain` / `asan`（host-build、host-* の exit、js・loader・http・http-async・fetch・component） | host-build exit 0・warning 0・built 94、host-* 75 exit 0・17 other、js 17/17、loader 11/11、http 15/15、http-async 19/20（FAIL は async-sheets）、fetch 38/38、component 83 | 同じ（plain・asan とも host-exits.txt が一致、http-async の FAIL も同じ async-sheets） |
| `plan/tools/media/run-host-mediafile.sh` | PASS（10 行） | log が同じ bytes |
| `plan/tools/media/run-host-codec.sh` | PASS（9 行） | 同じ |
| `plan/tools/h264sw/run-unit.sh all` | PASS（24 本） | 同じ |
| `plan/tools/h264sw/backend/run-backend.sh`・`run-host-decode.sh` | PASS・PASS | 同じ |
| `plan/tools/h264sw/run-stream.sh` | PASS | 同じ |
| `plan/ws334/tests/media-wake-test.sh plain` / `asan` | PASS / PASS | 同じ |

他の確かめ:

- `timeout 2400 sh plan/ws334/tests/media-wine.sh` → `media-wine: PASS`（新しい試験）。試料は tree の `plan/ws083/tests/streams/pb-main-352.h264`（Main、I・P・B、352x288、15 枚）を host の ffmpeg が符号化し直さず MP4 に入れた物（Q1 に確かめる試料の件は、tree の中の物で済ませた）。host の program（libmedia の zedBSD の source の一覧、POSIX の media-os）と Windows の `media-tool.exe`（test-program で全部の static library と link）が同じ行を出す: decoder の層で 15 枚、最初の 8 枚の大きさと hash、engine の最初の絵の hash（`f2b1e755940a6453`）、backend `h264-software`。Wine の側で `vulkan-1.dll` を media-os（`LoadLibraryExW`）で開き、`vkGetPhysicalDeviceProperties`（M7 の新しい呼び出し）と queue family の問い合わせまで通ってから software に落ちることを log で見る（host の llvmpipe も Wine も video の queue を持たない）。`media-tool.exe` の import は KERNEL32 と UCRT の api-ms-win-crt-* の 10 個だけ（libmedia は Vulkan を実行時に読むので vulkan-1.dll の import も無い）。
- `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（libmedia.a が libmedia の全部で x86_64・aarch64 とも warning 0。新しい 3c: libmedia.a の import は win64 46・winarm64 44 で全部が表の中、`vk*` を名前で参照しない。表に kernel32 の GetFileSizeEx・GetFileType・MultiByteToWideChar、UCRT の exp2・lrint、winpthreads の clock_gettime64・nanosleep64 を足した。3b は除いていた `media_*` を libmedia.a の定義で解くようにした。libbrowser.a の import の数は win64 165・winarm64 161 で前と同じ）。
- `LC_ALL=C.UTF-8 timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` → FAIL（`libkeiland/platform/platform.h` だけ。基点でも同じで、p023 の Q1 の判断の通り p035 で扱う。media-os.h・media-os-*.c は変えていない）。
- zedBSD amd64（`config-amd64.mk`）の `build/amd64/dynamic/libmedia.so` と利用側の `bin/videoplayer`・`bin/music`・`bin/media-probe`・`bin/browser`、arm64（`config-rpi4.mk`）の同じ 5 つ → exit 0、warning 0。sysroot は main の `build/<arch>/sysroot` を `build/tmp/p026/sysroot-<arch>` に写し tree の `include/libc`・`userland/desktop/include` を重ねた物（p020 の方法）。`check-dynamic-elf.py` を `/usr/bin/python3` で流し直して amd64・arm64 とも exit 0。
- `git diff --check` → 出力なし。

自分で決めた点・phase.md と code の食い違い:

- M7: phase.md の `unsigned flights` の名前は、runtime に既に今の飛んでいる枚数 `uint32_t flights` があるので、上限を `flight_limit`、飛ばす可否を `skip_failed` にした。device を probe する度に `vkvideo_firmware`（`vkvideo_device` の loop の頭、`vkvideo_family` の前）が `vkGetPhysicalDeviceProperties` の `vendorID` を見て決める（0x14e4 なら 4・飛ばす、他は 1・飛ばさない）。`vkGetPhysicalDeviceProperties` を instance の dispatch の表に足した（Vulkan 1.0 の core、必須）。`vkvideo.c` は新しい `media_vkvideo_runtime_skips_failed` で聞く（vkvideo-runtime.h に 1 行）。配列の大きさは `VKVIDEO_FLIGHTS` 4 のまま。source の確かめ: `src/drivers/gpu/bcm2711/vk/vulkan-query.c:429` が vendorID 0x14e4、同じ file の :273-277 が唯一の queue family に `BCM2711_VIDEO_QUEUE_DECODE`（0x20、video decode の bit）を足す。他に video decode の queue を出す driver は i915・nvrtx（amd64）だけで、zedBSD arm64 の結果は Pi 4 で前と同じになる見込み。
- 書き直した comment から開発の管理情報（ws236 の番号）を外した。log の文「no Vulkan library (libvulkan.so.1, libvulkan.so)」は Windows で名前が違うので「no Vulkan loader」にした（この文を見る試験は無い）。
- M4: `media_file_open` の誤りは media-os の写しになる。前と違うのは端の場合だけ: 空の path は ENOENT → EINVAL、directory は（開けた後の読みで）EINVAL・EIO → EISDIR、ENOTDIR → ENOENT、EMFILE・ENFILE → ENOMEM、普通の file でない物は EIO。利用者（engine・Music・Video Player・media-probe）は値を素通しするだけで分岐しない。`mf_read_at` の読みの失敗は前と同じく全部 EIO。
- 手順 7（Windows の build）: Makefile は `LIBMEDIA_SOURCES` から `LIBMEDIA_OS_SOURCES`（media-os-posix.c）を除いた `LIBMEDIA_CORE_SOURCES` と layers の media-os-win32.c で libmedia.a を作る（p036 の形の読み替えの通り DLL・.def は作らない）。libmedia の object は Vulkan の header の後に compile する。
- media-main.c の engine の最初の絵は decoder の層の 2 枚目と同じ hash（engine は表示の時刻が 0 より前の絵を飛ばすため）。host と Wine で同じなので、比べる値としてそのまま使う。

未実施:

- Raspberry Pi 4 の上の Vulkan Video（flights 4・失敗を飛ばす道）の確かめ（実機・QEMU はこの Queue で使わない）。host には vendor 0x14e4 の device が無く、M7 の Pi 4 の枝は compile と source の読みだけ。
- 実機の Windows での Vulkan Video の道（p032 以後）、aarch64 の Windows の実行（link と表だけ）、Linux の Keiland・FreeBSD の build。
- 変異の確かめ（統合の Phase なので受け入れに無い。media-os の module は p013 で済み）。

消してよい物（Q1 へ）: `build/tmp/p026-base`（基点の展開と試験の出力）、`build/tmp/p026`（sysroot の写し、Windows の試しの build、log）、`build/tmp/p026-*.log`・`build/tmp/p026-media.sh`、`build/tmp/ws334-media-build`・`build/tmp/ws334-media-wine*`（media-wine.sh の出力）、`build/arm64`（zedBSD arm64 の試しの build の出力）。どれも使い捨て。

