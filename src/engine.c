/*
 * Pure pointer/scroll kinematics and key routing state machine.
 * Zero OS syscalls, zero file I/O, zero dynamic heap allocations.
 */

#include "uptrkeys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

// True modifiers that pass through in pointer mode unless bound in bindings[].
static const unsigned int mod_keycodes[] = {
	KEY_LEFTSHIFT,  KEY_RIGHTSHIFT,
	KEY_LEFTCTRL,   KEY_RIGHTCTRL,
	KEY_LEFTALT,    KEY_RIGHTALT,
	KEY_LEFTMETA,   KEY_RIGHTMETA,
};

static const struct {
	unsigned int code;
	const char *name;
} key_names[] = {
	{KEY_A, "a"}, {KEY_B, "b"}, {KEY_C, "c"}, {KEY_D, "d"}, {KEY_E, "e"},
	{KEY_F, "f"}, {KEY_G, "g"}, {KEY_H, "h"}, {KEY_I, "i"}, {KEY_J, "j"},
	{KEY_K, "k"}, {KEY_L, "l"}, {KEY_M, "m"}, {KEY_N, "n"}, {KEY_O, "o"},
	{KEY_P, "p"}, {KEY_Q, "q"}, {KEY_R, "r"}, {KEY_S, "s"}, {KEY_T, "t"},
	{KEY_U, "u"}, {KEY_V, "v"}, {KEY_W, "w"}, {KEY_X, "x"}, {KEY_Y, "y"},
	{KEY_Z, "z"},
	{KEY_0, "0"}, {KEY_1, "1"}, {KEY_2, "2"}, {KEY_3, "3"}, {KEY_4, "4"},
	{KEY_5, "5"}, {KEY_6, "6"}, {KEY_7, "7"}, {KEY_8, "8"}, {KEY_9, "9"},
	{KEY_LEFTSHIFT, "Shift_L"},   {KEY_RIGHTSHIFT, "Shift_R"},
	{KEY_LEFTCTRL,  "Control_L"}, {KEY_RIGHTCTRL,  "Control_R"},
	{KEY_LEFTALT,   "Alt_L"},     {KEY_RIGHTALT,   "Alt_R"},
	{KEY_LEFTMETA,  "Super_L"},   {KEY_RIGHTMETA,  "Super_R"},
	{KEY_CAPSLOCK,  "CapsLock"},  {KEY_NUMLOCK,    "NumLock"},
	{KEY_COMPOSE,   "Compose"},   {KEY_SELECT,     "Select"},
	{KEY_SPACE,     "space"},     {KEY_SEMICOLON,  "semicolon"},
	{KEY_COMMA,     "comma"},     {KEY_DOT,        "period"},
	{KEY_SLASH,     "slash"},     {KEY_BACKSLASH,  "backslash"},
	{KEY_APOSTROPHE,"apostrophe"},{KEY_GRAVE,      "grave"},
	{KEY_MINUS,     "minus"},     {KEY_EQUAL,      "equal"},
	{KEY_LEFTBRACE, "bracketleft"},{KEY_RIGHTBRACE,"bracketright"},
	{KEY_HOME,      "Home"},      {KEY_END,        "End"},
	{KEY_PAGEUP,    "PageUp"},    {KEY_PAGEDOWN,   "PageDown"},
	{KEY_INSERT,    "Insert"},    {KEY_DELETE,     "Delete"},
	{KEY_ENTER,     "Return"},    {KEY_ESC,        "Escape"},
	{KEY_TAB,       "Tab"},       {KEY_BACKSPACE,  "BackSpace"},
	{KEY_UP, "Up"}, {KEY_DOWN, "Down"}, {KEY_LEFT, "Left"}, {KEY_RIGHT, "Right"},
	{KEY_F1,  "F1"},  {KEY_F2,  "F2"},  {KEY_F3,  "F3"},  {KEY_F4,  "F4"},
	{KEY_F5,  "F5"},  {KEY_F6,  "F6"},  {KEY_F7,  "F7"},  {KEY_F8,  "F8"},
	{KEY_F9,  "F9"},  {KEY_F10, "F10"}, {KEY_F11, "F11"}, {KEY_F12, "F12"},
};

static void
eng_log(const Engine *eng, int level, const char *fmt, ...)
{
	if (!eng || !eng->ops.log_msg || eng->debug_level < level) return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	eng->ops.log_msg(eng->ops.userdata, level, buf);
}

const char *
keycode_name(unsigned int code)
{
	for (size_t i = 0; i < ARRAY_LEN(key_names); i++) {
		if (key_names[i].code == code) return key_names[i].name;
	}
	return NULL;
}

static bool
is_mod_key(unsigned int code)
{
	for (size_t i = 0; i < ARRAY_LEN(mod_keycodes); i++) {
		if (mod_keycodes[i] == code) return true;
	}
	return false;
}

