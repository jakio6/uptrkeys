#include "uptrkeys.h"
#include "config.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

typedef struct {
	unsigned int type; // 1=key, 2=button, 3=motion, 4=scroll
	unsigned int code;
	int val1;
	int val2;
	int val3;
	int val4;
} RecordedEvent;

typedef struct {
	RecordedEvent events[256];
	size_t count;
	size_t msg_count;
} EventLog;

static void
log_key(void *ud, unsigned int keycode, int value)
{
	EventLog *log = ud;
	assert(log->count < ARRAY_LEN(log->events));
	log->events[log->count++] = (RecordedEvent){
		.type = 1, .code = keycode, .val1 = value,
	};
}

static void
log_button(void *ud, unsigned int button, int value)
{
	EventLog *log = ud;
	assert(log->count < ARRAY_LEN(log->events));
	log->events[log->count++] = (RecordedEvent){
		.type = 2, .code = button, .val1 = value,
	};
}

static void
log_motion(void *ud, int dx, int dy)
{
	EventLog *log = ud;
	assert(log->count < ARRAY_LEN(log->events));
	log->events[log->count++] = (RecordedEvent){
		.type = 3, .val1 = dx, .val2 = dy,
	};
}

static void
log_scroll(void *ud, ScrollDelta sd)
{
	EventLog *log = ud;
	assert(log->count < ARRAY_LEN(log->events));
	log->events[log->count++] = (RecordedEvent){
		.type = 4,
		.val1 = sd.x_discrete,
		.val2 = sd.y_discrete,
		.val3 = sd.x_hires,
		.val4 = sd.y_hires,
	};
}

static void
log_msg_cb(void *ud, int level, const char *msg)
{
	EventLog *log = ud;
	assert(level >= 1 && msg != NULL && msg[0] != '\0');
	log->msg_count++;
}

static EngineOps
make_test_ops(EventLog *log)
{
	memset(log, 0, sizeof(*log));
	return (EngineOps){
		.emit_key = log_key,
		.emit_button = log_button,
		.emit_motion = log_motion,
		.emit_scroll = log_scroll,
		.log_msg = log_msg_cb,
		.userdata = log,
	};
}

static int
count_events(const EventLog *log, unsigned int type, unsigned int code, int val1)
{
	int n = 0;
	for (size_t i = 0; i < log->count; i++) {
		const RecordedEvent *e = &log->events[i];
		if (e->type == type && e->code == code && e->val1 == val1) n++;
	}
	return n;
}

static void
test_pointer_tick(void)
{
	MotionTrack m = {.base_speed = 100.0, .mul = 1.0, .dir = DIR_RIGHT};
	PointerDelta pd = pointer_tick(&m, 1000000);
	assert(pd.dx == 100 && pd.dy == 0);

	m = (MotionTrack){.base_speed = 100.0, .mul = 1.0, .dir = DIR_LEFT | DIR_UP};
	pd = pointer_tick(&m, 500000);
	assert(pd.dx == -50 && pd.dy == -50);

	// Subpixel accumulation across 4 ticks of 3ms at 100px/sec = 1.2px
	m = (MotionTrack){.base_speed = 100.0, .mul = 1.0, .dir = DIR_RIGHT};
	assert(pointer_tick(&m, 3000).dx == 0);
	assert(pointer_tick(&m, 3000).dx == 0);
	assert(pointer_tick(&m, 3000).dx == 0);
	assert(pointer_tick(&m, 3000).dx == 1);
}

static void
test_scroll_tick(void)
{
	// 10 notches/sec over 25 frames of 40ms -> 10 discrete notches and 1200 hires units
	MotionTrack m = {.base_speed = 10.0, .mul = 1.0, .dir = DIR_UP};
	int sum_disc = 0;
	int sum_hires = 0;

	ScrollDelta first = scroll_tick(&m, 40000);
	assert(first.y_discrete == 1);
	assert(first.y_hires == HIRES_PER_NOTCH);
	sum_disc += first.y_discrete;
	sum_hires += first.y_hires;

	for (int i = 1; i < 25; i++) {
		ScrollDelta sd = scroll_tick(&m, 40000);
		sum_disc += sd.y_discrete;
		sum_hires += sd.y_hires;
	}
	assert(sum_disc == 10);
	assert(sum_hires == 10 * HIRES_PER_NOTCH);

	// Slow multiplier (1/5 of 10 = 2 notches/sec): smooth hires output after 1st tick
	m = (MotionTrack){.base_speed = 10.0, .mul = 0.2, .dir = DIR_LEFT};
	sum_disc = 0;
	sum_hires = 0;
	int hires_frames = 0;
	for (int i = 0; i < 100; i++) {
		ScrollDelta sd = scroll_tick(&m, 10000);
		sum_disc += sd.x_discrete;
		sum_hires += sd.x_hires;
		if (sd.x_hires < 0) hires_frames++;
	}
	assert(sum_disc == -2);
	assert(sum_hires == -2 * HIRES_PER_NOTCH);
	assert(hires_frames > 10);
}

