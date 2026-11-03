// SDL3 input for the Linux build: platform-neutral IDs from SDL scancodes and SDL gamepads,
// supporting generic gamepads (DualSense, Xbox, Switch Pro, 8BitDo, etc.) with deadzones,
// button mapping, triggers, rumble, and 6-axis gyro/accelerometer motion.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform.h"
#include "ui/menu.h"

__attribute__((weak)) int menu_is_open(void) { return 0; }
__attribute__((weak)) void menu_toggle(void) {}

enum {
    STICK_MAX = 32767,
    STICK_DEAD = 2500,
    STICK_DIR = 16384,
    TRIGGER_ON = 16384,
    RESCAN_NS = 1000000000ull   // 1s
};

static PlatMutex lock = PLAT_MUTEX_INIT;
static uint8_t key_down[KEY_COUNT];
static uint8_t pad_down[PAD_COUNT];
static int32_t axis_vals[6];                  // lx, ly, rx, ry, lt, rt
static SDL_Gamepad *pad;
static uint64_t next_scan_ns;
static int pad_rumble;
static int pad_labels[4];                     // PAD_SOUTH..PAD_NORTH labels
static int pad_east_a;
static int pad_layout = PAD_LAYOUT_NINTENDO;
static SDL_JoystickID sensor_pad;
static int sensor_ok;
static pthread_t motion_tid;
static volatile int motion_started;
static volatile int g_blocked, g_menu;

