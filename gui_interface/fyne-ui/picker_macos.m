//go:build darwin && !ios

// The macOS open panel (NSOpenPanel) for picker_macos.go.  In the Mac App
// Store build the app is sandboxed, and only a file or folder the user picks
// in this panel is opened to it; Fyne's own dialog is drawn by the app and
// reaches nothing outside the container.

#import <Cocoa/Cocoa.h>
#include <stdint.h>
#include <string.h>

extern void mercuryPicked(uintptr_t h, char *path, void *bookmark, int len);

// Runs the panel on the main thread and reports the choice (path NULL:
// cancelled).  A folder comes with a security-scoped bookmark, so a
// sandboxed app can open it again after a restart.
void mercury_pick(int folder, uintptr_t h)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseFiles = !folder;
        p.canChooseDirectories = folder;
        p.canCreateDirectories = folder;
        p.allowsMultipleSelection = NO;
        NSURL *u = [p runModal] == NSModalResponseOK ? p.URL : nil;
        NSData *bm = nil;
        if (u && folder)
            bm = [u bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        mercuryPicked(h, u ? (char *)u.fileSystemRepresentation : NULL,
                      bm ? (void *)bm.bytes : NULL, bm ? (int)bm.length : 0);
    });
}

// Opens a folder from its bookmark for the life of the process.  The path
// (malloc'd), or NULL if the bookmark no longer resolves.
char *mercury_open_bookmark(void *b, int len)
{
    @autoreleasepool {
        NSData *bm = [NSData dataWithBytes:b length:(NSUInteger)len];
        BOOL stale = NO;
        NSURL *u = [NSURL URLByResolvingBookmarkData:bm
                                             options:NSURLBookmarkResolutionWithSecurityScope
                                       relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
        if (!u)
            return NULL;
        [u startAccessingSecurityScopedResource];
        return strdup(u.fileSystemRepresentation);
    }
}
