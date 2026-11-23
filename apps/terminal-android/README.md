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
