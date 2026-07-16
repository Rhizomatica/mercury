# macOS universal, self-contained build (vendored static hamlib)

This document describes how the macOS build of Mercury is made **universal**
(`x86_64` + `arm64`) and **self-contained** — with no Homebrew dependency at
build or run time — by vendoring **static, universal** hamlib and libusb into
the repository, mirroring the way Windows hamlib is vendored in
`radio_io/hamlib-w64/`.

It contains:

1. What is vendored and where (`radio_io/hamlib-macos/`).
2. How the universal static libs were built (reproducible recipe).
3. The **proposed** Makefile changes to consume them.
4. The exact commands that were verified end-to-end, with proof output.

> Status: the vendored artifacts under `radio_io/hamlib-macos/` are in place.
> The Makefile changes below are a **proposal** — they are written here but not
> yet applied to the tracked `Makefile` / `radio_io/Makefile`.

---

## 1. Vendored artifacts

Layout mirrors `radio_io/hamlib-w64/` (a `lib/` with the archive(s), an
`include/hamlib/` with the public headers, and the LGPL license text):

```
radio_io/hamlib-macos/
├── COPYING.LIB.txt              # LGPL 2.1 (hamlib + libusb are LGPL)
├── include/hamlib/*.h           # public hamlib headers (arch-independent)
└── lib/
    ├── libhamlib.a              # UNIVERSAL static (x86_64 + arm64)
    └── libusb-1.0.a             # UNIVERSAL static (x86_64 + arm64)
```

Versions (matched to what Homebrew installs on the build host):

| Library | Version |
|---------|---------|
| hamlib  | 4.7.2   |
| libusb  | 1.0.30  |

Verify on macOS:

```console
$ lipo -archs radio_io/hamlib-macos/lib/libhamlib.a
x86_64 arm64
$ lipo -archs radio_io/hamlib-macos/lib/libusb-1.0.a
x86_64 arm64
$ file radio_io/hamlib-macos/lib/libhamlib.a
… Mach-O universal binary with 2 architectures: [x86_64 …] [arm64 …]
```

The headers deliberately **exclude** hamlib's internal `config.h` (a build-time
header, not part of the public API), matching the `hamlib-w64` header set.

---

## 2. How the universal static libs were built

Built on an Intel (x86_64) macOS host with Apple clang, which cross-compiles the
arm64 slice with `-arch arm64`. Intel Homebrew has **no arm64 bottles**, so both
libs must be built from source, per arch, then combined with `lipo`.

Two gotchas were hit and worked around:

* **libtool nested-archive corruption.** If hamlib's `configure` is given the
  libusb static archive as a bare path in `LIBUSB_LIBS` (e.g.
  `.../libusb-1.0.a`), libtool *embeds the whole `libusb-1.0.a` as a member
  inside `libhamlib.a`*. The result fails to link with
  `ld: archive member 'libusb-1.0.a' not a mach-o file`. **Fix:** pass libusb as
  a normal link reference `-L<dir> -lusb-1.0` so `libhamlib.a` stays pure hamlib
  objects; supply the static `libusb-1.0.a` explicitly at the *final* link.
* **hamlib CLI tools break the cross-build.** `make` (all) tries to link the
  example programs (`rigctl`, `rigsmtr`, …) which fail under `-arch`. **Fix:**
  build only the library target: `make -C src libhamlib.la`, and install the
  `.a` + headers by hand. Use a *fresh* extraction per arch and
  `--disable-maintainer-mode` so make does not try to re-run autoconf.

### libusb (per arch A in {x86_64, arm64}; triple x86_64-apple-darwin / aarch64-apple-darwin)

```sh
./configure --prefix=$OUT/libusb-$A \
  --enable-static --disable-shared --disable-udev \
  CC="clang -arch $A" CFLAGS="-arch $A -O2" \
  --host=$TRIPLE
make -j4 && make install
```

### hamlib 4.7.2 (per arch A)

```sh
LU=$OUT/libusb-$A
./configure --prefix=$OUT/hamlib-$A \
  --enable-static --disable-shared --without-cxx-binding \
  --disable-maintainer-mode --host=$TRIPLE \
  CC="clang -arch $A" CXX="clang++ -arch $A" CFLAGS="-arch $A -O2" \
  CPPFLAGS="-I$LU/include/libusb-1.0" \
  LIBUSB_CFLAGS="-I$LU/include/libusb-1.0" \
  LIBUSB_LIBS="-L$LU/lib -lusb-1.0 -framework IOKit -framework CoreFoundation -framework Security"
# build ONLY the library (skip the CLI tools that break under -arch):
make -C src libhamlib.la
# install by hand:
cp src/.libs/libhamlib.a  $OUT/hamlib-$A/lib/libhamlib.a
cp include/hamlib/*.h     $OUT/hamlib-$A/include/hamlib/
```

