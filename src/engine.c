/*
 * Pure pointer/scroll kinematics and key/modifier routing state machine.
 * Zero OS syscalls, zero file I/O, zero dynamic heap allocations.
 */

#include "uptrkeys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const unsigned int mod_keycodes[] = {
	KEY_LEFTSHIFT,
	KEY_RIGHTSHIFT,
	KEY_CAPSLOCK,
	KEY_LEFTCTRL,
	KEY_RIGHTCTRL,
	KEY_LEFTALT,
	KEY_RIGHTALT,
	KEY_NUMLOCK,
	KEY_LEFTMETA,
	KEY_RIGHTMETA,
	KEY_COMPOSE,
};

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

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

static void
format_mods(unsigned int mod, char *dst, size_t dstlen)
{
	if (!dst || dstlen == 0) return;
	if (mod == MOD_NONE) {
		snprintf(dst, dstlen, "none");
		return;
	}
	static const struct {
		unsigned int mask;
		const char *name;
	} mods[] = {
		{MOD_SHIFT, "Shift"},
		{MOD_CAPS, "Caps"},
		{MOD_CTRL, "Ctrl"},
		{MOD_ALT, "Alt"},
		{MOD_NUMLOCK, "NumLock"},
		{MOD_SUPER, "Super"},
		{MOD_ALTGR, "AltGr"},
	};
	size_t off = 0;
	for (size_t i = 0; i < ARRAY_LEN(mods); i++) {
		if (!(mod & mods[i].mask)) continue;
		int n = snprintf(dst + off, dstlen - off, "%s%s", off ? "+" : "", mods[i].name);
		if (n < 0 || (size_t)n >= dstlen - off) {
			dst[dstlen - 1] = '\0';
			return;
		}
		off += (size_t)n;
	}
}

static void
format_dir(unsigned int dir, char *dst, size_t dstlen)
{
	if (!dst || dstlen == 0) return;
	if (dir == DIR_NONE) {
		snprintf(dst, dstlen, "none");
		return;
	}
	snprintf(dst, dstlen, "%s%s%s%s",
	         (dir & DIR_UP) ? "U" : "",
	         (dir & DIR_DOWN) ? "D" : "",
	         (dir & DIR_LEFT) ? "L" : "",
	         (dir & DIR_RIGHT) ? "R" : "");
}

static void
emit_virt_key(Engine *eng, unsigned int code, int value)
{
	if (code >= KEY_CNT) return;
	if (value == 1) {
		eng->virt_down[code] = 1;
	} else if (value == 0) {
		eng->virt_down[code] = 0;
	}
	if (eng->ops.emit_key) {
		eng->ops.emit_key(eng->ops.userdata, code, value);
	}
}

static int
button_index(unsigned int button)
{
	if (button < BTN_LEFT || button >= BTN_LEFT + MAX_MOUSE_BUTTONS) return -1;
	return (int)(button - BTN_LEFT);
}

static void
emit_virt_button(Engine *eng, unsigned int button, int value)
{
	int idx = button_index(button);
	if (idx >= 0) {
		eng->btn_down[idx] = (value != 0) ? 1 : 0;
	}
	if (eng->ops.emit_button) {
		eng->ops.emit_button(eng->ops.userdata, button, value);
	}
}

unsigned int
keycode_to_modbit(unsigned int code)
{
	switch (code) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		return MOD_SHIFT;
	case KEY_CAPSLOCK:
		return MOD_CAPS;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		return MOD_CTRL;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		return MOD_ALT;
	case KEY_NUMLOCK:
		return MOD_NUMLOCK;
	case KEY_LEFTMETA:
	case KEY_RIGHTMETA:
		return MOD_SUPER;
	case KEY_COMPOSE:
		return MOD_ALTGR;
	default:
		return MOD_NONE;
	}
}

static unsigned int
mods_except(const Engine *eng, unsigned int exclude_code)
{
	unsigned int mods = MOD_NONE;
	for (size_t i = 0; i < ARRAY_LEN(mod_keycodes); i++) {
		unsigned int k = mod_keycodes[i];
		if (k != exclude_code && eng->phys_down[k]) {
			mods |= keycode_to_modbit(k);
		}
	}
	return mods;
}

unsigned int
engine_current_mods(const Engine *eng)
{
	return mods_except(eng, KEY_CNT);
}

static bool
is_bound_local(const Engine *eng, unsigned int code)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if (!(b->opts & BIND_GLOBAL) && b->keycode == code) {
			return true;
		}
	}
	return false;
}

static bool
is_bound_global(const Engine *eng, unsigned int code)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if ((b->opts & BIND_GLOBAL) && b->keycode == code) {
			return true;
		}
	}
	return false;
}

