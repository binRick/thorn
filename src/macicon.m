// macOS-only: set the Dock / Cmd-Tab application icon at runtime.
// raylib uses GLFW, whose window-icon API (glfwSetWindowIcon) is a no-op on
// macOS, so the app-switcher icon must be set through Cocoa instead. Compiled
// into the binary only on Darwin (see the Makefile); never built for web.
#import <AppKit/AppKit.h>

void SetMacDockIcon(const char *path){
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:path];
        NSImage *img = [[NSImage alloc] initWithContentsOfFile:p];
        if (img != nil) {
            [NSApp setApplicationIconImage:img];
        }
    }
}