static const SDL_GamepadButton pad_sdl[PAD_COUNT] = {
    [PAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH,
    [PAD_EAST] = SDL_GAMEPAD_BUTTON_EAST,
    [PAD_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [PAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH,
    [PAD_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [PAD_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    [PAD_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [PAD_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    [PAD_START] = SDL_GAMEPAD_BUTTON_START,
    [PAD_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [PAD_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    [PAD_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [PAD_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    [PAD_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
};

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

static int32_t axis(int16_t v) {
    int32_t a = v < -STICK_MAX ? -STICK_MAX : v;
    return (a > -STICK_DEAD && a < STICK_DEAD) ? 0 : a;
}

static void stick_bits(uint64_t *b, int32_t x, int32_t y, int first) {
    if (x < -STICK_DIR) *b |= 1ull << (first + 0);
    if (y > STICK_DIR) *b |= 1ull << (first + 1);
    if (x > STICK_DIR) *b |= 1ull << (first + 2);
    if (y < -STICK_DIR) *b |= 1ull << (first + 3);
}

static int32_t add32(int32_t a, int32_t b) {
    int32_t v = a + b;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static void scan_pads(void) {
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            rt_log("input: SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: %s", SDL_GetError());
            return;
        }
    }
    if (pad && SDL_GamepadConnected(pad)) return;
    if (pad) {
        SDL_CloseGamepad(pad);
        pad = NULL;
        sensor_pad = 0;
        sensor_ok = 0;
        for (int i = 0; i < 4; i++) pad_labels[i] = PAD_LABEL_NONE;
        rt_log("input: gamepad disconnected");
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && !pad; i++) {
        pad = SDL_OpenGamepad(ids[i]);
    }
    SDL_free(ids);
    if (!pad) return;

    pad_rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    pad_east_a = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
    for (int i = 0; i < 4; i++) {
        SDL_GamepadButtonLabel lb = SDL_GetGamepadButtonLabel(pad, pad_sdl[PAD_SOUTH + i]);
        pad_labels[i] = (lb == SDL_GAMEPAD_BUTTON_LABEL_A          ? PAD_LABEL_A
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_B        ? PAD_LABEL_B
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_X        ? PAD_LABEL_X
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_Y        ? PAD_LABEL_Y
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_CROSS    ? PAD_LABEL_CROSS
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE   ? PAD_LABEL_CIRCLE
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_SQUARE   ? PAD_LABEL_SQUARE
                       : lb == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ? PAD_LABEL_TRIANGLE
                                                                 : PAD_LABEL_NONE);
    }
    const char *set = cfg_value("TOTK_PAD_LAYOUT");
    pad_layout = input_pad_layout_pick(set, pad_east_a);
    int motion = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    rt_log("input: gamepad \"%s\" (%s, %s, %s layout, east button %s)",
           SDL_GetGamepadName(pad),
           pad_rumble ? "rumble" : "no rumble",
           motion ? "gyro and accelerometer" : "no motion sensors",
           pad_layout == PAD_LAYOUT_NINTENDO ? "nintendo" : "xbox",
           pad_east_a ? "labelled A" : "not labelled A");
}

void input_linux_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        plat_mutex_lock(&lock);
        scan_pads();
        plat_mutex_unlock(&lock);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        plat_mutex_lock(&lock);
        if (pad && e->gdevice.which == SDL_GetGamepadID(pad)) {
            SDL_CloseGamepad(pad);
            pad = NULL;
            sensor_pad = 0;
            sensor_ok = 0;
            rt_log("input: gamepad removed");
        }
        plat_mutex_unlock(&lock);
        break;
    default: break;
    }
}

void input_linux_pump(void) {
    const bool *ks = SDL_GetKeyboardState(NULL);
    memset(key_down, 0, sizeof key_down);
    if (ks) {
        for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
            if (!ks[i]) continue;
            int k = key_of_scancode((SDL_Scancode)i);
            if (k > 0 && k < KEY_COUNT) key_down[k] = 1;
        }
    }
    float mx, my;
    Uint32 mbtn = SDL_GetMouseState(&mx, &my);
    (void)mx;
    (void)my;
    if (mbtn & SDL_BUTTON_LMASK) key_down[KEY_MOUSE_L] = 1;
    if (mbtn & SDL_BUTTON_RMASK) key_down[KEY_MOUSE_R] = 1;
    if (mbtn & SDL_BUTTON_MMASK) key_down[KEY_MOUSE_M] = 1;

    memset(pad_down, 0, sizeof pad_down);
    memset(axis_vals, 0, sizeof axis_vals);
    if (!pad) {
        uint64_t now = plat_now_ns();
        if (now >= next_scan_ns) {
            next_scan_ns = now + RESCAN_NS;
            scan_pads();
        }
    }
    if (!pad || !SDL_GamepadConnected(pad)) return;

    for (int i = 0; i < PAD_COUNT; i++) {
        if (!pad_sdl[i]) continue;
        if (SDL_GetGamepadButton(pad, pad_sdl[i])) pad_down[i] = 1;
    }
    int lt = (int)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    int rt = (int)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (lt >= TRIGGER_ON) pad_down[PAD_ZL] = 1;
    if (rt >= TRIGGER_ON) pad_down[PAD_ZR] = 1;

    axis_vals[0] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    axis_vals[1] = -axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));   // Invert Y: up is +y in Switch HID
    axis_vals[2] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    axis_vals[3] = -axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));  // Invert Y: up is +y in Switch HID
    axis_vals[4] = lt;
    axis_vals[5] = rt;
}

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
    plat_mutex_lock(&lock);
    input_linux_pump();
    uint64_t act = input_map_buttons(key_down);
    if (pad) act |= input_map_pad_buttons(pad_down, pad_layout);
    int32_t kl[2] = {0, 0}, kr[2] = {0, 0};
    input_map_stick(key_down, 0, kl);
    input_map_stick(key_down, 1, kr);
    int32_t sl[2] = {add32(kl[0], axis_vals[0]), add32(kl[1], axis_vals[1])};
    int32_t sr[2] = {add32(kr[0], axis_vals[2]), add32(kr[1], axis_vals[3])};

    static int combo_held;
    int32_t st[4] = {sl[0], sl[1], sr[0], sr[1]};
    int toggle = input_menu_gate(&combo_held, menu_is_open(), &act, st);
    sl[0] = st[0], sl[1] = st[1], sr[0] = st[2], sr[1] = st[3];
    plat_mutex_unlock(&lock);

    if (toggle) menu_toggle();

    stick_bits(&act, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&act, sr[0], sr[1], B_SR_LEFT);

    int hidden = g_blocked || g_menu;
    if (buttons) *buttons = hidden ? 0 : act;
    if (lx) *lx = hidden ? 0 : sl[0];
    if (ly) *ly = hidden ? 0 : sl[1];
    if (rx) *rx = hidden ? 0 : sr[0];
    if (ry) *ry = hidden ? 0 : sr[1];
    if (!hidden) input_log(act, lx ? *lx : 0, ly ? *ly : 0);
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    input_poll(buttons, lx, ly, rx, ry);
}

void input_block(int on) { g_blocked = on; }
void input_menu(int open) { g_menu = open; }

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    plat_mutex_lock(&lock);
    input_linux_pump();
    if (!pad) {
        plat_mutex_unlock(&lock);
        if (buttons) *buttons = 0;
        if (lx) *lx = 0;
        if (ly) *ly = 0;
        return 0;
    }
    uint64_t b = input_map_pad_buttons(pad_down, pad_layout);
    int32_t x = axis_vals[0];
    int32_t y = axis_vals[1];
    plat_mutex_unlock(&lock);
    if (buttons) *buttons = b;
    if (lx) *lx = x;
    if (ly) *ly = y;
    return 1;
}

int input_pad_layout(void) {
    plat_mutex_lock(&lock);
    int l = pad ? pad_layout : -1;
    plat_mutex_unlock(&lock);
    return l;
}

int input_pad_label(int pad_id) {
    if (pad_id < PAD_SOUTH || pad_id > PAD_NORTH) return PAD_LABEL_NONE;
    plat_mutex_lock(&lock);
    int lb = pad ? pad_labels[pad_id - PAD_SOUTH] : PAD_LABEL_NONE;
    plat_mutex_unlock(&lock);
    return lb;
}

int input_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    plat_mutex_lock(&lock);
    int ok = 0;
    if (pad && pad_rumble) {
        ok = SDL_RumbleGamepad(pad, low, high, ms ? ms : 120) ? 1 : 0;
    }
    plat_mutex_unlock(&lock);
    return ok;
}

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

int input_rumble_triggers_enabled(void) {
    return cfg_bool("TOTK_RUMBLE_TRIGGERS");
}

void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

int input_motion_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("TOTK_MOTION");
    }
    return on;
}