static bool
is_internal_mod_key(const Engine *eng, unsigned int code)
{
	unsigned int bit = keycode_to_modbit(code);
	if (!bit || !(bit & eng->internal_mods)) return false;
	return is_bound_local(eng, code);
}

static bool
key_has_norepeat(const Engine *eng, unsigned int code)
{
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if (b->keycode == code && (b->opts & BIND_NOREPEAT)) {
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

static void
release_non_passthrough_virt_keys(Engine *eng)
{
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (!eng->virt_down[k]) continue;
		if (keycode_to_modbit(k) != MOD_NONE && !is_internal_mod_key(eng, k)) {
			// Keep passthrough modifiers (e.g. Super/Meta) held so
			// Mod + Mouse combinations continue working seamlessly.
			continue;
		}
		emit_virt_key(eng, k, 0);
		if (eng->phys_down[k]) {
			eng->suppress_until_release[k] = 1;
			eng_log(eng, 1, "release & suppress pre-held key %s (%u)",
			        keycode_name(k) ? keycode_name(k) : "unknown", k);
		}
	}
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
	eng->ptr.base_speed = base_speed;
	eng->ptr.mul = 1.0;
	eng->scroll.base_speed = base_scroll;
	eng->scroll.mul = 1.0;
}

const char *
keycode_name(unsigned int code)
{
	switch (code) {
	case KEY_A: return "a";
	case KEY_B: return "b";
	case KEY_C: return "c";
	case KEY_D: return "d";
	case KEY_E: return "e";
	case KEY_F: return "f";
	case KEY_G: return "g";
	case KEY_H: return "h";
	case KEY_I: return "i";
	case KEY_J: return "j";
	case KEY_K: return "k";
	case KEY_L: return "l";
	case KEY_M: return "m";
	case KEY_N: return "n";
	case KEY_O: return "o";
	case KEY_P: return "p";
	case KEY_Q: return "q";
	case KEY_R: return "r";
	case KEY_S: return "s";
	case KEY_T: return "t";
	case KEY_U: return "u";
	case KEY_V: return "v";
	case KEY_W: return "w";
	case KEY_X: return "x";
	case KEY_Y: return "y";
	case KEY_Z: return "z";
	case KEY_0: return "0";
	case KEY_1: return "1";
	case KEY_2: return "2";
	case KEY_3: return "3";
	case KEY_4: return "4";
	case KEY_5: return "5";
	case KEY_6: return "6";
	case KEY_7: return "7";
	case KEY_8: return "8";
	case KEY_9: return "9";
	case KEY_LEFTSHIFT: return "Shift_L";
	case KEY_RIGHTSHIFT: return "Shift_R";
	case KEY_LEFTCTRL: return "Control_L";
	case KEY_RIGHTCTRL: return "Control_R";
	case KEY_LEFTALT: return "Alt_L";
	case KEY_RIGHTALT: return "Alt_R";
	case KEY_LEFTMETA: return "Super_L";
	case KEY_RIGHTMETA: return "Super_R";
	case KEY_CAPSLOCK: return "CapsLock";
	case KEY_NUMLOCK: return "NumLock";
	case KEY_COMPOSE: return "Compose";
	case KEY_SELECT: return "Select";
	case KEY_SPACE: return "space";
	case KEY_SEMICOLON: return "semicolon";
	case KEY_COMMA: return "comma";
	case KEY_DOT: return "period";
	case KEY_SLASH: return "slash";
	case KEY_BACKSLASH: return "backslash";
	case KEY_APOSTROPHE: return "apostrophe";
	case KEY_GRAVE: return "grave";
	case KEY_MINUS: return "minus";
	case KEY_EQUAL: return "equal";
	case KEY_LEFTBRACE: return "bracketleft";
	case KEY_RIGHTBRACE: return "bracketright";
	case KEY_HOME: return "Home";
	case KEY_END: return "End";
	case KEY_PAGEUP: return "PageUp";
	case KEY_PAGEDOWN: return "PageDown";
	case KEY_INSERT: return "Insert";
	case KEY_DELETE: return "Delete";
	case KEY_ENTER: return "Return";
	case KEY_ESC: return "Escape";
	case KEY_TAB: return "Tab";
	case KEY_BACKSPACE: return "BackSpace";
	case KEY_UP: return "Up";
	case KEY_DOWN: return "Down";
	case KEY_LEFT: return "Left";
	case KEY_RIGHT: return "Right";
	case KEY_F1: return "F1";
	case KEY_F2: return "F2";
	case KEY_F3: return "F3";
	case KEY_F4: return "F4";
	case KEY_F5: return "F5";
	case KEY_F6: return "F6";
	case KEY_F7: return "F7";
	case KEY_F8: return "F8";
	case KEY_F9: return "F9";
	case KEY_F10: return "F10";
	case KEY_F11: return "F11";
	case KEY_F12: return "F12";
	default: return NULL;
	}
}

void
format_binding(char *dst, size_t dstlen, unsigned int mod, unsigned int keycode)
{
	if (!dst || dstlen == 0) return;
	size_t off = 0;
	struct {
		unsigned int mask;
		const char *name;
	} mods[] = {
		{MOD_SHIFT, "Shift"},
		{MOD_CAPS, "Caps"},
		{MOD_CTRL, "Control"},
		{MOD_ALT, "Alt"},
		{MOD_NUMLOCK, "NumLock"},
		{MOD_SUPER, "Super"},
		{MOD_ALTGR, "AltGr"},
	};
	for (size_t i = 0; i < ARRAY_LEN(mods); i++) {
		if (!(mod & mods[i].mask)) continue;
		int n = snprintf(dst + off, dstlen - off, "%s+", mods[i].name);
		if (n < 0 || (size_t)n >= dstlen - off) {
			dst[dstlen - 1] = '\0';
			return;
		}
		off += (size_t)n;
	}
	const char *kname = keycode_name(keycode);
	if (kname) {
		snprintf(dst + off, dstlen - off, "%s", kname);
	} else {
		snprintf(dst + off, dstlen - off, "key(0x%x)", keycode);
	}
}

bool
engine_validate_bindings(const Binding *bindings, size_t num_bindings,
                         char *errbuf, size_t errlen)
{
	char desc[128];
	for (size_t i = 0; i < num_bindings; i++) {
		const Binding *a = &bindings[i];
		if (a->keycode >= KEY_CNT) {
			if (errbuf && errlen) {
				snprintf(errbuf, errlen, "invalid keycode 0x%x at index %zu",
				         a->keycode, i);
			}
			return false;
		}
		if (a->mod && !(a->opts & BIND_GLOBAL)) {
			format_binding(desc, sizeof(desc), a->mod, a->keycode);
			if (errbuf && errlen) {
				snprintf(errbuf, errlen, "modified binding without BIND_GLOBAL: %s", desc);
			}
			return false;
		}
		if ((a->opts & BIND_ANY_MOD) && !(a->opts & BIND_GLOBAL)) {
			format_binding(desc, sizeof(desc), a->mod, a->keycode);
			if (errbuf && errlen) {
				snprintf(errbuf, errlen, "BIND_ANY_MOD requires BIND_GLOBAL: %s", desc);
			}
			return false;
		}
		if (a->mod && a->release_fn) {
			format_binding(desc, sizeof(desc), a->mod, a->keycode);
			if (errbuf && errlen) {
				snprintf(errbuf, errlen, "modified binding cannot have release_fn: %s", desc);
			}
			return false;
		}
		for (size_t j = i + 1; j < num_bindings; j++) {
			const Binding *b = &bindings[j];
			if (a->keycode != b->keycode) continue;
			if ((a->opts & BIND_GLOBAL) != (b->opts & BIND_GLOBAL)) continue;
			bool both_any = (a->opts & BIND_GLOBAL) &&
			                (a->opts & BIND_ANY_MOD) && (b->opts & BIND_ANY_MOD);
			bool same_kind_same_mod = (a->mod == b->mod) &&
			                          ((a->opts & BIND_ANY_MOD) == (b->opts & BIND_ANY_MOD));
			if (both_any || same_kind_same_mod) {
				format_binding(desc, sizeof(desc), a->mod, a->keycode);
				if (errbuf && errlen) {
					snprintf(errbuf, errlen, "duplicate binding: %s", desc);
				}
				return false;
			}
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
	if ((dir & DIR_UP) && !(dir & DIR_DOWN)) {
		m->dir = (m->dir & ~DIR_DOWN) | DIR_UP;
	} else if ((dir & DIR_DOWN) && !(dir & DIR_UP)) {
		m->dir = (m->dir & ~DIR_UP) | DIR_DOWN;
	}
	if (dir & (DIR_LEFT | DIR_RIGHT)) {
		m->x_rem = 0.0;
		m->x_cont = false;
	}
	if ((dir & DIR_LEFT) && !(dir & DIR_RIGHT)) {
		m->dir = (m->dir & ~DIR_RIGHT) | DIR_LEFT;
	} else if ((dir & DIR_RIGHT) && !(dir & DIR_LEFT)) {
		m->dir = (m->dir & ~DIR_LEFT) | DIR_RIGHT;
	}
}

static void
stop_dir(MotionTrack *m, unsigned int dir)
{
	m->dir &= ~dir;
}

PointerDelta
pointer_tick(MotionTrack *m, int usec)
{
	PointerDelta pd = {0, 0};
	if (m->dir == DIR_NONE || usec <= 0) return pd;

	double xsign = ((m->dir & DIR_RIGHT) ? 1.0 : 0.0) - ((m->dir & DIR_LEFT) ? 1.0 : 0.0);
	double ysign = ((m->dir & DIR_DOWN) ? 1.0 : 0.0) - ((m->dir & DIR_UP) ? 1.0 : 0.0);
	double step = m->base_speed * m->mul * ((double)usec / 1e6);

	double dx = step * xsign + m->x_rem;
	double dy = step * ysign + m->y_rem;
	double int_part = 0.0;

	m->x_rem = modf(dx, &int_part);
	pd.dx = (int)int_part;

	m->y_rem = modf(dy, &int_part);
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

	double advance = base_speed * mul * ((double)usec / 1e6);
	double pos = *rem + advance;
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
		int prev_units = (int)llround(prev_pos * HIRES_PER_NOTCH);
		int new_units = (int)llround(pos * HIRES_PER_NOTCH);
		hires = new_units - prev_units;
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
	if ((sd.x_discrete || sd.y_discrete || sd.x_hires || sd.y_hires) &&
	    eng->ops.emit_scroll) {
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
	// Pass 1: exact modifier match (takes precedence over BIND_ANY_MOD fallback).
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if (!(b->opts & BIND_GLOBAL) || (b->opts & BIND_ANY_MOD) || b->keycode != code) continue;
		if (skip_activate && b->press_fn == cmd_activate) continue;
		if (mods_match(mods, b->mod)) return (int)i;
	}
	// Pass 2: BIND_ANY_MOD fallback.
	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if (!(b->opts & BIND_GLOBAL) || !(b->opts & BIND_ANY_MOD) || b->keycode != code) continue;
		if (skip_activate && b->press_fn == cmd_activate) continue;
		return (int)i;
	}
	return -1;
}

void
engine_process_key(Engine *eng, unsigned int code, int value)
{
	if (code >= KEY_CNT) return;

	const char *kname = keycode_name(code) ? keycode_name(code) : "unknown";

	if (value == 2) {
		if (!eng->active && eng->virt_down[code] && !key_has_norepeat(eng, code)) {
			eng_log(eng, 2, "key %s (%u) val=2 -> forward repeat", kname, code);
			emit_virt_key(eng, code, 2);
		} else {
			eng_log(eng, 2, "key %s (%u) val=2 -> suppressed (active=%d)",
			        kname, code, eng->active ? 1 : 0);
		}
		return;
	}

	if (value == 0) {
		char modbuf[64];
		format_mods(mods_except(eng, code), modbuf, sizeof(modbuf));
		eng_log(eng, 1, "in key %s (%u) val=0 active=%d mods=%s",
		        kname, code, eng->active ? 1 : 0, modbuf);

		eng->phys_down[code] = 0;
		if (eng->virt_down[code]) {
			emit_virt_key(eng, code, 0);
		}
		if (eng->suppress_until_release[code]) {
			eng->suppress_until_release[code] = 0;
			eng_log(eng, 2, "cleared suppress on %s (%u)", kname, code);
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
			for (size_t i = 0; i < eng->num_bindings; i++) {
				const Binding *b = &eng->bindings[i];
				if ((b->opts & BIND_GLOBAL) || b->mod != MOD_NONE || b->keycode != code) {
					continue;
				}
				if (b->release_fn) {
					b->release_fn(eng, &b->release_arg);
				}
				return;
			}
		}
		return;
	}

	if (value != 1) return;

	eng->phys_down[code] = 1;
	unsigned int mods = mods_except(eng, code);
	char modbuf[64];
	format_mods(mods, modbuf, sizeof(modbuf));
	eng_log(eng, 1, "in key %s (%u) val=1 active=%d mods=%s",
	        kname, code, eng->active ? 1 : 0, modbuf);

	if (!eng->active) {
		int idx = find_global_binding(eng, code, mods, false);
		if (idx >= 0) {
			const Binding *b = &eng->bindings[idx];
			if (b->release_fn) {
				eng->active_release_idx[code] = idx + 1;
			} else {
				eng->suppress_until_release[code] = 1;
			}
			if (b->press_fn) {
				b->press_fn(eng, &b->press_arg);
			}
			return;
		}
		emit_virt_key(eng, code, 1);
		return;
	}

	if (eng->suppress_until_release[code]) {
		eng_log(eng, 2, "key %s (%u) suppressed until release", kname, code);
		return;
	}

	int gidx = find_global_binding(eng, code, mods, true);
	if (gidx >= 0) {
		const Binding *b = &eng->bindings[gidx];
		if (b->release_fn) {
			eng->active_release_idx[code] = gidx + 1;
		} else {
			eng->suppress_until_release[code] = 1;
		}
		if (b->press_fn) {
			b->press_fn(eng, &b->press_arg);
		}
		return;
	}

	bool passed_mod = false;
	if (keycode_to_modbit(code) != MOD_NONE &&
	    !is_internal_mod_key(eng, code) &&
	    !is_bound_global(eng, code)) {
		emit_virt_key(eng, code, 1);
		passed_mod = true;
	}

	for (size_t i = 0; i < eng->num_bindings; i++) {
		const Binding *b = &eng->bindings[i];
		if ((b->opts & BIND_GLOBAL) || b->mod != MOD_NONE || b->keycode != code) {
			continue;
		}
		if (b->press_fn) {
			b->press_fn(eng, &b->press_arg);
		}
		return;
	}

	if (!passed_mod) {
		eng_log(eng, 2, "swallowed unbound key %s (%u) while active", kname, code);
	}
}

void
cmd_activate(Engine *eng, const Arg *arg)
{
	eng->active = true;
	eng_log(eng, 1, "state: active=1 (pointer mode enabled)");
	release_non_passthrough_virt_keys(eng);
	if (arg && arg->u > 0 && arg->u < KEY_CNT && eng->phys_down[arg->u]) {
		eng->suppress_until_release[arg->u] = 1;
	}
}

void
cmd_deactivate(Engine *eng, const Arg *arg)
{
	(void)arg;
	int released_btns = 0;
	for (unsigned int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
		if (eng->btn_down[i]) {
			emit_virt_button(eng, BTN_LEFT + i, 0);
			released_btns++;
		}
	}
	eng->active = false;
	cmd_reset(eng, NULL);
	eng_log(eng, 1, "state: active=0 (pointer mode disabled, released_btns=%d)", released_btns);
}

void
cmd_toggle_active(Engine *eng, const Arg *arg)
{
	(void)arg;
	if (eng->active) {
		cmd_deactivate(eng, NULL);
	} else {
		cmd_activate(eng, NULL);
	}
}

void
cmd_activate_scroll(Engine *eng, const Arg *arg)
{
	(void)arg;
	cmd_activate(eng, NULL);
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (eng->virt_down[k] && (keycode_to_modbit(k) & eng->internal_mods)) {
			emit_virt_key(eng, k, 0);
			if (eng->phys_down[k]) {
				eng->suppress_until_release[k] = 1;
			}
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
	char dbuf[16];
	format_dir(eng->ptr.dir, dbuf, sizeof(dbuf));
	eng_log(eng, 1, "motion: dir=%s mul=%.3g scroll_mode=%d",
	        dbuf, eng->ptr.mul, eng->move_is_scroll ? 1 : 0);
}

void
cmd_move_stop(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	unsigned int prev = eng->ptr.dir;
	stop_dir(&eng->ptr, arg->u);
	if (eng->ptr.dir != prev) {
		char dbuf[16];
		format_dir(eng->ptr.dir, dbuf, sizeof(dbuf));
		eng_log(eng, 1, "motion: dir=%s mul=%.3g scroll_mode=%d",
		        dbuf, eng->ptr.mul, eng->move_is_scroll ? 1 : 0);
	}
}

void
cmd_scroll_start(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	start_dir(&eng->scroll, arg->u);
	char dbuf[16];
	format_dir(eng->scroll.dir, dbuf, sizeof(dbuf));
	eng_log(eng, 1, "scroll: dir=%s mul=%.3g", dbuf, eng->scroll.mul);
}

void
cmd_scroll_stop(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	unsigned int prev = eng->scroll.dir;
	stop_dir(&eng->scroll, arg->u);
	if (eng->scroll.dir != prev) {
		char dbuf[16];
		format_dir(eng->scroll.dir, dbuf, sizeof(dbuf));
		eng_log(eng, 1, "scroll: dir=%s mul=%.3g", dbuf, eng->scroll.mul);
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
	if (!arg) return;
	emit_virt_button(eng, arg->u, 1);
}

void
cmd_click_release(Engine *eng, const Arg *arg)
{
	if (!arg) return;
	emit_virt_button(eng, arg->u, 0);
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