`configure` must report `Enable USB backends  yes` (USB/serial rig support kept).

### Combine into universal archives

```sh
lipo -create $OUT/hamlib-x86_64/lib/libhamlib.a  $OUT/hamlib-arm64/lib/libhamlib.a  -output libhamlib.a
lipo -create $OUT/libusb-x86_64/lib/libusb-1.0.a $OUT/libusb-arm64/lib/libusb-1.0.a -output libusb-1.0.a
lipo -archs libhamlib.a   # -> x86_64 arm64
lipo -archs libusb-1.0.a  # -> x86_64 arm64
```

Sanity-check that each slice really contains hamlib code (not an empty/aliased
archive):

```sh
lipo libhamlib.a -thin x86_64 -output /tmp/h.a && nm /tmp/h.a | grep -c 'T _rig_init'  # -> 1
lipo libhamlib.a -thin arm64  -output /tmp/h.a && nm /tmp/h.a | grep -c 'T _rig_init'  # -> 1
```

---

## 3. Proposed Makefile changes

The goal: on Darwin, when `radio_io/hamlib-macos/lib/libhamlib*.a` is present,
prefer the vendored **static** libs + Apple frameworks over the Homebrew
`pkg-config` path — exactly mirroring the existing `HAMLIB_W64_DIR` /
`ifneq ($(strip $(HAMLIB_W64_LIBS)),)` pattern for Windows.

### 3a. Top-level `Makefile` (Darwin hamlib branch)

Add a `HAMLIB_MACOS_DIR` alongside `HAMLIB_W64_DIR`:

```make
HAMLIB_W64_DIR    = radio_io/hamlib-w64
HAMLIB_W64_LIBS   = $(wildcard $(HAMLIB_W64_DIR)/lib/libhamlib*.a)
HAMLIB_MACOS_DIR  = radio_io/hamlib-macos
HAMLIB_MACOS_LIBS = $(wildcard $(HAMLIB_MACOS_DIR)/lib/libhamlib*.a)
```

Then, in the `Darwin` arm of the platform `ifeq`, replace the shared
`pkg-config` hamlib detection *for Darwin only* with a vendored-first branch.
The current code (config lines ~49-62) is:

```make
    ifeq ($(UNAME_S),Darwin)
	FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif
    ...
    HAVE_HAMLIB := $(shell pkg-config --exists hamlib 2>/dev/null && echo 1)
    ifeq ($(HAVE_HAMLIB),1)
	HAMLIB_CFLAGS := $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
	HAMLIB_LDFLAGS := $(shell pkg-config --libs hamlib)
    else
	...
```

Proposed replacement (Darwin prefers vendored static; other UNIX unchanged):

```make
    ifeq ($(UNAME_S),Darwin)
	FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif

    # macOS frameworks needed by hamlib's USB/serial backends.
    MACOS_HAMLIB_FRAMEWORKS = -framework IOKit -framework CoreFoundation -framework Security

    ifeq ($(UNAME_S),Darwin)
      ifneq ($(strip $(HAMLIB_MACOS_LIBS)),)
        # Vendored, universal, static hamlib+libusb — no Homebrew dependency.
        HAVE_HAMLIB   := 1
        HAMLIB_CFLAGS := -I$(HAMLIB_MACOS_DIR)/include -DHAVE_HAMLIB
        HAMLIB_LDFLAGS := $(HAMLIB_MACOS_DIR)/lib/libhamlib.a \
                          $(HAMLIB_MACOS_DIR)/lib/libusb-1.0.a \
                          $(MACOS_HAMLIB_FRAMEWORKS)
      else
        # Fall back to Homebrew pkg-config if the vendored libs are absent.
        HAVE_HAMLIB := $(shell pkg-config --exists hamlib 2>/dev/null && echo 1)
        ifeq ($(HAVE_HAMLIB),1)
          HAMLIB_CFLAGS := $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
          HAMLIB_LDFLAGS := $(shell pkg-config --libs hamlib)
        endif
      endif
    else
      # Linux / FreeBSD: unchanged pkg-config detection.
      HAVE_HAMLIB := $(shell pkg-config --exists hamlib 2>/dev/null && echo 1)
      ifeq ($(HAVE_HAMLIB),1)
        HAMLIB_CFLAGS := $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
        HAMLIB_LDFLAGS := $(shell pkg-config --libs hamlib)
      endif
    endif
    ifneq ($(HAVE_HAMLIB),1)
      HAVE_HAMLIB    = 0
      HAMLIB_CFLAGS  =
      HAMLIB_LDFLAGS =
    endif
```