static int sensors_ready(void) {
    if (!pad) return 0;
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id == sensor_pad) return sensor_ok;
    sensor_pad = id;
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    sensor_ok = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    if (sensor_ok) {
        rt_log("input: motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)",
               (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO),
               (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    } else {
        rt_log("input: \"%s\" has no gyro and accelerometer: six-axis reads at rest",
               SDL_GetGamepadName(pad));
    }
    return sensor_ok;
}

static void *motion_thread_func(void *arg) {
    (void)arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 4000000};   // 4 ms = 250 Hz
    while (motion_started) {
        nanosleep(&ts, NULL);
        float a[3] = {0}, g[3] = {0};
        int have = 0;
        plat_mutex_lock(&lock);
        if (pad && SDL_GamepadConnected(pad)) {
            SDL_UpdateGamepads();
            have = sensors_ready() &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        }
        plat_mutex_unlock(&lock);
        if (!have || !input_motion_enabled()) continue;
        double acc[3], gyro[3];
        input_motion_axes(a, g, acc, gyro);
        motion_feed(motion_global(), acc, gyro, kern_now_ns());
    }
    return NULL;
}

void input_motion_start(void) {
    plat_mutex_lock(&lock);
    if (!motion_started) {
        motion_started = 1;
        if (!input_motion_enabled())
            rt_log("input: motion off (TOTK_MOTION=0); it can be turned on while the game runs");
        pthread_create(&motion_tid, NULL, motion_thread_func, NULL);
    }
    plat_mutex_unlock(&lock);
}

void input_map_snapshot(InputMap *m) {
    plat_mutex_lock(&lock);
    input_map_configure();
    *m = input_map;
    plat_mutex_unlock(&lock);
}

int input_capture(int reset, int *id) {
    static uint8_t prev_k[KEY_COUNT], prev_p[PAD_COUNT];
    uint8_t k[KEY_COUNT] = {0}, p[PAD_COUNT] = {0};
    plat_mutex_lock(&lock);
    memcpy(k, key_down, sizeof k);
    memcpy(p, pad_down, sizeof p);
    plat_mutex_unlock(&lock);
    int got = INPUT_CAPTURE_NONE;
    if (!reset) {
        if (k[KEY_ENTER] && !prev_k[KEY_ENTER]) got = INPUT_CAPTURE_CANCEL;
        for (int i = 1; i < KEY_COUNT && !got; i++)
            if (k[i] && !prev_k[i]) got = INPUT_CAPTURE_KEY, *id = i;
        for (int i = 1; i < PAD_COUNT && !got; i++)
            if (p[i] && !prev_p[i]) got = INPUT_CAPTURE_PAD, *id = i;
    }
    memcpy(prev_k, k, sizeof k);
    memcpy(prev_p, p, sizeof p);
    return got;
}

uint32_t input_key_char(int key) {
    switch (key) {
    case KEY_MINUS: return '-';
    case KEY_EQUALS: return '=';
    case KEY_LBRACKET: return '[';
    case KEY_RBRACKET: return ']';
    case KEY_SEMICOLON: return ';';
    case KEY_APOSTROPHE: return '\'';
    case KEY_COMMA: return ',';
    case KEY_PERIOD: return '.';
    case KEY_SLASH: return '/';
    case KEY_BACKSLASH: return '\\';
    case KEY_GRAVE: return '`';
    default: return 0;
    }
}

void input_cursor_suspend(int on) { (void)on; }
