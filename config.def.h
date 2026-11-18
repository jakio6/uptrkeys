// Updates per second while moving or scrolling.
#define FPS 120

// Pointer movement speed in pixels per second.
#define BASE_SPEED 1000.0

// Scroll speed in notches per second (smooth hi-res units are 120x this rate).
#define BASE_SCROLL 14.0

// Modifiers listed here are suppressed from applications ONLY for modifier keys
// that are also bound in `bindings[]` below (for example KEY_LEFTCTRL and
// KEY_LEFTALT for speed scaling), or when used as part of a global scroll chord
// such as MOD_SHIFT + KEY_CAPSLOCK.
//
// All other modifiers — including MOD_SUPER (Super_L/Super_R), unbound
// Shift keys (KEY_LEFTSHIFT/KEY_RIGHTSHIFT since Tab is used for scrolling),
// and right-side modifiers (KEY_RIGHTCTRL, KEY_RIGHTALT) — pass straight
// through to the virtual keyboard even while pointer mode is active, so
// Mod + Click / Drag / Scroll combinations (Super+Click, Shift+Click,
// Ctrl+Click, etc.) work identically to a physical mouse.
static const unsigned int internal_mods = MOD_SHIFT | MOD_CTRL | MOD_ALT;

// Keybindings using native Linux <linux/input-event-codes.h> constants.
//
// Physical CapsLock acts directly as the hold-to-activate prefix layer key:
//  - Hold CapsLock to enter pointer mode; release CapsLock to exit.
//  - BIND_ANY_MOD allows pressing Super/Ctrl/Alt either BEFORE or DURING
//    holding CapsLock so Mod + Mouse combinations work seamlessly without
//    stealing any tiling WM shortcuts.
//  - Shift + CapsLock enters sticky scroll mode until `q` is pressed.
static const Binding bindings[] = {
// mod         keycode        opts                                      press_fn                   press_arg          release_fn          release_arg
// Enable / disable
// {MOD_SUPER, KEY_W,         BIND_GLOBAL,                              cmd_activate,              {.u = KEY_W},      NULL,               {0}},
{MOD_NONE,     KEY_Q,         BIND_NONE,                                cmd_deactivate,            {0},               NULL,               {0}},
{MOD_NONE,     KEY_CAPSLOCK,  BIND_GLOBAL | BIND_NOREPEAT | BIND_ANY_MOD, cmd_activate,            {0},               cmd_deactivate,     {0}},
{MOD_SHIFT,    KEY_CAPSLOCK,  BIND_GLOBAL | BIND_NOREPEAT,              cmd_activate_scroll,       {0},               NULL,               {0}},
// {MOD_SUPER, KEY_V,         BIND_GLOBAL,                              cmd_toggle_active,         {0},               NULL,               {0}},
{MOD_NONE,     KEY_X,         BIND_NONE,                                cmd_quit,                  {0},               NULL,               {0}},

// Directional movement (WASD)
{MOD_NONE,     KEY_W,         BIND_NONE,                                cmd_move_start,            {.u = DIR_UP},     cmd_move_stop,      {.u = DIR_UP}},
{MOD_NONE,     KEY_A,         BIND_NONE,                                cmd_move_start,            {.u = DIR_LEFT},   cmd_move_stop,      {.u = DIR_LEFT}},
{MOD_NONE,     KEY_S,         BIND_NONE,                                cmd_move_start,            {.u = DIR_DOWN},   cmd_move_stop,      {.u = DIR_DOWN}},
{MOD_NONE,     KEY_D,         BIND_NONE,                                cmd_move_start,            {.u = DIR_RIGHT},  cmd_move_stop,      {.u = DIR_RIGHT}},

// Scrolling (Tab holds scroll mode; f toggles scroll mode; Left/Right Shift stay free for Shift+Click/Scroll)
{MOD_NONE,     KEY_TAB,       BIND_NONE,                                cmd_move_to_scroll,        {.i = 1},          cmd_move_to_scroll, {.i = 0}},
{MOD_NONE,     KEY_F,         BIND_NONE,                                cmd_toggle_move_to_scroll, {0},               NULL,               {0}},

// Speed multipliers
{MOD_NONE,     KEY_LEFTALT,   BIND_NONE,                                cmd_divide_speed,          {.f = 8.0},        cmd_multiply_speed, {.f = 8.0}},
{MOD_NONE,     KEY_LEFTCTRL,  BIND_NONE,                                cmd_multiply_speed,        {.f = 32.0},       cmd_divide_speed,   {.f = 32.0}},
{MOD_NONE,     KEY_J,         BIND_NONE,                                cmd_divide_speed,          {.f = 8.0},        cmd_multiply_speed, {.f = 8.0}},
{MOD_NONE,     KEY_K,         BIND_NONE,                                cmd_divide_speed,          {.f = 2.0},        cmd_multiply_speed, {.f = 2.0}},
{MOD_NONE,     KEY_L,         BIND_NONE,                                cmd_multiply_speed,        {.f = 4.0},        cmd_divide_speed,   {.f = 4.0}},
{MOD_NONE,     KEY_SEMICOLON, BIND_NONE,                                cmd_multiply_speed,        {.f = 8.0},        cmd_divide_speed,   {.f = 8.0}},

// Clicking (left-handed: space/e/r; right-handed for dragging with WASD: n/m)
{MOD_NONE,     KEY_SPACE,     BIND_NONE,                                cmd_click_press,           {.u = BTN_LEFT},   cmd_click_release,  {.u = BTN_LEFT}},
{MOD_NONE,     KEY_E,         BIND_NONE,                                cmd_click_press,           {.u = BTN_RIGHT},  cmd_click_release,  {.u = BTN_RIGHT}},
{MOD_NONE,     KEY_R,         BIND_NONE,                                cmd_click_press,           {.u = BTN_MIDDLE}, cmd_click_release,  {.u = BTN_MIDDLE}},
{MOD_NONE,     KEY_N,         BIND_NONE,                                cmd_click_press,           {.u = BTN_RIGHT},  cmd_click_release,  {.u = BTN_RIGHT}},
{MOD_NONE,     KEY_M,         BIND_NONE,                                cmd_click_press,           {.u = BTN_MIDDLE}, cmd_click_release,  {.u = BTN_MIDDLE}},

// Debugging
// {MOD_SUPER, KEY_G,         BIND_GLOBAL,                              cmd_reset,                 {0},               NULL,               {0}},
};