Order matters: `libhamlib.a` **before** `libusb-1.0.a` (hamlib references
libusb), and both before the frameworks.

### 3b. `radio_io/Makefile` (subdir hamlib include path)

`radio_io/Makefile` re-derives its own include flags (`HAMLIB_CFLAGS_LOCAL`).
Today it has a Windows branch (`-Ihamlib-w64/include`) and otherwise calls
`pkg-config` (Homebrew). Add a Darwin branch mirroring Windows so the subdir
compiles against the **vendored** headers, not Homebrew's:

Current (lines ~37-45):

```make
ifeq ($(HAVE_HAMLIB),1)
ifeq ($(OS),Windows_NT)
HAMLIB_CFLAGS_LOCAL = -Ihamlib-w64/include -DHAVE_HAMLIB
else
HAMLIB_CFLAGS_LOCAL = $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
endif
else
HAMLIB_CFLAGS_LOCAL =
endif
```

Proposed:

```make
ifeq ($(HAVE_HAMLIB),1)
ifeq ($(OS),Windows_NT)
HAMLIB_CFLAGS_LOCAL = -Ihamlib-w64/include -DHAVE_HAMLIB
else ifneq ($(wildcard hamlib-macos/lib/libhamlib*.a),)
HAMLIB_CFLAGS_LOCAL = -Ihamlib-macos/include -DHAVE_HAMLIB
else
HAMLIB_CFLAGS_LOCAL = $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
endif
else
HAMLIB_CFLAGS_LOCAL =
endif
```

(The header ABI is compatible with Homebrew's 4.7.2, so even without this change
the object files compile; the change removes the residual Homebrew include-path
reference for a truly Homebrew-free build tree.)

### 3c. Universal CLI (`-arch x86_64 -arch arm64`)

`clang` compiles universal object files in one invocation, **but** an
intermediate `ar` archive that contains *fat* `.o` members is rejected by `ld`
(`archive member 'audioio.o' not a mach-o file`). The robust, portable approach
(also required for the Go UI) is **build per-arch, then `lipo` the final
binary**. Proposed `macos-universal` target for the top-level Makefile:

```make
# Universal (x86_64 + arm64), self-contained macOS build.
# Builds each slice against the matching thin slice of the vendored fat
# static libs, then lipo-combines the two mercury binaries.
MACOS_ARCHS = x86_64 arm64

macos-universal:
	@for A in $(MACOS_ARCHS); do \
	  $(MAKE) clean; \
	  mkdir -p build-$$A; \
	  lipo $(HAMLIB_MACOS_DIR)/lib/libhamlib.a  -thin $$A -output build-$$A/libhamlib.a; \
	  lipo $(HAMLIB_MACOS_DIR)/lib/libusb-1.0.a -thin $$A -output build-$$A/libusb-1.0.a; \
	  $(MAKE) internal_deps CC="clang -arch $$A" HAVE_HAMLIB=1 \
	      HAMLIB_CFLAGS="-I$(HAMLIB_MACOS_DIR)/include -DHAVE_HAMLIB"; \
	  $(MAKE) mercury CC="clang -arch $$A" HAVE_HAMLIB=1 \
	      HAMLIB_CFLAGS="-I$(HAMLIB_MACOS_DIR)/include -DHAVE_HAMLIB" \
	      HAMLIB_LDFLAGS="build-$$A/libhamlib.a build-$$A/libusb-1.0.a $(MACOS_HAMLIB_FRAMEWORKS)"; \
	  mv mercury build-$$A/mercury; \
	done
	lipo -create build-x86_64/mercury build-arm64/mercury -output mercury
	@lipo -archs mercury
```

Note: `internal_deps` must complete **before** `mercury` (separate `$(MAKE)`
invocations, not one parallel `make internal_deps mercury -j`) — otherwise the
`mercury` link races the sub-archive builds
(`No rule to make target 'modem/freedv/libfreedvdata.a'`).

### 3d. Universal Go UI (`mercury-ui`)

The UI links hamlib via `#cgo pkg-config: hamlib` in
`gui_interface/fyne-ui/mercury_link_darwin.go`. To keep that file unchanged
while sourcing the **vendored static** libs, point `PKG_CONFIG_PATH` at a
generated `hamlib.pc` that emits the vendored `.a`s + frameworks. Build each
arch, then `lipo`. Proposed `fyne-ui-macos-universal` target:

