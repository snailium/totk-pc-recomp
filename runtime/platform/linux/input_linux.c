// SDL3 input for the Linux build: the platform-neutral ids of input_map.h from SDL scancodes and SDL gamepads, the same
// way input_win.c translates Win32 keys and SDL buttons on Windows. One place, so the bindings, the menu and the guest's
// hid layer see identical action bits on both platforms.
//
// The window pumps events (gfx_window_linux.c gfx_pump_events); every event goes through input_linux_event, and
// input_linux_pump refreshes the keyboard state once per frame. Gamepads are opened when SDL reports one, and rumble
// goes back through SDL.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform.h"

static uint8_t key_down[KEY_COUNT];
static uint8_t pad_down[PAD_COUNT];
static int32_t axis[6];                       // lx, ly, rx, ry, left trigger, right trigger
static SDL_Gamepad *pad;
static int layout = PAD_LAYOUT_NINTENDO;
static volatile int g_blocked, g_menu;

// The key ids are the bindings' vocabulary; every row of the default table names one of these.
static int key_of_scancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return KEY_A + (int)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return KEY_1 + (int)(sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_KP_0 && sc <= SDL_SCANCODE_KP_9) return KEY_NUM0 + (int)(sc - SDL_SCANCODE_KP_0);
    switch (sc) {
    case SDL_SCANCODE_0: return KEY_0;
    case SDL_SCANCODE_SPACE: return KEY_SPACE;
    case SDL_SCANCODE_TAB: return KEY_TAB;
    case SDL_SCANCODE_LSHIFT: return KEY_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
    case SDL_SCANCODE_LCTRL: return KEY_LCTRL;
    case SDL_SCANCODE_RCTRL: return KEY_RCTRL;
    case SDL_SCANCODE_LALT: return KEY_LALT;
    case SDL_SCANCODE_CAPSLOCK: return KEY_CAPSLOCK;
    case SDL_SCANCODE_UP: return KEY_UP;
    case SDL_SCANCODE_DOWN: return KEY_DOWN;
    case SDL_SCANCODE_LEFT: return KEY_LEFT;
    case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
    case SDL_SCANCODE_RETURN: return KEY_ENTER;
    case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
    case SDL_SCANCODE_INSERT: return KEY_INSERT;
    case SDL_SCANCODE_DELETE: return KEY_DELETE;
    case SDL_SCANCODE_HOME: return KEY_HOME;
    case SDL_SCANCODE_END: return KEY_END;
    case SDL_SCANCODE_PAGEUP: return KEY_PAGEUP;
    case SDL_SCANCODE_PAGEDOWN: return KEY_PAGEDOWN;
    case SDL_SCANCODE_F2: return KEY_F2;
    case SDL_SCANCODE_F3: return KEY_F3;
    case SDL_SCANCODE_F4: return KEY_F4;
    case SDL_SCANCODE_F5: return KEY_F5;
    case SDL_SCANCODE_F6: return KEY_F6;
    case SDL_SCANCODE_F7: return KEY_F7;
    case SDL_SCANCODE_F9: return KEY_F9;
    case SDL_SCANCODE_F12: return KEY_F12;
    case SDL_SCANCODE_MINUS: return KEY_MINUS;
    case SDL_SCANCODE_EQUALS: return KEY_EQUALS;
    case SDL_SCANCODE_LEFTBRACKET: return KEY_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET: return KEY_RBRACKET;
    case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
    case SDL_SCANCODE_COMMA: return KEY_COMMA;
    case SDL_SCANCODE_PERIOD: return KEY_PERIOD;
    case SDL_SCANCODE_SLASH: return KEY_SLASH;
    case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
    case SDL_SCANCODE_GRAVE: return KEY_GRAVE;
    case SDL_SCANCODE_KP_PLUS: return KEY_NUM_ADD;
    case SDL_SCANCODE_KP_MINUS: return KEY_NUM_SUB;
    case SDL_SCANCODE_KP_MULTIPLY: return KEY_NUM_MUL;
    case SDL_SCANCODE_KP_DIVIDE: return KEY_NUM_DIV;
    case SDL_SCANCODE_KP_PERIOD: return KEY_NUM_DECIMAL;
    default: return KEY_NONE;
    }
}

static int pad_of_button(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return PAD_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return PAD_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return PAD_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return PAD_NORTH;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return PAD_LSTICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return PAD_RSTICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return PAD_L;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PAD_R;
    case SDL_GAMEPAD_BUTTON_START: return PAD_START;
    case SDL_GAMEPAD_BUTTON_BACK: return PAD_BACK;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return PAD_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return PAD_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return PAD_DPAD_RIGHT;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return PAD_DPAD_DOWN;
    default: return PAD_NONE;
    }
}

// The stick range the guest expects is the passed-through range of libnx: -32767..32767, y down.
static int32_t axis_of(SDL_GamepadAxis a) {
    int32_t v = (int32_t)SDL_GetGamepadAxis(pad, a);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return v;
}

static void pad_open(void) {
    if (pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) return;
    if (n > 0) pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (pad) {
        const char *name = SDL_GetGamepadName(pad);
        int east_a = SDL_GetGamepadButtonFromString("a") == SDL_GAMEPAD_BUTTON_EAST;
        layout = input_pad_layout_pick(NULL, east_a);
        rt_log("input: gamepad %s opened (%s layout)", name ? name : "?", layout == PAD_LAYOUT_NINTENDO ? "nintendo" : "xbox");
    }
}