static void
emit_virt_key(Engine *eng, unsigned int code, int value)
{
	if (code >= KEY_CNT) return;
	if (value == 1 || value == 0) eng->virt_down[code] = (uint8_t)value;
	if (eng->ops.emit_key) eng->ops.emit_key(eng->ops.userdata, code, value);
}

static void
emit_virt_button(Engine *eng, unsigned int button, int value)
{
	if (button >= BTN_LEFT && button < BTN_LEFT + MAX_MOUSE_BUTTONS) {
		eng->btn_down[button - BTN_LEFT] = (value != 0) ? 1 : 0;
	}
	if (eng->ops.emit_button) eng->ops.emit_button(eng->ops.userdata, button, value);
}

static const Binding *
find_binding(const Engine *eng, unsigned int code)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		if (eng->bindings[i].keycode == code) return &eng->bindings[i];
	}
	return NULL;
}

static void
reset_tracks(Engine *eng)
{
	eng->ptr = (MotionTrack){.base_speed = eng->base_speed, .mul = 1.0};
	eng->scroll = (MotionTrack){.base_speed = eng->base_scroll, .mul = 1.0};
	eng->move_is_scroll = false;
}

void
engine_init(Engine *eng, unsigned int prefix_key,
            const Binding *bindings, size_t num_bindings,
            double base_speed, double base_scroll, EngineOps ops)
{
	memset(eng, 0, sizeof(*eng));
	eng->prefix_key = prefix_key;
	eng->bindings = bindings;
	eng->num_bindings = num_bindings;
	eng->base_speed = base_speed;
	eng->base_scroll = base_scroll;
	eng->ops = ops;
	reset_tracks(eng);
}

bool
engine_validate_bindings(unsigned int prefix_key, const Binding *bindings,
                         size_t num_bindings, char *errbuf, size_t errlen)
{
	if (prefix_key >= KEY_CNT) {
		if (errbuf && errlen) snprintf(errbuf, errlen, "invalid prefix_key 0x%x", prefix_key);
		return false;
	}
	for (size_t i = 0; i < num_bindings; i++) {
		unsigned int code = bindings[i].keycode;
		const char *err = NULL;
		if (code >= KEY_CNT) {
			if (errbuf && errlen) {
				snprintf(errbuf, errlen, "invalid keycode 0x%x at index %zu", code, i);
			}
			return false;
		}
		if (code == prefix_key) {
			err = "prefix key cannot appear in bindings";
		} else {
			for (size_t j = i + 1; j < num_bindings; j++) {
				if (bindings[j].keycode == code) {
					err = "duplicate binding";
					break;
				}
			}
		}
		if (err) {
			const char *kname = keycode_name(code);
			if (errbuf && errlen) {
				if (kname) snprintf(errbuf, errlen, "%s: %s", err, kname);
				else snprintf(errbuf, errlen, "%s: key(0x%x)", err, code);
			}
			return false;
		}
	}
	return true;
}

static void
start_dir(MotionTrack *m, unsigned int dir)
{
	if (dir & (DIR_UP | DIR_DOWN)) {
		m->y_rem = 0.0;
		m->y_cont = false;
	}
	if ((dir & DIR_UP) && !(dir & DIR_DOWN)) m->dir = (m->dir & ~DIR_DOWN) | DIR_UP;
	else if ((dir & DIR_DOWN) && !(dir & DIR_UP)) m->dir = (m->dir & ~DIR_UP) | DIR_DOWN;

	if (dir & (DIR_LEFT | DIR_RIGHT)) {
		m->x_rem = 0.0;
		m->x_cont = false;
	}
	if ((dir & DIR_LEFT) && !(dir & DIR_RIGHT)) m->dir = (m->dir & ~DIR_RIGHT) | DIR_LEFT;
	else if ((dir & DIR_RIGHT) && !(dir & DIR_LEFT)) m->dir = (m->dir & ~DIR_LEFT) | DIR_RIGHT;
}

PointerDelta
pointer_tick(MotionTrack *m, int usec)
{
	PointerDelta pd = {0, 0};
	if (m->dir == DIR_NONE || usec <= 0) return pd;

	double xsign = ((m->dir & DIR_RIGHT) ? 1.0 : 0.0) - ((m->dir & DIR_LEFT) ? 1.0 : 0.0);
	double ysign = ((m->dir & DIR_DOWN) ? 1.0 : 0.0) - ((m->dir & DIR_UP) ? 1.0 : 0.0);
	double step = m->base_speed * m->mul * ((double)usec / 1e6);

	double int_part = 0.0;
	m->x_rem = modf(step * xsign + m->x_rem, &int_part);
	pd.dx = (int)int_part;
	m->y_rem = modf(step * ysign + m->y_rem, &int_part);
	pd.dy = (int)int_part;
	return pd;
}

