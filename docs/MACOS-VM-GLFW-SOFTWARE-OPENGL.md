# Running the Fyne UI on a GPU-less macOS VM (GLFW software-OpenGL fallback)

**Audience:** whoever is working on `gui_interface/fyne-ui` (Mercury's Fyne GUI).
**TL;DR:** The Fyne UI panics on launch inside a headless/virtualized macOS
(e.g. the QEMU/KVM dev VM we use to build & test the macOS target) with:

```
panic: FormatUnavailable: NSGL: Failed to find a suitable pixel format
```

This is **not** a Mercury bug and **not** a Fyne bug. It's GLFW refusing to
create an OpenGL context when there is no hardware-accelerated renderer. Below
is a small, portable patch to `go-gl/glfw` that adds a **software-renderer
fallback**, so the UI runs in the VM while keeping full hardware acceleration
everywhere a real GPU exists.

---

## Why it happens

- Mercury's GUI: `gui_interface/fyne-ui/` — `fyne.io/fyne/v2 v2.8.0`, which uses
  **`github.com/go-gl/glfw/v3.4/glfw`** (see `go.sum`).
- Fyne draws with OpenGL via GLFW. On macOS, GLFW builds its
  `NSOpenGLPixelFormatAttribute` list starting with `NSOpenGLPFAAccelerated`
  (`glfw/src/nsgl_context.m`, first attribute added). That attribute *demands* a
  hardware renderer.
- Our macOS dev VM (OSX-KVM / QEMU, macOS Sequoia) has **no accelerated GPU**
  (NVIDIA has no macOS driver on Sequoia; the Intel iGPU can't be passed
  through on the laptop host). So `initWithAttributes:` returns `nil` →
  GLFW reports `FORMAT_UNAVAILABLE` → the app panics before showing a window.
- macOS *does* ship a **software OpenGL renderer** (`kCGLRendererGenericFloatID`),
  but GLFW never asks for it and provides no option to
  ([glfw#2080](https://github.com/glfw/glfw/issues/2080)). There is **no env var**
  to force software GL on macOS (unlike Mesa's `LIBGL_ALWAYS_SOFTWARE`).

## The fix (one localized change, inherently portable)

GLFW always puts `NSOpenGLPFAAccelerated` at `attribs[0]`. So: if the accelerated
pixel format fails, **retry once from `attribs + 1`** (dropping only that one
attribute), which lets macOS return the software renderer.

Why this is safe on every platform:
- **Real GPU (any OS):** the accelerated request runs first and succeeds → the
  fallback branch is never reached → behavior is byte-for-byte unchanged, full
  hardware acceleration retained.
- **GPU-less macOS VM:** accelerated fails → fallback yields a software context →
  the UI runs (CPU-rendered, slower, but fully functional).
- **Linux / Windows:** `nsgl_context.m` is compiled **only on macOS**, so those
  builds are untouched.

### Patched region of `glfw/src/nsgl_context.m`

Change this:

```objc
    window->context.nsgl.pixelFormat =
        [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];
    if (window->context.nsgl.pixelFormat == nil)
    {
        _glfwInputError(GLFW_FORMAT_UNAVAILABLE,
                        "NSGL: Failed to find a suitable pixel format");
        return GLFW_FALSE;
    }
```

into this:

```objc
    window->context.nsgl.pixelFormat =
        [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];
    if (window->context.nsgl.pixelFormat == nil)
    {
        // Portable fallback for GPU-less environments (QEMU/KVM and other VM
        // guests with no accelerated renderer). attribs[0] is always
        // NSOpenGLPFAAccelerated (added at the top of this function); no pixel
        // format can satisfy it without hardware acceleration. Retry once from
        // attribs+1 so macOS returns its software (kCGLRendererGenericFloatID)
        // renderer instead of failing. Hardware stays strictly preferred: the
        // accelerated attempt runs first, so GPU machines never reach here.
        assert(attribs[0] == NSOpenGLPFAAccelerated);
        window->context.nsgl.pixelFormat =
            [[NSOpenGLPixelFormat alloc] initWithAttributes:(attribs + 1)];
    }
    if (window->context.nsgl.pixelFormat == nil)
    {
        _glfwInputError(GLFW_FORMAT_UNAVAILABLE,
                        "NSGL: Failed to find a suitable pixel format");
        return GLFW_FALSE;
    }
```

Verified against both `go-gl/glfw` **v3.4** (what Fyne 2.8.0 pulls) and v3.3 —
the anchor block is identical, and `NSOpenGLPFAAccelerated` is `attribs[0]` in
both.

## How to ship it in this repo (vendored module + `go.mod` replace)

We keep a patched copy of the glfw module **in-tree** so it travels with the
repo into the VM build. No external fork to maintain; nothing changes for
non-macOS builds.

Run from `gui_interface/fyne-ui/`:

```bash
MODPATH="github.com/go-gl/glfw/v3.4/glfw"
DEST="third_party/glfw-v3.4"

SRC="$(go list -m -f '{{.Dir}}' "$MODPATH")"     # resolved module in the cache
mkdir -p third_party && rm -rf "$DEST"
cp -r "$SRC/." "$DEST/" && chmod -R u+w "$DEST"

# apply the patch (idempotent literal replacement — see script below)
python3 apply-glfw-software-fallback.py "$DEST/glfw/src/nsgl_context.m"

# note: `go mod edit -replace` uses `=` (the `=>` form is the go.mod file syntax)
go mod edit -replace "${MODPATH}=./${DEST}"
go mod tidy
```

Then commit `third_party/`, `go.mod`, `go.sum`.

### `apply-glfw-software-fallback.py` (idempotent)

```python
import sys
p = sys.argv[1]
s = open(p).read()
anchor = """    window->context.nsgl.pixelFormat =
        [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];
    if (window->context.nsgl.pixelFormat == nil)
    {
        _glfwInputError(GLFW_FORMAT_UNAVAILABLE,
                        "NSGL: Failed to find a suitable pixel format");
        return GLFW_FALSE;
    }"""
fallback = """    window->context.nsgl.pixelFormat =
        [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];
    if (window->context.nsgl.pixelFormat == nil)
    {
        // Portable fallback for GPU-less environments (QEMU/KVM and other VM
        // guests with no accelerated renderer). attribs[0] is always
        // NSOpenGLPFAAccelerated (added at the top of this function); no pixel
        // format can satisfy it without hardware acceleration. Retry once from
        // attribs+1 so macOS returns its software (kCGLRendererGenericFloatID)
        // renderer instead of failing. Hardware stays strictly preferred: the
        // accelerated attempt runs first, so GPU machines never reach here.
        assert(attribs[0] == NSOpenGLPFAAccelerated);
        window->context.nsgl.pixelFormat =
            [[NSOpenGLPixelFormat alloc] initWithAttributes:(attribs + 1)];
    }
    if (window->context.nsgl.pixelFormat == nil)
    {
        _glfwInputError(GLFW_FORMAT_UNAVAILABLE,
                        "NSGL: Failed to find a suitable pixel format");
        return GLFW_FALSE;
    }"""
if "initWithAttributes:(attribs + 1)" in s:
    print("already patched (no-op)"); raise SystemExit(0)
if anchor not in s:
    print("ERROR: anchor not found; go-gl/glfw changed. Re-inspect nsgl_context.m."); raise SystemExit(1)
open(p, "w").write(s.replace(anchor, fallback, 1))
print("patched OK")
```

> Re-run the whole block after any `go get -u` that bumps the glfw version:
> it re-copies the (possibly new) module and re-applies the patch. If the glfw
> version changes (e.g. v3.4 → v3.5), update `MODPATH`/`DEST` and confirm the
> anchor still matches.

## Caveats / things to check on first run

1. **OpenGL version.** Fyne requests desktop **OpenGL 2.1** (legacy), which the
   macOS software renderer supports — so the fallback should get a valid
   context. If a future Fyne requests a **3.2+ core** profile, the software
   renderer may still refuse; then also force a 2.x context.
2. **Performance.** Software rendering is CPU-only → sluggish. Fine for
   functional/interactive testing of the macOS build; not for smooth animation.
   For fast GUI iteration, run the same Fyne code natively on Linux (real GPU).
3. **Scope.** This only matters when *running* the UI inside a GPU-less macOS
   VM. Building/packaging/signing the macOS app needs no GPU and works without
   this. On real Mac hardware the patch is inert (accelerated path wins).

## Alternative for CI / non-interactive checks

For headless UI verification (no window at all), use Fyne's **software test
canvas** — works in the VM with zero GL:

```go
import "fyne.io/fyne/v2/test"
w := test.NewWindow(content)
test.AssertImageMatches(t, "panel.png", w.Canvas().Capture())  // visual regression
```

Use the GLFW patch when you need the *real interactive window* in the VM; use
the software test canvas for automated snapshot/logic tests.

## Upstreaming

The same fix belongs in upstream GLFW (`glfw/glfw`), which `go-gl/glfw` vendors.
A ready-to-send patch (upstream paths, targets `src/nsgl_context.m` in
`_glfwCreateContextNSGL`) is kept alongside this doc:

- `gui_interface/fyne-ui/glfw-nsgl-software-fallback.patch`

If GLFW accepts it, `go-gl/glfw` inherits it on its next vendor sync and this
repo's `third_party/` copy + `replace` can be dropped.

## References
- go-gl/glfw source: `glfw/src/nsgl_context.m` (`createContextNSGL`)
- [glfw#2080 — allow macOS OpenGL software renderer](https://github.com/glfw/glfw/issues/2080)
- [go-gl/glfw#335 — NSGL pixel format panic](https://github.com/go-gl/glfw/issues/335)
- [fyne-io/fyne#2373 — the panic we hit](https://github.com/fyne-io/fyne/issues/2373)
- Context: macOS dev VM built with OSX-KVM (QEMU/KVM, macOS Sequoia); see the
  Simple-KVM/OSX-KVM setup notes.
