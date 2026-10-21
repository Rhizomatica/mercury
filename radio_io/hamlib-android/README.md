# Hamlib for Android (vendored)

A static Hamlib for the Android `.apk` (`make fyne-ui-android`): Android has
no system Hamlib, and `pkg-config` on the build host would answer for the
host's.  Built from the Rhizomatica fork's `usb-serial-userspace` branch,
which adds `usb:`/`usbfd:` serial ports (Hamlib drives the rig's
USB-serial chip itself through usbfs: an Android app gets no
`/dev/ttyUSB*`).  Upstream: Hamlib/Hamlib#2272.

- Source: https://github.com/Rhizomatica/Hamlib, commit `9af8b8b8d`
- Toolchain: Android NDK r27d (27.3.13750724), API 26, arm64-v8a
- Licence: LGPL-2.1-or-later (`COPYING.LIB.txt`)

Build:

    T=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
    ./bootstrap
    PKG_CONFIG_LIBDIR=/nonexistent ac_cv_header_glob_h=no ./configure \
        --host=aarch64-linux-android \
        CC=$T/bin/aarch64-linux-android26-clang AR=$T/bin/llvm-ar \
        RANLIB=$T/bin/llvm-ranlib STRIP=$T/bin/llvm-strip CFLAGS="-O2 -fPIC" \
        --enable-static --disable-shared --without-libusb --without-readline \
        --without-samplerate --without-indi --without-cxx-binding \
        --without-xml-support
    make -C src
    $T/bin/llvm-strip --strip-debug src/.libs/libhamlib.a

`ac_cv_header_glob_h=no`: `glob()` is in Android's libc only from API 28;
without it the microHam device search is left out (no microHam devices on
a phone).  `include/hamlib/` is the source tree's `include/hamlib/*.h`.