static void
test_binding_validation(void)
{
	char err[128] = {0};
	assert(engine_validate_bindings(PREFIX_KEY, bindings, ARRAY_LEN(bindings), err, sizeof(err)));

	Binding dup[] = {
		{KEY_W, cmd_move_start, {.u = DIR_UP}, cmd_move_stop, {.u = DIR_UP}},
		{KEY_W, cmd_move_start, {.u = DIR_DOWN}, NULL, {0}},
	};
	assert(!engine_validate_bindings(PREFIX_KEY, dup, ARRAY_LEN(dup), err, sizeof(err)));

	Binding prefix_in_table[] = {
		{KEY_CAPSLOCK, cmd_quit, {0}, NULL, {0}},
	};
	assert(!engine_validate_bindings(KEY_CAPSLOCK, prefix_in_table, ARRAY_LEN(prefix_in_table),
	                                 err, sizeof(err)));

	assert(!engine_validate_bindings(KEY_CNT + 1, bindings, ARRAY_LEN(bindings), err, sizeof(err)));
}

static void
test_normal_mode_and_wm_keys_untouched(void)
{
	EventLog log;
	Engine eng;
	engine_init(&eng, PREFIX_KEY, bindings, ARRAY_LEN(bindings),
	            BASE_SPEED, BASE_SCROLL, make_test_ops(&log));
	eng.debug_level = 2;

	// Normal typing forwards press, repeat, and release
	engine_process_key(&eng, KEY_A, 1);
	engine_process_key(&eng, KEY_A, 2);
	engine_process_key(&eng, KEY_A, 0);
	assert(log.count == 3);
	assert(log.events[0].code == KEY_A && log.events[0].val1 == 1);
	assert(log.events[1].code == KEY_A && log.events[1].val1 == 2);
	assert(log.events[2].code == KEY_A && log.events[2].val1 == 0);
	assert(!eng.virt_down[KEY_A]);

	// Tiling WM shortcuts (Super+w, Super+v, Super+g, Super+Return) are never stolen
	log.count = 0;
	static const unsigned int wm_keys[] = {KEY_W, KEY_V, KEY_G, KEY_ENTER};
	engine_process_key(&eng, KEY_LEFTMETA, 1);
	for (size_t i = 0; i < ARRAY_LEN(wm_keys); i++) {
		engine_process_key(&eng, wm_keys[i], 1);
		engine_process_key(&eng, wm_keys[i], 0);
	}
	engine_process_key(&eng, KEY_LEFTMETA, 0);
	assert(log.count == 10);
	assert(!eng.active);
	assert(!eng.virt_down[KEY_LEFTMETA]);
}

static void
test_mod_plus_mouse_both_orders(void)
{
	EventLog log;
	Engine eng;
	engine_init(&eng, PREFIX_KEY, bindings, ARRAY_LEN(bindings),
	            BASE_SPEED, BASE_SCROLL, make_test_ops(&log));
	eng.debug_level = 2;

	// Order A: Hold Super FIRST, then hold CapsLock -> Super+LeftDrag works like physical mouse!
	engine_process_key(&eng, KEY_LEFTMETA, 1);
	assert(eng.virt_down[KEY_LEFTMETA] == 1);

	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	assert(eng.active);
	assert(eng.virt_down[KEY_LEFTMETA] == 1);
	assert(eng.virt_down[KEY_CAPSLOCK] == 0);
	assert(count_events(&log, 1, KEY_CAPSLOCK, 1) == 0);

	engine_process_key(&eng, KEY_SPACE, 1);
	assert(eng.btn_down[0] == 1);
	assert(count_events(&log, 2, BTN_LEFT, 1) == 1);

	engine_process_key(&eng, KEY_D, 1);
	engine_tick(&eng, 10000);
	assert(log.events[log.count - 1].type == 3 && log.events[log.count - 1].val1 == 10);

	// Release CapsLock while still holding Super+Space+D -> exits cleanly & releases BTN_LEFT
	engine_process_key(&eng, KEY_CAPSLOCK, 0);
	assert(!eng.active);
	assert(eng.btn_down[0] == 0);
	assert(eng.virt_down[KEY_LEFTMETA] == 1);
	assert(count_events(&log, 2, BTN_LEFT, 0) == 1);

	engine_process_key(&eng, KEY_D, 0);
	engine_process_key(&eng, KEY_SPACE, 0);
	engine_process_key(&eng, KEY_LEFTMETA, 0);
	assert(eng.virt_down[KEY_LEFTMETA] == 0);

	// Order B: Hold CapsLock FIRST, then hold Super + RightClick + W/S (drag-resize)
	log.count = 0;
	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	assert(eng.active);
	engine_process_key(&eng, KEY_LEFTMETA, 1);
	assert(eng.virt_down[KEY_LEFTMETA] == 1);
	engine_process_key(&eng, KEY_E, 1);
	assert(eng.btn_down[BTN_RIGHT - BTN_LEFT] == 1);
	engine_process_key(&eng, KEY_W, 1);
	engine_tick(&eng, 10000);
	assert(log.events[log.count - 1].type == 3 && log.events[log.count - 1].val2 == -10);

	engine_process_key(&eng, KEY_W, 0);
	engine_process_key(&eng, KEY_E, 0);
	engine_process_key(&eng, KEY_LEFTMETA, 0);
	assert(eng.virt_down[KEY_LEFTMETA] == 0);
	engine_process_key(&eng, KEY_CAPSLOCK, 0);
	assert(!eng.active);
}

