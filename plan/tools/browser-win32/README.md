# plan/tools/browser-win32

The tests of the Windows build of the Web browser (OmniGrammaton, `apps/browser-win32/`: libbrowser, libmedia and
libkeiland's Win32 implementation, cross compiled into a Windows .exe), and the host tests of libbrowser that were
written with it. Wine is the reference, not the judgement of a real Windows. Every script runs from any directory
(it changes to the top of the tree) and writes only under `build/tmp/browser-win32-*` and `build/review/`.
Wine commands take `flock /tmp/zedbsd-wine.lock`; do not wrap a script in that lock again.
The toolchains are the host's mingw-w64 (x86_64, the CI's) and llvm-mingw (`LLVM_MINGW`, default
`$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64`, for aarch64 and the unit tests).

| Script | What it checks | Last line when it passes |
| --- | --- | --- |
| `cross-build.sh` | `make browser-win32` and `apps/browser-win32/Makefile` for each arch of `WIN32_ARCH` (default x86_64; `"x86_64 aarch64"` for both): warning 0 in the Windows files, the undefined names against `allowed-imports.txt`, the exports, the DLL imports, the zip's contents against `zip-contents.txt`, the script tool | `cross-build: PASS` |
| `run-unit.sh all` (or names, `--posix`, `--win32`) | the unit tests `unit/test-*.c` of the Windows build's modules (the operating-system layers, storage, libkeiland's Win32 files, the apps' tools) on the host with ASan and UBSan and under Wine; `modules.sh` lists the sources each test links | `run-unit: PASS` |
| `wine-run.sh` | OmniGrammaton from its zip under Wine with lavapipe: the items M1 to M8 Wine can show (headless output the same bytes as the host's, windows, input, titlebar hit-test, theme, TSF, protection, certificate stores); pictures in `build/review/browser-win32-wine/` | `wine-run: PASS` |
| `script-core-link.sh` | libbrowser's script core alone as an archive (host and Windows): only C library and system names undefined, and the JavaScript tests through `script-core-main.c` (still `plan/ws334/tests/script-core-main.c`, which `apps/browser-win32/Makefile` names) | `script-core-link: PASS` |
| `fetch-wine.sh`, `tls-wine.sh`, `media-wine.sh` | under Wine: the network against a local server, https with libssl-compat and the Windows certificate stores, libmedia's H.264 MP4 against the host's output | `fetch-wine: PASS`, `tls-wine: PASS`, `media-wine: PASS` |
| `host-tests-windows.sh` | libbrowser's host tests (`plan/ws074/tests/host-*.c`, runners, GPU pictures, Interop reftests) on the host and under Wine, with the differences listed (`compare-ppm.py`, `compare-reftests.py`, `binary-mode.c`, `win32-posix.h`) | `host-tests-windows: done` with the number of differences (each needs a reason) |
| `keiland-common.sh` | which libkeiland files compile unchanged for Windows, against `LIBKEILAND_WIN32_COMMON_SOURCES` of the Makefile | `keiland-common: PASS` |
| `python3 -I plan/tools/browser-win32/keiland-stubs.py --check` | libkeiland's Windows stubs (`unsupported.c`) are what the generator writes (without `--check` it writes them) | `keiland-stubs: PASS` |
| `stage1.sh TREE plain\|asan [PORT_OFFSET]` | the browser's host tests on one tree (host build, host-* tests, JavaScript, network, component), for counting before and after a change | the summary lines in `TREE/build/tmp/browser-win32-stage1-VARIANT/summary.txt` |
| `storage-test.sh`, `scheme-test.sh`, `net-ring-test.sh`, `media-wake-test.sh` (`plain`, `asan` or `tsan`) | host tests of libbrowser and libmedia over the WS074 host build: storage instances and the persistent cache, the color scheme, the loader's threads, libmedia's wake-ups | `<name>: PASS` |
