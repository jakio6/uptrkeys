#ifndef UPTRKEYS_H
#define UPTRKEYS_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#define HIRES_PER_NOTCH 120
#define MAX_MOUSE_BUTTONS 8

enum Direction {
	DIR_NONE  = 0,
	DIR_UP    = (1u << 0),
	DIR_DOWN  = (1u << 1),
	DIR_LEFT  = (1u << 2),
	DIR_RIGHT = (1u << 3),
};

enum ModMask {
	MOD_NONE    = 0,
	MOD_SHIFT   = (1u << 0),
	MOD_CAPS    = (1u << 1),
	MOD_CTRL    = (1u << 2),
	MOD_ALT     = (1u << 3),
	MOD_NUMLOCK = (1u << 4),
	MOD_SUPER   = (1u << 5),
	MOD_ALTGR   = (1u << 6),
};

enum BindOpts {
	BIND_NONE     = 0,
	BIND_GLOBAL   = (1u << 0),
	BIND_NOREPEAT = (1u << 1),
	BIND_ANY_MOD  = (1u << 2),
};

typedef struct {
	double base_speed;
	unsigned int dir;
	double mul;
	double x_rem;
	double y_rem;
	bool x_cont;
	bool y_cont;
} MotionTrack;

typedef struct {
	int dx;
	int dy;
} PointerDelta;

typedef struct {
	int x_discrete;
	int y_discrete;
	int x_hires;
	int y_hires;
} ScrollDelta;

typedef union {
	int i;
	unsigned int u;
	double f;
	const void *v;
} Arg;

struct Engine;

typedef void (*ActionFn)(struct Engine *eng, const Arg *arg);

typedef struct {
	unsigned int mod;
	unsigned int keycode;
	unsigned int opts;
	ActionFn press_fn;
	Arg press_arg;
	ActionFn release_fn;
	Arg release_arg;
} Binding;

typedef struct {
	void (*emit_key)(void *userdata, unsigned int keycode, int value);
	void (*emit_button)(void *userdata, unsigned int button, int value);
	void (*emit_motion)(void *userdata, int dx, int dy);
	void (*emit_scroll)(void *userdata, ScrollDelta sd);
	void (*log_msg)(void *userdata, int level, const char *msg);
	void *userdata;
} EngineOps;

typedef struct Engine {
	const Binding *bindings;
	size_t num_bindings;
	unsigned int internal_mods;
	double base_speed;
	double base_scroll;
	EngineOps ops;
	int debug_level;

	MotionTrack ptr;
	MotionTrack scroll;
	bool active;
	bool move_is_scroll;
	bool quitting;

	uint8_t phys_down[KEY_CNT];
	uint8_t virt_down[KEY_CNT];
	uint8_t suppress_until_release[KEY_CNT];
	int active_release_idx[KEY_CNT];
	uint8_t btn_down[MAX_MOUSE_BUTTONS];
} Engine;

// Pure state machine and kinematics (src/engine.c — zero syscalls, zero heap)
void engine_init(Engine *eng, const Binding *bindings, size_t num_bindings,
                 unsigned int internal_mods, double base_speed,
                 double base_scroll, EngineOps ops);
bool engine_validate_bindings(const Binding *bindings, size_t num_bindings,
                              char *errbuf, size_t errlen);
PointerDelta pointer_tick(MotionTrack *m, int usec);
ScrollDelta scroll_tick(MotionTrack *m, int usec);
void engine_tick(Engine *eng, int usec);
void engine_process_key(Engine *eng, unsigned int code, int value);
bool engine_is_moving(const Engine *eng);
unsigned int engine_current_mods(const Engine *eng);
unsigned int keycode_to_modbit(unsigned int code);
const char *keycode_name(unsigned int code);
void format_binding(char *dst, size_t dstlen, unsigned int mod, unsigned int keycode);

// Bindable commands (src/engine.c)
void cmd_activate(Engine *eng, const Arg *arg);
void cmd_deactivate(Engine *eng, const Arg *arg);
void cmd_toggle_active(Engine *eng, const Arg *arg);
void cmd_activate_scroll(Engine *eng, const Arg *arg);
void cmd_move_start(Engine *eng, const Arg *arg);
void cmd_move_stop(Engine *eng, const Arg *arg);
void cmd_scroll_start(Engine *eng, const Arg *arg);
void cmd_scroll_stop(Engine *eng, const Arg *arg);
void cmd_move_to_scroll(Engine *eng, const Arg *arg);
void cmd_toggle_move_to_scroll(Engine *eng, const Arg *arg);
void cmd_multiply_speed(Engine *eng, const Arg *arg);
void cmd_divide_speed(Engine *eng, const Arg *arg);
void cmd_click_press(Engine *eng, const Arg *arg);
void cmd_click_release(Engine *eng, const Arg *arg);
void cmd_reset(Engine *eng, const Arg *arg);
void cmd_quit(Engine *eng, const Arg *arg);

// Kernel evdev/uinput backend (src/backend.c — zero heap, /dev/input + /dev/uinput only)
extern int debug_level;
int backend_run(Engine *eng, int fps, const volatile sig_atomic_t *stop_flag);

#endif
