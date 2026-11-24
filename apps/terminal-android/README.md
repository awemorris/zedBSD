mikuterm for Android
====================

The terminal as an Android app: the C program of `apps/terminal-win32` on libkeiland's
Android layer, with a few small Java classes for the system's own parts.  The build uses no
Gradle: one CMake project makes the native libraries, compiles the Java, links the resources
and signs the apk.

What the build needs
--------------------

- CMake 3.20 or later, Ninja, a JDK 17 or later (`javac`, `keytool`) and `zip`.
- The Android SDK with `build-tools;35.0.0`, `platforms;android-35` and `platform-tools`
  (`emulator` and a system image to try the app).
- The Android NDK r28 (28.2.13676358).

Building
--------

    cmake -S apps/terminal-android -B build/android -G Ninja \
      -DANDROID_SDK_ROOT=$HOME/opt/android-sdk -DANDROID_NDK_ROOT=$HOME/opt/android-ndk-r28c
    cmake --build build/android --target apk

`ANDROID_SDK_ROOT` and `ANDROID_NDK_ROOT` can also come from the environment (the SDK from
`ANDROID_HOME` too).  The apk is `build/android/mikuterm-<version>.apk`, with its SHA-256 in
`mikuterm-<version>.apk.sha256`.  `MIKUTERM_ANDROID_ABIS` (default `arm64-v8a;x86_64`)
chooses the ABIs it carries.

To put it on a device or an emulator, then start it:

    cmake --build build/android --target install-apk run

`MIKUTERM_ADB_SERIAL` picks the device when more than one is attached.

Opening it in Android Studio
----------------------------

`studio/` is a Gradle project for the IDE: build, install, and debug the Java and the C on a
device or an emulator.  It is a way in, not a second build: it copies no source (the Java, the
resources, the manifest and `native/CMakeLists.txt` are read where they are), and a release apk
comes from the CMake build above.

- Open the `studio/` folder in Android Studio (a version that supports Android Gradle
  Plugin 9.5; Gradle 9.6 is fetched by the wrapper).  A command line works too:
  `cd apps/terminal-android/studio && ./gradlew assembleDebug`, and the apk is
  `app/build/outputs/apk/debug/app-debug.apk`.
- The IDE needs the SDK (`platforms;android-35`; it fetches the build-tools and CMake it wants)
  and the NDK `28.2.13676358`, the same NDK as above, and `local.properties` with `sdk.dir`
  (the IDE writes it; it is not committed; or `ANDROID_HOME` in the environment).  Native
  debugging (lldb) needs the NDK's LLDB from the SDK manager.  The assets step runs `cmake` and
  `ninja`: the SDK's CMake when it is installed there, otherwise the ones in PATH.
- What differs from the CMake build: the ABIs are the same (`arm64-v8a`, `x86_64`) and so is
  the application id, the SDK levels and the assets (made by `cmake/make-assets.cmake` through a
  configure of the top `CMakeLists.txt` in `app/build/share-assets`).  But the C is a Debug
  build (`-g`, not optimised) by Gradle's CMake, `MIKUTERM_ANDROID_HELLO` and `MIKUTERM_ANDROID_ECHO` are off, the
  apk is signed with Gradle's own debug key and `build-tools` is 36.0.0 (what the plugin wants).
  An apk of the CMake build and one of Gradle's are signed with different keys, so `adb uninstall
  io.noctvm.mikuterm` before changing from one to the other.
- Gradle warns that `package` and `extractNativeLibs` are in `AndroidManifest.xml`: the CMake
  build's `aapt2` needs them there, and Gradle ignores the first, and `packaging.jniLibs.useLegacyPackaging = false` in
  `studio/app/build.gradle` keeps the libraries stored as the second asks.

Signing
-------

By default the apk is signed with a debug key that the build makes in its own folder, so a
build folder's apk cannot update another folder's (uninstall first).  The apk is also
debuggable.  For a release, give the key store and the key's alias, and the store's password
in the environment (it is never put on a command line):

    MIKUTERM_KS_PASS=... cmake -S apps/terminal-android -B build/release -G Ninja \
      -DANDROID_SDK_ROOT=... -DANDROID_NDK_ROOT=... \
      -DMIKUTERM_KEYSTORE=/path/to/release.jks -DMIKUTERM_KEY_ALIAS=<alias> \
      -DMIKUTERM_DEBUGGABLE=OFF

Keep the key store out of the source tree.

X11 forwarding
--------------

mikuterm runs its X server on an in-process thread per SSH connection when `ForwardX11 yes` is
set in `ssh/config`.

- Top-level X windows open as tabs (menus, tooltips, and owned dialogs are composited over their
  home tab); `X11WindowTabify` is ignored because Android has no separate OS windows.
- `X11RootWindow WxH` opens the whole root window in a single tab with two-finger pan and pinch
  zoom (snapping to the fit scale and 1.0).
- `X11Render` is always CPU rendering and X11/Android clipboard bridging is not enabled.
- `X11Scale` defaults to the screen density (`X11Scale auto`).
- Developer note: the `io.noctvm.keiland.DEBUG_PINCH` and `io.noctvm.keiland.DEBUG_PAN` debug
  broadcasts are accepted only when the package is debuggable (`MIKUTERM_DEBUGGABLE=OFF` disables
  them in release builds).
