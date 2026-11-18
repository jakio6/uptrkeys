/*
 * Pure pointer/scroll kinematics and key/modifier routing state machine.
 * Zero OS syscalls, zero file I/O, zero dynamic heap allocations.
 */

#include "uptrkeys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

static const struct {
	unsigned int code;
	unsigned int bit;
} mod_keys[] = {
	{KEY_LEFTSHIFT,  MOD_SHIFT},
	{KEY_RIGHTSHIFT, MOD_SHIFT},
	{KEY_CAPSLOCK,   MOD_CAPS},
	{KEY_LEFTCTRL,   MOD_CTRL},
	{KEY_RIGHTCTRL,  MOD_CTRL},
	{KEY_LEFTALT,    MOD_ALT},
	{KEY_RIGHTALT,   MOD_ALT},
	{KEY_NUMLOCK,    MOD_NUMLOCK},
	{KEY_LEFTMETA,   MOD_SUPER},
	{KEY_RIGHTMETA,  MOD_SUPER},
};

static const struct {
	unsigned int mask;
	const char *name;
} mod_names[] = {
	{MOD_SHIFT,   "Shift"},
	{MOD_CAPS,    "Caps"},
	{MOD_CTRL,    "Ctrl"},
	{MOD_ALT,     "Alt"},
	{MOD_NUMLOCK, "NumLock"},
	{MOD_SUPER,   "Super"},
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

static size_t
format_mods(unsigned int mod, char *dst, size_t dstlen, bool show_none)
{
	if (!dst || dstlen == 0) return 0;
	if (mod == MOD_NONE) {
		if (!show_none) {
			dst[0] = '\0';
			return 0;
		}
		int n = snprintf(dst, dstlen, "none");
		return (n > 0 && (size_t)n < dstlen) ? (size_t)n : dstlen - 1;
	}
	size_t off = 0;
	for (size_t i = 0; i < ARRAY_LEN(mod_names); i++) {
		if (!(mod & mod_names[i].mask)) continue;
		int n = snprintf(dst + off, dstlen - off, "%s%s", off ? "+" : "", mod_names[i].name);
		if (n < 0 || (size_t)n >= dstlen - off) {
			dst[dstlen - 1] = '\0';
			return dstlen - 1;
		}
		off += (size_t)n;
	}
	return off;
}

const char *
keycode_name(unsigned int code)
{
	for (size_t i = 0; i < ARRAY_LEN(key_names); i++) {
		if (key_names[i].code == code) return key_names[i].name;
	}
	return NULL;
}

void
format_binding(char *dst, size_t dstlen, unsigned int mod, unsigned int keycode)
{
	if (!dst || dstlen == 0) return;
	size_t off = format_mods(mod, dst, dstlen, false);
	if (off > 0 && off + 1 < dstlen) {
		dst[off++] = '+';
		dst[off] = '\0';
	}
	const char *kname = keycode_name(keycode);
	if (kname) {
		snprintf(dst + off, dstlen - off, "%s", kname);
	} else {
		snprintf(dst + off, dstlen - off, "key(0x%x)", keycode);
	}
}

unsigned int
keycode_to_modbit(unsigned int code)
{
	for (size_t i = 0; i < ARRAY_LEN(mod_keys); i++) {
		if (mod_keys[i].code == code) return mod_keys[i].bit;
	}
	return MOD_NONE;
}

static unsigned int
mods_except(const Engine *eng, unsigned int exclude_code)
{
	unsigned int mods = MOD_NONE;
	for (size_t i = 0; i < ARRAY_LEN(mod_keys); i++) {
		if (mod_keys[i].code != exclude_code && eng->phys_down[mod_keys[i].code]) {
			mods |= mod_keys[i].bit;
		}
	}
	return mods;
}

unsigned int
engine_current_mods(const Engine *eng)
{
	return mods_except(eng, KEY_CNT);
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

static void
suppress_virt_key(Engine *eng, unsigned int code)
{
	emit_virt_key(eng, code, 0);
	if (eng->phys_down[code]) {
		eng->suppress_until_release[code] = 1;
		const char *kname = keycode_name(code);
		eng_log(eng, 1, "release & suppress pre-held key %s (%u)",
		        kname ? kname : "unknown", code);
	}
}

static bool
is_bound(const Engine *eng, unsigned int code, bool want_global)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if (((b->opts & BIND_GLOBAL) != 0) == want_global && b->keycode == code) {
			return true;
		}
	}
	return false;
}

static bool
is_internal_mod_key(const Engine *eng, unsigned int code)
{
	unsigned int bit = keycode_to_modbit(code);
	return bit && (bit & eng->internal_mods) && is_bound(eng, code, false);
}