void input_linux_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMAPPED: pad_open(); break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && e->gdevice.which == SDL_GetGamepadID(pad)) {
            SDL_CloseGamepad(pad);
            pad = NULL;
            rt_log("input: gamepad removed");
        }
        break;
    default: break;
    }
}

// Once per frame: the keyboard and mouse state, and the gamepad's.
void input_linux_pump(void) {
    const bool *ks = SDL_GetKeyboardState(NULL);
    memset(key_down, 0, sizeof key_down);
    if (ks)
        for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
            if (!ks[i]) continue;
            int k = key_of_scancode((SDL_Scancode)i);
            if (k > 0 && k < KEY_COUNT) key_down[k] = 1;
        }
    float mx, my;
    Uint32 mbtn = SDL_GetMouseState(&mx, &my);
    (void)mx;
    (void)my;
    if (mbtn & SDL_BUTTON_LMASK) key_down[KEY_MOUSE_L] = 1;
    if (mbtn & SDL_BUTTON_RMASK) key_down[KEY_MOUSE_R] = 1;
    if (mbtn & SDL_BUTTON_MMASK) key_down[KEY_MOUSE_M] = 1;

    memset(pad_down, 0, sizeof pad_down);
    memset(axis, 0, sizeof axis);
    if (!pad) pad_open();
    if (!pad) return;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (!SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) continue;
        int p = pad_of_button(b);
        if (p) pad_down[p] = 1;
    }
    int lt = (int)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    int rt = (int)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (lt > 8000) pad_down[PAD_ZL] = 1;
    if (rt > 8000) pad_down[PAD_ZR] = 1;
    axis[0] = axis_of(SDL_GAMEPAD_AXIS_LEFTX);
    axis[1] = axis_of(SDL_GAMEPAD_AXIS_LEFTY);
    axis[2] = axis_of(SDL_GAMEPAD_AXIS_RIGHTX);
    axis[3] = axis_of(SDL_GAMEPAD_AXIS_RIGHTY);
    axis[4] = lt;
    axis[5] = rt;
}

static int32_t add32(int32_t a, int32_t b) {
    int32_t v = a + b;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

// TOTK_INPUT_LOG=1: every change of the action bits, so a run can prove that a key press reaches the bindings.
static void input_log(uint64_t act, int32_t lx, int32_t ly) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("TOTK_INPUT_LOG");
        on = v && *v == '1';
    }
    if (!on) return;
    static uint64_t last;
    static int32_t llx, lly;
    if (act == last && lx == llx && ly == lly) return;
    last = act, llx = lx, lly = ly;
    rt_log("input: actions %#llx, left stick %d/%d", (unsigned long long)act, lx, ly);
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    input_linux_pump();
    uint64_t act = input_map_buttons(key_down);
    if (pad) act |= input_map_pad_buttons(pad_down, layout);
    int32_t kl[2] = {0, 0}, kr[2] = {0, 0};
    input_map_stick(key_down, 0, kl);
    input_map_stick(key_down, 1, kr);
    int hidden = g_blocked || g_menu;   // a text field or the menu owns the gamepad while it is up
    if (buttons) *buttons = hidden ? 0 : act;
    if (lx) *lx = hidden ? 0 : add32(kl[0], axis[0]);
    if (ly) *ly = hidden ? 0 : add32(kl[1], axis[1]);
    if (rx) *rx = hidden ? 0 : add32(kr[0], axis[2]);
    if (ry) *ry = hidden ? 0 : add32(kr[1], axis[3]);
    if (!hidden) input_log(act, lx ? *lx : 0, ly ? *ly : 0);
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    input_poll(buttons, lx, ly, rx, ry);
}

void input_block(int on) { g_blocked = on; }
void input_menu(int open) { g_menu = open; }

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    input_linux_pump();
    if (!pad) {
        if (buttons) *buttons = 0;
        if (lx) *lx = 0;
        if (ly) *ly = 0;
        return 0;
    }
    uint64_t b = input_map_pad_buttons(pad_down, layout);
    if (buttons) *buttons = b;
    if (lx) *lx = axis[0];
    if (ly) *ly = axis[1];
    return 1;
}

int input_pad_layout(void) { return pad ? layout : -1; }

int input_pad_label(int pad_id) {
    if (!pad) return PAD_LABEL_NONE;
    switch (pad_id) {
    case PAD_SOUTH: return layout == PAD_LAYOUT_NINTENDO ? PAD_LABEL_B : PAD_LABEL_A;
    case PAD_EAST: return layout == PAD_LAYOUT_NINTENDO ? PAD_LABEL_A : PAD_LABEL_B;
    case PAD_WEST: return PAD_LABEL_Y;
    case PAD_NORTH: return PAD_LABEL_X;
    default: return PAD_LABEL_NONE;
    }
}

int input_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    if (!pad) return 0;
    (void)ms;
    return SDL_RumbleGamepad(pad, low, high, 120) ? 1 : 0;
}

// The motion and rumble helpers input.h declares. SDL's sensors are not wired to the guest yet, so the motion path
// reports nothing rather than inventing values; the guest asks for its six-axis sensor once at boot (see the hid log).
uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

int input_rumble_triggers_enabled(void) { return 0; }
void input_motion_start(void) {}
int input_motion_enabled(void) { return 0; }
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]) {
    (void)sdl_acc;
    (void)sdl_gyro;
    memset(acc_g, 0, sizeof(double) * 3);
    memset(gyro, 0, sizeof(double) * 3);
}
