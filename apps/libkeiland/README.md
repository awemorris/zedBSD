# libkeiland platform layers

libkeiland is the small GUI library that zedBSD's desktop programs and the mikuterm
terminal draw with.  Most of it is portable C and lives in `userland/desktop/libkeiland/`
(the widgets in `ui/`, the platform-neutral code, and zedBSD's own backend).  This
directory holds the layers that sit outside zedBSD and let other programs use the same
library on another operating system.

- `win32/`: the Windows backend.  It implements the window, input, IME, clipboard,
  drag and drop, title bar and presentation on the Win32 API, and the stubs
  (`unsupported.c`) of what Windows does not have.  It is used by
  `apps/terminal-win32` (mikuterm for Windows) and `apps/browser-win32`.
- `cmake/keiland-sources.cmake`: the lists of source files a CMake build reads, as
  paths from the root of the tree.  `KEILAND_COMMON_SOURCES` are the files every
  system shares; `KEILAND_WIN32_SOURCES` are a Windows program's files (the Windows
  platform layer in `userland/desktop/libkeiland/platform/` and `win32/`).
  `apps/terminal-win32/CMakeLists.txt` includes it.

`apps/browser-win32/Makefile` names the same Windows files in its own lists.