static bool
key_has_norepeat(const Engine *eng, unsigned int code)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		if (eng->bindings[i].keycode == code && (eng->bindings[i].opts & BIND_NOREPEAT)) {
			return true;
		}
	}
	return false;
}

static bool
mods_match(unsigned int current_mods, unsigned int bind_mod)
{
	unsigned int mask = ~(MOD_CAPS | MOD_NUMLOCK) | (bind_mod & (MOD_CAPS | MOD_NUMLOCK));
	return (current_mods & mask) == bind_mod;
}

void
engine_init(Engine *eng, const Binding *bindings, size_t num_bindings,
            unsigned int internal_mods, double base_speed,
            double base_scroll, EngineOps ops)
{
	memset(eng, 0, sizeof(*eng));
	eng->bindings = bindings;
	eng->num_bindings = num_bindings;
	eng->internal_mods = internal_mods;
	eng->base_speed = base_speed;
	eng->base_scroll = base_scroll;
	eng->ops = ops;
	eng->ptr = (MotionTrack){.base_speed = base_speed, .mul = 1.0};
	eng->scroll = (MotionTrack){.base_speed = base_scroll, .mul = 1.0};
}

bool
engine_validate_bindings(const Binding *bindings, size_t num_bindings,
                         char *errbuf, size_t errlen)
{
	char desc[128];
	for (size_t i = 0; i < num_bindings; i++) {
		const Binding *a = &bindings[i];
		const char *err = NULL;

		if (a->keycode >= KEY_CNT) {
			if (errbuf && errlen) {
				snprintf(errbuf, errlen, "invalid keycode 0x%x at index %zu", a->keycode, i);
			}
			return false;
		}
		if (a->mod && !(a->opts & BIND_GLOBAL)) {
			err = "modified binding without BIND_GLOBAL";
		} else if ((a->opts & BIND_ANY_MOD) && !(a->opts & BIND_GLOBAL)) {
			err = "BIND_ANY_MOD requires BIND_GLOBAL";
		} else if (a->mod && a->release_fn) {
			err = "modified binding cannot have release_fn";
		} else {
			for (size_t j = i + 1; j < num_bindings; j++) {
				const Binding *b = &bindings[j];
				if (a->keycode != b->keycode) continue;
				if ((a->opts & BIND_GLOBAL) != (b->opts & BIND_GLOBAL)) continue;
				bool both_any = (a->opts & BIND_GLOBAL) &&
				                (a->opts & BIND_ANY_MOD) && (b->opts & BIND_ANY_MOD);
				bool same_mod = (a->mod == b->mod) &&
				                ((a->opts & BIND_ANY_MOD) == (b->opts & BIND_ANY_MOD));
				if (both_any || same_mod) {
					err = "duplicate binding";
					break;
				}
			}
		}
		if (err) {
			format_binding(desc, sizeof(desc), a->mod, a->keycode);
			if (errbuf && errlen) snprintf(errbuf, errlen, "%s: %s", err, desc);
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

static int
find_global_binding(const Engine *eng, unsigned int code, unsigned int mods, bool skip_activate)
{
	// Pass 1: exact modifier match; Pass 2: BIND_ANY_MOD fallback.
	for (int pass = 0; pass < 2; pass++) {
		bool want_any = (pass == 1);
		for (size_t i = 0; i < eng->num_bindings; i++) {
			const Binding *b = &eng->bindings[i];
			if (!(b->opts & BIND_GLOBAL) || b->keycode != code) continue;
			if (((b->opts & BIND_ANY_MOD) != 0) != want_any) continue;
			if (skip_activate && b->press_fn == cmd_activate) continue;
			if (want_any || mods_match(mods, b->mod)) return (int)i;
		}
	}
	return -1;
}

static const Binding *
find_local_binding(const Engine *eng, unsigned int code)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if (!(b->opts & BIND_GLOBAL) && b->mod == MOD_NONE && b->keycode == code) {
			return b;
		}
	}
	return NULL;
}

static bool
dispatch_global(Engine *eng, unsigned int code, unsigned int mods, bool skip_activate)
{
	int idx = find_global_binding(eng, code, mods, skip_activate);
	if (idx < 0) return false;
	const Binding *b = &eng->bindings[idx];
	if (b->release_fn) {
		eng->active_release_idx[code] = idx + 1;
	} else {
		eng->suppress_until_release[code] = 1;
	}
	if (b->press_fn) b->press_fn(eng, &b->press_arg);
	return true;
}

void
engine_process_key(Engine *eng, unsigned int code, int value)
{
	if (code >= KEY_CNT) return;
	const char *kname = keycode_name(code);
	if (!kname) kname = "unknown";

	if (value == 2) {
		bool forward = !eng->active && eng->virt_down[code] && !key_has_norepeat(eng, code);
		eng_log(eng, 2, "key %s (%u) val=2 -> %s", kname, code, forward ? "forward" : "suppressed");
		if (forward) emit_virt_key(eng, code, 2);
		return;
	}
	if (value != 0 && value != 1) return;

	unsigned int mods = mods_except(eng, code);
	if (eng->debug_level >= 1) {
		char modbuf[64];
		format_mods(mods, modbuf, sizeof(modbuf), true);
		eng_log(eng, 1, "in key %s (%u) val=%d active=%d mods=%s",
		        kname, code, value, eng->active ? 1 : 0, modbuf);
	}

	eng->phys_down[code] = (uint8_t)value;

	if (value == 0) {
		if (eng->virt_down[code]) emit_virt_key(eng, code, 0);
		if (eng->suppress_until_release[code]) {
			eng->suppress_until_release[code] = 0;
			return;
		}
		if (eng->active_release_idx[code] > 0) {
			size_t idx = (size_t)(eng->active_release_idx[code] - 1);
			eng->active_release_idx[code] = 0;
			if (idx < eng->num_bindings && eng->bindings[idx].release_fn) {
				eng->bindings[idx].release_fn(eng, &eng->bindings[idx].release_arg);
			}
			return;
		}
		if (eng->active) {
			const Binding *b = find_local_binding(eng, code);
			if (b && b->release_fn) b->release_fn(eng, &b->release_arg);
		}
		return;
	}

	if (!eng->active) {
		if (!dispatch_global(eng, code, mods, false)) emit_virt_key(eng, code, 1);
		return;
	}
	if (eng->suppress_until_release[code]) {
		eng_log(eng, 2, "key %s (%u) suppressed until release", kname, code);
		return;
	}
	if (dispatch_global(eng, code, mods, true)) return;

	bool passed_mod = false;
	if (keycode_to_modbit(code) != MOD_NONE &&
	    !is_internal_mod_key(eng, code) &&
	    !is_bound(eng, code, true)) {
		emit_virt_key(eng, code, 1);
		passed_mod = true;
	}

	const Binding *b = find_local_binding(eng, code);
	if (b) {
		if (b->press_fn) b->press_fn(eng, &b->press_arg);
	} else if (!passed_mod) {
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
cmd_activate(Engine *eng, const Arg *arg)
{
	eng->active = true;
	eng_log(eng, 1, "state: active=1 (pointer mode enabled)");
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (!eng->virt_down[k]) continue;
		if (keycode_to_modbit(k) != MOD_NONE && !is_internal_mod_key(eng, k)) continue;
		suppress_virt_key(eng, k);
	}
	if (arg && arg->u > 0 && arg->u < KEY_CNT && eng->phys_down[arg->u]) {
		eng->suppress_until_release[arg->u] = 1;
	}
}

void
cmd_deactivate(Engine *eng, const Arg *arg)
{
	(void)arg;
	int released = 0;
	for (unsigned int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
		if (eng->btn_down[i]) {
			emit_virt_button(eng, BTN_LEFT + i, 0);
			released++;
		}
	}
	eng->active = false;
	cmd_reset(eng, NULL);
	eng_log(eng, 1, "state: active=0 (pointer mode disabled, released_btns=%d)", released);
}

void
cmd_toggle_active(Engine *eng, const Arg *arg)
{
	(void)arg;
	if (eng->active) cmd_deactivate(eng, NULL);
	else cmd_activate(eng, NULL);
}

void
cmd_activate_scroll(Engine *eng, const Arg *arg)
{
	(void)arg;
	cmd_activate(eng, NULL);
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (eng->virt_down[k] && (keycode_to_modbit(k) & eng->internal_mods)) {
			suppress_virt_key(eng, k);
		}
	}
	Arg enable = {.i = 1};
	cmd_move_to_scroll(eng, &enable);
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
cmd_toggle_move_to_scroll(Engine *eng, const Arg *arg)
{
	(void)arg;
	Arg toggle = {.i = eng->move_is_scroll ? 0 : 1};
	cmd_move_to_scroll(eng, &toggle);
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
cmd_reset(Engine *eng, const Arg *arg)
{
	(void)arg;
	eng->ptr = (MotionTrack){.base_speed = eng->base_speed, .mul = 1.0};
	eng->scroll = (MotionTrack){.base_speed = eng->base_scroll, .mul = 1.0};
	eng->move_is_scroll = false;
}

void
cmd_quit(Engine *eng, const Arg *arg)
{
	(void)arg;
	cmd_deactivate(eng, NULL);
	eng->quitting = true;
}
