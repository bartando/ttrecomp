#include <filesystem>
#include <string>

#include <SDL3/SDL.h>

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "tabletennis_icon_png.h"

namespace tabletennis {

std::filesystem::path PickIsoFileMacOS() {
  @autoreleasepool {
    // The wizard runs inside the SDL window; bring it forward so the panel
    // doesn't open behind whatever the user had focused.
    NSWindow* ns_window = nil;
    if (SDL_Window* sdl_window = SDL_GetKeyboardFocus()) {
      SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
      ns_window = static_cast<NSWindow*>(
          SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
      SDL_RaiseWindow(sdl_window);
    }
    [NSApp activateIgnoringOtherApps:YES];
    if (ns_window) {
      [ns_window makeKeyAndOrderFront:nil];
    }

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Select your Table Tennis Xbox 360 ISO"];
    [panel setPrompt:@"Select ISO"];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setResolvesAliases:YES];
    if (UTType* iso_type = [UTType typeWithFilenameExtension:@"iso"]) {
      [panel setAllowedContentTypes:@[ iso_type ]];
    }

    std::filesystem::path result;
    if ([panel runModal] == NSModalResponseOK) {
      NSURL* url = [[panel URLs] firstObject];
      if (url && [url isFileURL]) {
        result = std::string([[url path] UTF8String]);
      }
    }
    return result;
  }
}

// Without an .app bundle the dock shows a generic exec icon, so set ours at
// runtime.
void SetDockIconMacOS() {
  @autoreleasepool {
    NSData* data = [NSData dataWithBytesNoCopy:const_cast<unsigned char*>(kTabletennisIconPng)
                                        length:sizeof(kTabletennisIconPng)
                                  freeWhenDone:NO];
    NSImage* image = [[NSImage alloc] initWithData:data];
    if (image) {
      [NSApp setApplicationIconImage:image];
    }
  }
}

}  // namespace tabletennis