static void
test_passthrough_vs_bound_mods(void)
{
	EventLog log;
	Engine eng;
	engine_init(&eng, PREFIX_KEY, bindings, ARRAY_LEN(bindings),
	            BASE_SPEED, BASE_SCROLL, make_test_ops(&log));
	eng.debug_level = 2;

	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	assert(eng.active);

	// Unbound modifiers pass through for Mod+Click / Mod+Scroll!
	engine_process_key(&eng, KEY_RIGHTCTRL, 1);
	engine_process_key(&eng, KEY_LEFTSHIFT, 1);
	engine_process_key(&eng, KEY_RIGHTSHIFT, 1);
	engine_process_key(&eng, KEY_RIGHTALT, 1);
	assert(eng.virt_down[KEY_RIGHTCTRL] == 1);
	assert(eng.virt_down[KEY_LEFTSHIFT] == 1);
	assert(eng.virt_down[KEY_RIGHTSHIFT] == 1);
	assert(eng.virt_down[KEY_RIGHTALT] == 1);
	engine_process_key(&eng, KEY_SPACE, 1);
	assert(eng.btn_down[0] == 1);
	engine_process_key(&eng, KEY_SPACE, 0);
	engine_process_key(&eng, KEY_RIGHTALT, 0);
	engine_process_key(&eng, KEY_RIGHTSHIFT, 0);
	engine_process_key(&eng, KEY_LEFTSHIFT, 0);
	engine_process_key(&eng, KEY_RIGHTCTRL, 0);
	assert(eng.virt_down[KEY_RIGHTCTRL] == 0);

	// Bound modifiers (LeftCtrl, LeftAlt) are consumed and scale speed
	engine_process_key(&eng, KEY_LEFTCTRL, 1);
	assert(eng.virt_down[KEY_LEFTCTRL] == 0);
	assert(fabs(eng.ptr.mul - 32.0) < 1e-9);
	engine_process_key(&eng, KEY_LEFTCTRL, 0);
	assert(fabs(eng.ptr.mul - 1.0) < 1e-9);

	engine_process_key(&eng, KEY_LEFTALT, 1);
	assert(eng.virt_down[KEY_LEFTALT] == 0);
	assert(fabs(eng.ptr.mul - 0.125) < 1e-9);
	engine_process_key(&eng, KEY_LEFTALT, 0);
	assert(fabs(eng.ptr.mul - 1.0) < 1e-9);

	engine_process_key(&eng, KEY_CAPSLOCK, 0);
	assert(!eng.active);

	// Pre-held bound modifier (LeftCtrl before CapsLock) is released & suppressed until release
	engine_process_key(&eng, KEY_LEFTCTRL, 1);
	assert(eng.virt_down[KEY_LEFTCTRL] == 1);
	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	assert(eng.virt_down[KEY_LEFTCTRL] == 0);
	assert(eng.suppress_until_release[KEY_LEFTCTRL] == 1);
	engine_process_key(&eng, KEY_LEFTCTRL, 0);
	assert(eng.suppress_until_release[KEY_LEFTCTRL] == 0);
	assert(fabs(eng.ptr.mul - 1.0) < 1e-9);
	engine_process_key(&eng, KEY_CAPSLOCK, 0);
}