static void
scroll_axis_tick(double base_speed, double mul, int usec, int sign,
                 double *rem, bool *cont, int *out_discrete, int *out_hires)
{
	*out_discrete = 0;
	*out_hires = 0;
	if (sign == 0 || usec <= 0) return;

	double pos = *rem + base_speed * mul * ((double)usec / 1e6);
	int discrete = 0;
	int hires = 0;

	if (!*cont) {
		*cont = true;
		if ((int)pos == 0) {
			discrete = 1;
			hires = HIRES_PER_NOTCH;
			*rem = pos - 1.0;
		} else {
			double int_part = 0.0;
			*rem = modf(pos, &int_part);
			discrete = (int)int_part;
			hires = (int)llround(pos * HIRES_PER_NOTCH);
		}
	} else if (pos > 0.0) {
		double prev_pos = (*rem > 0.0) ? *rem : 0.0;
		hires = (int)llround(pos * HIRES_PER_NOTCH) - (int)llround(prev_pos * HIRES_PER_NOTCH);
		double int_part = 0.0;
		*rem = modf(pos, &int_part);
		discrete = (int)int_part;
	} else {
		*rem = pos;
	}

	*out_discrete = sign * discrete;
	*out_hires = sign * hires;
}

ScrollDelta
scroll_tick(MotionTrack *m, int usec)
{
	ScrollDelta sd = {0, 0, 0, 0};
	if (m->dir == DIR_NONE || usec <= 0) return sd;

	int xsign = ((m->dir & DIR_RIGHT) ? 1 : 0) - ((m->dir & DIR_LEFT) ? 1 : 0);
	int ysign = ((m->dir & DIR_UP) ? 1 : 0) - ((m->dir & DIR_DOWN) ? 1 : 0);
	scroll_axis_tick(m->base_speed, m->mul, usec, xsign,
	                 &m->x_rem, &m->x_cont, &sd.x_discrete, &sd.x_hires);
	scroll_axis_tick(m->base_speed, m->mul, usec, ysign,
	                 &m->y_rem, &m->y_cont, &sd.y_discrete, &sd.y_hires);
	return sd;
}

void
engine_tick(Engine *eng, int usec)
{
	ScrollDelta sd = scroll_tick(&eng->scroll, usec);
	if (eng->move_is_scroll) {
		ScrollDelta msd = scroll_tick(&eng->ptr, usec);
		sd.x_discrete += msd.x_discrete;
		sd.y_discrete += msd.y_discrete;
		sd.x_hires += msd.x_hires;
		sd.y_hires += msd.y_hires;
	} else {
		PointerDelta pd = pointer_tick(&eng->ptr, usec);
		if ((pd.dx || pd.dy) && eng->ops.emit_motion) {
			eng->ops.emit_motion(eng->ops.userdata, pd.dx, pd.dy);
		}
	}
	if ((sd.x_discrete || sd.y_discrete || sd.x_hires || sd.y_hires) && eng->ops.emit_scroll) {
		eng->ops.emit_scroll(eng->ops.userdata, sd);
	}
}

bool
engine_is_moving(const Engine *eng)
{
	return eng->ptr.dir != DIR_NONE || eng->scroll.dir != DIR_NONE;
}

static void
activate(Engine *eng)
{
	eng->active = true;
	eng_log(eng, 1, "state: active=1 (pointer mode enabled)");
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (!eng->virt_down[k]) continue;
		// Keep unbound modifiers down so pre-held Super/Shift+Click works.
		if (is_mod_key(k) && !find_binding(eng, k)) continue;
		emit_virt_key(eng, k, 0);
		eng->suppress_until_release[k] = 1;
		const char *kname = keycode_name(k);
		eng_log(eng, 1, "release & suppress pre-held key %s (%u)",
		        kname ? kname : "unknown", k);
	}
}

static void
deactivate(Engine *eng)
{
	int released = 0;
	for (unsigned int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
		if (eng->btn_down[i]) {
			emit_virt_button(eng, BTN_LEFT + i, 0);
			released++;
		}
	}
	eng->active = false;
	reset_tracks(eng);
	eng_log(eng, 1, "state: active=0 (pointer mode disabled, released_btns=%d)", released);
}

