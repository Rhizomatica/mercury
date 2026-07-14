package main

// coreBuildID identifies the libmercury_core.a the binary was linked against.
//
// The Makefile sets it via -ldflags "-X main.coreBuildID=<hash of the archive>".
// Go's build cache does NOT track the contents of a static archive linked
// through #cgo LDFLAGS (-lmercury_core), so without this a warm-cache
// `go build` silently relinks against a STALE cached object whenever only the
// C core changed.  Feeding the archive's hash into the link command line (which
// IS part of the link-action cache key) forces a relink precisely when — and
// only when — the archive's contents change.
var coreBuildID = "dev"

// Keep the linker from eliminating coreBuildID so -X can resolve the symbol.
var _ = coreBuildID
