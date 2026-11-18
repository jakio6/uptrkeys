// Updates per second while moving or scrolling.
#define FPS 120

// Pointer movement speed in pixels per second.
#define BASE_SPEED 1000.0

// Scroll speed in notches per second (smooth hi-res units are 120x this rate).
#define BASE_SCROLL 14.0

// Hold this key to enter pointer mode; release to return to normal typing.
#define PREFIX_KEY KEY_CAPSLOCK

// Pointer-mode keybindings using <linux/input-event-codes.h> constants.
// Bound modifiers (LeftCtrl, LeftAlt) are consumed while active; unbound
// modifiers (Shift, Super, RightCtrl, RightAlt) pass straight through so
// Mod + Click / Drag / Scroll work like a physical mouse.
static const Binding bindings[] = {
// keycode        press_fn            press_arg          release_fn          release_arg
// Quit
{KEY_X,           cmd_quit,           {0},               NULL,               {0}},

// Directional movement (WASD)
{KEY_W,           cmd_move_start,     {.u = DIR_UP},     cmd_move_stop,      {.u = DIR_UP}},
{KEY_A,           cmd_move_start,     {.u = DIR_LEFT},   cmd_move_stop,      {.u = DIR_LEFT}},
{KEY_S,           cmd_move_start,     {.u = DIR_DOWN},   cmd_move_stop,      {.u = DIR_DOWN}},
{KEY_D,           cmd_move_start,     {.u = DIR_RIGHT},  cmd_move_stop,      {.u = DIR_RIGHT}},

// Scrolling (hold Tab for scroll mode)
{KEY_TAB,         cmd_move_to_scroll, {.i = 1},          cmd_move_to_scroll, {.i = 0}},

// Speed multipliers
{KEY_LEFTALT,     cmd_divide_speed,   {.f = 8.0},        cmd_multiply_speed, {.f = 8.0}},
{KEY_LEFTCTRL,    cmd_multiply_speed, {.f = 32.0},       cmd_divide_speed,   {.f = 32.0}},
{KEY_J,           cmd_divide_speed,   {.f = 8.0},        cmd_multiply_speed, {.f = 8.0}},
{KEY_K,           cmd_divide_speed,   {.f = 2.0},        cmd_multiply_speed, {.f = 2.0}},
{KEY_L,           cmd_multiply_speed, {.f = 4.0},        cmd_divide_speed,   {.f = 4.0}},
{KEY_SEMICOLON,   cmd_multiply_speed, {.f = 8.0},        cmd_divide_speed,   {.f = 8.0}},

// Clicking (left-handed: space/e/r; right-handed for dragging with WASD: n/m)
{KEY_SPACE,       cmd_click_press,    {.u = BTN_LEFT},   cmd_click_release,  {.u = BTN_LEFT}},
{KEY_E,           cmd_click_press,    {.u = BTN_RIGHT},  cmd_click_release,  {.u = BTN_RIGHT}},
{KEY_R,           cmd_click_press,    {.u = BTN_MIDDLE}, cmd_click_release,  {.u = BTN_MIDDLE}},
{KEY_N,           cmd_click_press,    {.u = BTN_RIGHT},  cmd_click_release,  {.u = BTN_RIGHT}},
{KEY_M,           cmd_click_press,    {.u = BTN_MIDDLE}, cmd_click_release,  {.u = BTN_MIDDLE}},
};