```make
fyne-ui-macos-universal:
	@for A in x86_64 arm64; do \
	  case $$A in x86_64) GOA=amd64;; arm64) GOA=arm64;; esac; \
	  $(MAKE) clean; \
	  mkdir -p build-$$A; \
	  lipo $(HAMLIB_MACOS_DIR)/lib/libhamlib.a  -thin $$A -output build-$$A/libhamlib.a; \
	  lipo $(HAMLIB_MACOS_DIR)/lib/libusb-1.0.a -thin $$A -output build-$$A/libusb-1.0.a; \
	  $(MAKE) libmercury_core.a CC="clang -arch $$A" HAVE_HAMLIB=1 \
	      HAMLIB_CFLAGS="-I$(HAMLIB_MACOS_DIR)/include -DHAVE_HAMLIB"; \
	  printf 'Name: hamlib\nVersion: 4.7.2\nCflags: -I$(abspath $(HAMLIB_MACOS_DIR))/include -DHAVE_HAMLIB\nLibs: $(abspath build-$$A)/libhamlib.a $(abspath build-$$A)/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security\n' > build-$$A/hamlib.pc; \
	  ( cd $(FYNE_UI_DIR) && \
	    CGO_ENABLED=1 GOARCH=$$GOA GOOS=darwin CC="clang -arch $$A" \
	    PKG_CONFIG_PATH=$(abspath build-$$A) \
	    go build -tags mercury_embedded \
	      -ldflags "-X main.coreBuildID=$$(cksum $(abspath libmercury_core.a) | cut -d' ' -f1)" \
	      -o $(abspath build-$$A)/mercury-ui . ); \
	done
	lipo -create build-x86_64/mercury-ui build-arm64/mercury-ui -output mercury-ui
	@lipo -archs mercury-ui
```

> The cleaner long-term alternative is to teach `mercury_link_darwin.go` to
> prefer the vendored static libs (e.g. drop `#cgo pkg-config: hamlib` and add a
> `#cgo darwin LDFLAGS: ${SRCDIR}/../../radio_io/hamlib-macos/lib/libhamlib.a
> ${SRCDIR}/../../radio_io/hamlib-macos/lib/libusb-1.0.a -framework IOKit
> -framework CoreFoundation -framework Security`). That edits a tracked source
> file, so it is only noted here — the `PKG_CONFIG_PATH` shim above achieves the
> same result without touching tracked sources.

---

## 4. Verified commands and proof output

All of the following was run against the vendored libs in
`radio_io/hamlib-macos/` on the macOS 15.7 build host (Apple clang 17, go 1.26.5).

### CLI — universal, no Homebrew hamlib

Per-arch build then lipo (mirrors the `macos-universal` target above):

```sh
for A in x86_64 arm64; do
  make clean
  lipo radio_io/hamlib-macos/lib/libhamlib.a  -thin $A -output /tmp/vlib-$A/libhamlib.a
  lipo radio_io/hamlib-macos/lib/libusb-1.0.a -thin $A -output /tmp/vlib-$A/libusb-1.0.a
  make -j4 internal_deps CC="clang -arch $A" HAVE_HAMLIB=1 \
       HAMLIB_CFLAGS="-Iradio_io/hamlib-macos/include -DHAVE_HAMLIB"
  make mercury CC="clang -arch $A" HAVE_HAMLIB=1 \
       HAMLIB_CFLAGS="-Iradio_io/hamlib-macos/include -DHAVE_HAMLIB" \
       HAMLIB_LDFLAGS="/tmp/vlib-$A/libhamlib.a /tmp/vlib-$A/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security"
  mv mercury /tmp/mercury-$A
done
lipo -create /tmp/mercury-x86_64 /tmp/mercury-arm64 -output mercury
```

Proof:

```console
$ lipo -archs mercury
x86_64 arm64

$ otool -L mercury | grep -iE 'hamlib|libusb'      # (empty — statically linked)

$ otool -L mercury
mercury:
	/System/Library/Frameworks/CoreFoundation.framework/…/CoreFoundation
	/System/Library/Frameworks/CoreAudio.framework/…/CoreAudio
	/usr/lib/libSystem.B.dylib
	/System/Library/Frameworks/IOKit.framework/…/IOKit
	/System/Library/Frameworks/Security.framework/…/Security

$ ./mercury -h | head -1
Rhizomatica Mercury Version 1.9.9 (git 5d587251)
```

### Go UI — universal, no Homebrew hamlib

Per-arch build (with a generated `hamlib.pc` shim) then lipo (mirrors the
`fyne-ui-macos-universal` target):

```console
$ lipo -archs mercury-ui
x86_64 arm64

$ otool -L mercury-ui | grep -iE 'hamlib|libusb'   # (empty — statically linked)

$ otool -L mercury-ui | grep -c /usr/local          # 0 (no Homebrew refs)
0
```

All remaining `otool -L` entries for `mercury-ui` are macOS **system**
frameworks (AppKit, Cocoa, OpenGL, CoreVideo, Foundation, IOKit, Security, …)
plus `libSystem`/`libobjc` — i.e. nothing outside the base OS.
