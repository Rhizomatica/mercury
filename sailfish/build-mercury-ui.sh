#!/bin/sh
# Build mercury-ui (the Fyne GUI with the engine linked in) natively on a
# Sailfish OS phone, from a Mercury source tree.  A technology preview:
# tested on a Jolla phone (Sailfish OS 5.2.0.22, arm64) on 2026-11-03; it
# starts, and hears the radio through PulseAudio (-x pulse).
#
#   sh sailfish/build-mercury-ui.sh <khronos-include-dir>
#
# Needs, besides the modem's build dependencies (see mercury.spec):
#   - Go >= 1.26 on PATH: Sailfish's repositories have none, so take the
#     upstream linux-arm64 tarball from go.dev;
#   - wayland-devel and libxkbcommon-devel (pkcon install, as root);
#   - the Khronos EGL, GLES2 and KHR headers in <khronos-include-dir>/{EGL,
#     GLES2,KHR}.  libhybris-libEGL-devel and -libGLESv2-devel would bring
#     them, but they need android-headers, which no repository carries; the
#     headers of any Linux distribution's libegl-dev/libgles-dev do.
#
# Run it from the phone's session environment:
#   EGL_PLATFORM=wayland WAYLAND_DISPLAY=../../display/wayland-0 \
#   XDG_RUNTIME_DIR=/run/user/$(id -u) ./mercury-ui -x pulse
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
[ $# -eq 1 ] && [ -f "$1/EGL/egl.h" ] && [ -f "$1/GLES2/gl2.h" ] ||
    { echo "usage: $0 <dir with EGL/, GLES2/ and KHR/ headers>" >&2; exit 2; }
top=$(cd "$(dirname "$0")/.." && pwd)
gl=$top/sailfish/.gl
inc=$(cd "$1" && pwd)

# The phone has the libraries (libhybris) but not their development links
# or pkg-config files: make both, inside the tree.
mkdir -p "$gl/lib" "$gl/pkgconfig"
ln -sf /usr/lib64/libEGL.so.1 "$gl/lib/libEGL.so"
ln -sf /usr/lib64/libGLESv2.so.2 "$gl/lib/libGLESv2.so"
ln -sf /usr/lib64/libwayland-egl.so.1 "$gl/lib/libwayland-egl.so"
for pc in egl:EGL:1.5 glesv2:GLESv2:3.2; do
    IFS=: read -r name lib ver <<EOF
$pc
EOF
    printf 'Name: %s\nDescription: libhybris %s\nVersion: %s\nCflags: -I%s\nLibs: -L%s -l%s\n' \
        "$name" "$lib" "$ver" "$inc" "$gl/lib" "$lib" > "$gl/pkgconfig/$name.pc"
done

export PKG_CONFIG_PATH=$gl/pkgconfig LIBRARY_PATH=$gl/lib CPATH=$inc GOTOOLCHAIN=local
cd "$top"
# Wayland only (no X11 on Sailfish), GLES2 instead of desktop GL, go-gl's
# procedures through EGL (there is no libGL), and the TLS padding libhybris
# needs (gui_interface/fyne-ui/tls_hybris.go).  Sailfish's gcc does not
# default to PIE, and the UI links -buildmode=pie.
exec make fyne-ui FYNE_UI_TAGS=wayland,gles2,egl,hybris EXTRA_CFLAGS="-g -O2 -fPIE"