void
engine_process_key(Engine *eng, unsigned int code, int value)
{
	if (code >= KEY_CNT) return;
	const char *kname = keycode_name(code);
	if (!kname) kname = "unknown";

	if (value == 2) {
		bool forward = !eng->active && eng->virt_down[code];
		eng_log(eng, 2, "key %s (%u) val=2 -> %s", kname, code,
		        forward ? "forward" : "suppressed");
		if (forward) emit_virt_key(eng, code, 2);
		return;
	}
	if (value != 0 && value != 1) return;

	eng_log(eng, 1, "in key %s (%u) val=%d active=%d",
	        kname, code, value, eng->active ? 1 : 0);

	if (value == 0) {
		if (eng->virt_down[code]) emit_virt_key(eng, code, 0);
		if (eng->suppress_until_release[code]) {
			eng->suppress_until_release[code] = 0;
			return;
		}
		if (code == eng->prefix_key) {
			if (eng->active) deactivate(eng);
			return;
		}
		if (eng->active) {
			const Binding *b = find_binding(eng, code);
			if (b && b->release_fn) b->release_fn(eng, &b->release_arg);
		}
		return;
	}

	if (code == eng->prefix_key) {
		if (!eng->active) activate(eng);
		return;
	}
	if (!eng->active) {
		emit_virt_key(eng, code, 1);
		return;
	}
	if (eng->suppress_until_release[code]) {
		eng_log(eng, 2, "key %s (%u) suppressed until release", kname, code);
		return;
	}

	const Binding *b = find_binding(eng, code);
	if (b) {
		if (b->press_fn) b->press_fn(eng, &b->press_arg);
	} else if (is_mod_key(code)) {
		emit_virt_key(eng, code, 1);
	} else {
		eng_log(eng, 2, "swallowed unbound key %s (%u) while active", kname, code);
	}
}

static void
log_track(const Engine *eng, const char *kind, const MotionTrack *m, int scroll_mode)
{
	if (eng->debug_level < 1) return;
	char dbuf[8];
	snprintf(dbuf, sizeof(dbuf), "%s%s%s%s",
	         (m->dir & DIR_UP) ? "U" : "",
	         (m->dir & DIR_DOWN) ? "D" : "",
	         (m->dir & DIR_LEFT) ? "L" : "",
	         (m->dir & DIR_RIGHT) ? "R" : "");
	if (!dbuf[0]) snprintf(dbuf, sizeof(dbuf), "none");
	if (scroll_mode >= 0) {
		eng_log(eng, 1, "%s: dir=%s mul=%.3g scroll_mode=%d", kind, dbuf, m->mul, scroll_mode);
	} else {
		eng_log(eng, 1, "%s: dir=%s mul=%.3g", kind, dbuf, m->mul);
	}
}

void
cmd_move_start(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	start_dir(&eng->ptr, arg->u);
	log_track(eng, "motion", &eng->ptr, eng->move_is_scroll ? 1 : 0);
}

void
cmd_move_stop(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	unsigned int prev = eng->ptr.dir;
	eng->ptr.dir &= ~arg->u;
	if (eng->ptr.dir != prev) {
		log_track(eng, "motion", &eng->ptr, eng->move_is_scroll ? 1 : 0);
	}
}

void
cmd_scroll_start(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	start_dir(&eng->scroll, arg->u);
	log_track(eng, "scroll", &eng->scroll, -1);
}

void
cmd_scroll_stop(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	unsigned int prev = eng->scroll.dir;
	eng->scroll.dir &= ~arg->u;
	if (eng->scroll.dir != prev) {
		log_track(eng, "scroll", &eng->scroll, -1);
	}
}

void
cmd_move_to_scroll(Engine *eng, const Arg *arg)
{
	bool enable = (arg && arg->i != 0);
	if (enable == eng->move_is_scroll) return;
	eng->move_is_scroll = enable;
	eng->ptr.base_speed = enable ? eng->base_scroll : eng->base_speed;
	eng->ptr.x_rem = 0.0;
	eng->ptr.y_rem = 0.0;
	eng->ptr.x_cont = false;
	eng->ptr.y_cont = false;
	eng_log(eng, 1, "state: move_is_scroll=%d (base_speed=%.1f)",
	        enable ? 1 : 0, eng->ptr.base_speed);
}

void
cmd_multiply_speed(Engine *eng, const Arg *arg)
{
	if (!arg || arg->f <= 0.0) return;
	eng->ptr.mul *= arg->f;
	eng->scroll.mul *= arg->f;
	eng_log(eng, 1, "speed: *%.3g -> mul=%.4g", arg->f, eng->ptr.mul);
}

void
cmd_divide_speed(Engine *eng, const Arg *arg)
{
	if (!arg || arg->f <= 0.0) return;
	eng->ptr.mul /= arg->f;
	eng->scroll.mul /= arg->f;
	eng_log(eng, 1, "speed: /%.3g -> mul=%.4g", arg->f, eng->ptr.mul);
}

void
cmd_click_press(Engine *eng, const Arg *arg)
{
	if (arg) emit_virt_button(eng, arg->u, 1);
}

void
cmd_click_release(Engine *eng, const Arg *arg)
{
	if (arg) emit_virt_button(eng, arg->u, 0);
}

void
cmd_quit(Engine *eng, const Arg *arg)
{
	(void)arg;
	deactivate(eng);
	eng->quitting = true;
}