static void
test_scroll_and_multipliers_under_capslock(void)
{
	EventLog log;
	Engine eng;
	engine_init(&eng, PREFIX_KEY, bindings, ARRAY_LEN(bindings),
	            BASE_SPEED, BASE_SCROLL, make_test_ops(&log));
	eng.debug_level = 2;

	// Hold CapsLock, hold Tab for scroll mode, scroll diagonally S + A
	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	engine_process_key(&eng, KEY_TAB, 1);
	assert(eng.virt_down[KEY_TAB] == 0);
	assert(eng.move_is_scroll);

	log.count = 0;
	engine_process_key(&eng, KEY_S, 1);
	engine_process_key(&eng, KEY_A, 1);
	engine_tick(&eng, 1);
	assert(log.count == 1 && log.events[0].type == 4);
	assert(log.events[0].val1 == -1 && log.events[0].val2 == -1);
	assert(log.events[0].val3 == -HIRES_PER_NOTCH && log.events[0].val4 == -HIRES_PER_NOTCH);

	engine_process_key(&eng, KEY_A, 0);
	engine_process_key(&eng, KEY_S, 0);
	engine_process_key(&eng, KEY_TAB, 0);
	assert(!eng.move_is_scroll);

	// Speed multipliers J (/8), K (/2), L (*4), ; (*8)
	engine_process_key(&eng, KEY_L, 1);
	assert(fabs(eng.ptr.mul - 4.0) < 1e-9);
	engine_process_key(&eng, KEY_J, 1);
	assert(fabs(eng.ptr.mul - 0.5) < 1e-9);
	engine_process_key(&eng, KEY_SEMICOLON, 1);
	assert(fabs(eng.ptr.mul - 4.0) < 1e-9);
	engine_process_key(&eng, KEY_K, 1);
	assert(fabs(eng.ptr.mul - 2.0) < 1e-9);

	// Releasing CapsLock resets speed multiplier and movement
	engine_process_key(&eng, KEY_CAPSLOCK, 0);
	assert(!eng.active);
	assert(!eng.move_is_scroll);
	assert(fabs(eng.ptr.mul - 1.0) < 1e-9);
	assert(!engine_is_moving(&eng));

	// CapsLock + X quits cleanly while releasing held buttons
	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	engine_process_key(&eng, KEY_SPACE, 1);
	assert(eng.btn_down[0] == 1);
	engine_process_key(&eng, KEY_X, 1);
	assert(eng.quitting && !eng.active && eng.btn_down[0] == 0);
	engine_process_key(&eng, KEY_X, 0);
	engine_process_key(&eng, KEY_SPACE, 0);
	engine_process_key(&eng, KEY_CAPSLOCK, 0);
	assert(eng.virt_down[KEY_CAPSLOCK] == 0);
}

static void
test_autorepeat_and_midkey_activation(void)
{
	EventLog log;
	Engine eng;
	engine_init(&eng, PREFIX_KEY, bindings, ARRAY_LEN(bindings),
	            BASE_SPEED, BASE_SCROLL, make_test_ops(&log));
	eng.debug_level = 2;

	// Normal key pressed BEFORE CapsLock is released cleanly upon CapsLock press
	engine_process_key(&eng, KEY_D, 1);
	engine_process_key(&eng, KEY_D, 2);
	assert(eng.virt_down[KEY_D] == 1);
	assert(count_events(&log, 1, KEY_D, 2) == 1);

	log.count = 0;
	engine_process_key(&eng, KEY_CAPSLOCK, 1);
	assert(eng.active);
	assert(eng.virt_down[KEY_D] == 0);
	assert(count_events(&log, 1, KEY_D, 0) == 1);
	// D was held prior to CapsLock -> not treated as move_start
	assert(eng.ptr.dir == DIR_NONE);

	// Releasing D does not emit duplicate release or unexpected movement stop
	log.count = 0;
	engine_process_key(&eng, KEY_D, 0);
	assert(log.count == 0);

	// Autorepeat (value=2) while CapsLock is active is suppressed for all keys
	engine_process_key(&eng, KEY_LEFTMETA, 1);
	engine_process_key(&eng, KEY_LEFTMETA, 2);
	engine_process_key(&eng, KEY_CAPSLOCK, 2);
	engine_process_key(&eng, KEY_SPACE, 1);
	engine_process_key(&eng, KEY_SPACE, 2);
	assert(count_events(&log, 1, KEY_LEFTMETA, 2) == 0);
	assert(count_events(&log, 1, KEY_CAPSLOCK, 2) == 0);
	assert(count_events(&log, 2, BTN_LEFT, 1) == 1);

	engine_process_key(&eng, KEY_SPACE, 0);
	engine_process_key(&eng, KEY_LEFTMETA, 0);
	engine_process_key(&eng, KEY_CAPSLOCK, 0);
	assert(!eng.active);
	assert(log.msg_count > 10);
}

int
main(void)
{
	test_pointer_tick();
	test_scroll_tick();
	test_binding_validation();
	test_normal_mode_and_wm_keys_untouched();
	test_mod_plus_mouse_both_orders();
	test_passthrough_vs_bound_mods();
	test_scroll_and_multipliers_under_capslock();
	test_autorepeat_and_midkey_activation();
	printf("uptrkeys_test: all 8 tests passed\n");
	return 0;
}
