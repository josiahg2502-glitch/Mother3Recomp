#include "host_window.h"
#include "recomp_runtime_ui.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cassert>
#include <cstdio>

static void key(SDL_Scancode scan) {
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = scan;
    e.key.keysym.sym = SDL_GetKeyFromScancode(scan);
    assert(SDL_PushEvent(&e) == 1);
}
int main() {
    // Isolate virtual input without consuming a player's connected controller.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint("SDL_XINPUT_ENABLED", "0");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "0");
    SDL_SetHint("SDL_JOYSTICK_RAWINPUT", "0");
    SDL_SetHint("SDL_JOYSTICK_WGI", "0");
    SDL_SetHint("SDL_JOYSTICK_DIRECTINPUT", "0");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0);
    int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
    assert(device >= 0);
    auto* joystick = SDL_JoystickOpen(device);
    assert(joystick);
    gbarecomp::HostWindow window;
    assert(window.open(3, 240, 160, "menu regression"));
    if (auto* native_window = SDL_GetWindowFromID(1)) SDL_HideWindow(native_window);
    window.set_menu_scancode(SDL_SCANCODE_F11);
    RecompRuntimeUiStandardConfig config{};
    config.menu.title = "Menu regression";
    config.features = RECOMP_RUNTIME_UI_STANDARD_RESUME;
    auto* ui = recomp_runtime_ui_create_standard(&config);
    assert(ui);
    window.set_runtime_ui(ui);
    static unsigned char pixels[240 * 160 * 3]{};
    for (int i = 0; i < 40; ++i) {
        key(SDL_SCANCODE_F11);
        assert(!window.pump().quit);
        assert(recomp_runtime_ui_is_open(ui));
        window.present(pixels);
        key(SDL_SCANCODE_F11);
        assert(!window.pump().quit);
        assert(!recomp_runtime_ui_is_open(ui));
    }
    key(SDL_SCANCODE_ESCAPE);
    assert(!window.pump().quit);
    recomp_runtime_ui_open(ui);
    key(SDL_SCANCODE_ESCAPE);
    assert(!window.pump().quit);
    recomp_runtime_ui_close(ui);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    window.pump();
    assert(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, -25000) == 0);
    SDL_JoystickUpdate();
    assert((window.pump().keyinput & (1 << 5)) == 0);
    SDL_JoystickClose(joystick);
    SDL_JoystickDetachVirtual(device);
#endif
    window.set_runtime_ui(nullptr);
    recomp_runtime_ui_destroy(ui);
    window.close();
    puts("F11 menu, Escape does not quit, left analog movement: PASS");
}
