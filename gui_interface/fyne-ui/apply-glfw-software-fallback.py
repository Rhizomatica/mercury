#!/usr/bin/env python3
"""Patch go-gl/glfw's macOS NSGL context creation to fall back to the software
OpenGL renderer when no accelerated pixel format is available (e.g. inside a
GPU-less QEMU/KVM macOS VM). Idempotent. macOS-only file; no effect on
Linux/Windows or on machines with a real GPU.

Usage:
    python3 apply-glfw-software-fallback.py <path-to>/glfw/src/nsgl_context.m

See docs/MACOS-VM-GLFW-SOFTWARE-OPENGL.md for the full rationale and the
vendoring + `go.mod replace` steps.
"""
import sys

if len(sys.argv) != 2:
    print("usage: apply-glfw-software-fallback.py <nsgl_context.m>")
    raise SystemExit(2)

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
    print("already patched (no-op)")
    raise SystemExit(0)
if anchor not in s:
    print("ERROR: anchor not found; go-gl/glfw changed. Re-inspect nsgl_context.m.")
    raise SystemExit(1)

open(p, "w").write(s.replace(anchor, fallback, 1))
print("patched OK:", p)
