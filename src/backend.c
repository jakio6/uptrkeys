#include "uptrkeys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Kernel input/output backend.
 *
 * Security & side-effect boundaries (all statically verifiable via `make audit`):
 *  - Zero heap allocations (fixed MAX_KEYBOARDS table).
 *  - Zero network, process-spawning, environment, or config-file access.
 *  - Opens ONLY:
 *      * /dev/uinput (or /dev/input/uinput) O_WRONLY for virtual kbd + pointer.
 *      * /dev/input/event* O_RDONLY (S_ISCHR verified via lstat + fstat, O_NOFOLLOW).
 *      * /dev/input directory via opendir + inotify_add_watch.
 *  - Never logs keystrokes unless -d / -dd is explicitly passed by the user.
 */

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)
#define SET_BIT(bit, arr) ((arr)[(bit) / BITS_PER_LONG] |= (1UL << ((bit) % BITS_PER_LONG)))
#define CLR_BIT(bit, arr) ((arr)[(bit) / BITS_PER_LONG] &= ~(1UL << ((bit) % BITS_PER_LONG)))

#define MAX_KEYBOARDS 32
#define DEV_PATH_MAX 300
#define GRAB_WAIT_RELEASE_MS 500
#define VDEV_KBD_NAME "uptrkeys virtual keyboard"
#define VDEV_PTR_NAME "uptrkeys virtual pointer"

int debug_level = 0;

typedef struct {
	int fd;
	dev_t dev;
	ino_t ino;
	char path[DEV_PATH_MAX];
	unsigned long down[NBITS(KEY_CNT)];
} KbdDevice;

typedef struct {
	int uinput_kbd_fd;
	int uinput_ptr_fd;
	int inotify_fd;
	struct timespec start_ts;
	KbdDevice devs[MAX_KEYBOARDS];
	size_t num_devs;
	size_t perm_denied;
	int first_denied_err;
	char first_denied_path[DEV_PATH_MAX];
	int scan_dir_err;
} Backend;

static double
uptime_sec(const Backend *b)
{
	if (!b || (b->start_ts.tv_sec == 0 && b->start_ts.tv_nsec == 0)) return 0.0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - b->start_ts.tv_sec) +
	       (double)(now.tv_nsec - b->start_ts.tv_nsec) / 1e9;
}

#define logf_lvl(b, lvl, ...) do { \
	if (debug_level >= (lvl)) { \
		fprintf(stderr, "[uptrkeys +%7.3fs] ", uptime_sec(b)); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} \
} while (0)

static void
backend_log(void *userdata, int level, const char *msg)
{
	const Backend *b = userdata;
	if (debug_level < level) return;
	fprintf(stderr, "[uptrkeys +%7.3fs] %s\n", uptime_sec(b), msg);
}

static const char *
button_name(unsigned int button)
{
	switch (button) {
	case BTN_LEFT: return "BTN_LEFT";
	case BTN_RIGHT: return "BTN_RIGHT";
	case BTN_MIDDLE: return "BTN_MIDDLE";
	case BTN_SIDE: return "BTN_SIDE";
	case BTN_EXTRA: return "BTN_EXTRA";
	default: return "BTN_MOUSE";
	}
}

static void
msleep(long ms)
{
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000L,
	};
	nanosleep(&ts, NULL);
}

static void
write_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev = {0};
	ev.type = type;
	ev.code = code;
	ev.value = value;
	ssize_t n = write(fd, &ev, sizeof(ev));
	(void)n;
}

static void
backend_emit_key(void *userdata, unsigned int keycode, int value)
{
	Backend *b = userdata;
	if (b->uinput_kbd_fd < 0) return;
	logf_lvl(b, (value == 2) ? 2 : 1, "out kbd: %s (%u) val=%d",
	         keycode_name(keycode) ? keycode_name(keycode) : "unknown", keycode, value);
	write_event(b->uinput_kbd_fd, EV_KEY, (uint16_t)keycode, value);
	write_event(b->uinput_kbd_fd, EV_SYN, SYN_REPORT, 0);
}

static void
backend_emit_button(void *userdata, unsigned int button, int value)
{
	Backend *b = userdata;
	if (b->uinput_ptr_fd < 0) return;
	logf_lvl(b, 1, "out btn: %s (0x%x) val=%d", button_name(button), button, value);
	write_event(b->uinput_ptr_fd, EV_KEY, (uint16_t)button, value);
	write_event(b->uinput_ptr_fd, EV_SYN, SYN_REPORT, 0);
}

static void
backend_emit_motion(void *userdata, int dx, int dy)
{
	Backend *b = userdata;
	if (b->uinput_ptr_fd < 0 || (!dx && !dy)) return;
	logf_lvl(b, 2, "out motion: dx=%d dy=%d", dx, dy);
	if (dx) write_event(b->uinput_ptr_fd, EV_REL, REL_X, dx);
	if (dy) write_event(b->uinput_ptr_fd, EV_REL, REL_Y, dy);
	write_event(b->uinput_ptr_fd, EV_SYN, SYN_REPORT, 0);
}

static void
backend_emit_scroll(void *userdata, ScrollDelta sd)
{
	Backend *b = userdata;
	if (b->uinput_ptr_fd < 0) return;
	bool sent = false;
	if (sd.y_hires) {
		write_event(b->uinput_ptr_fd, EV_REL, REL_WHEEL_HI_RES, sd.y_hires);
		sent = true;
	}
	if (sd.y_discrete) {
		write_event(b->uinput_ptr_fd, EV_REL, REL_WHEEL, sd.y_discrete);
		sent = true;
	}
	if (sd.x_hires) {
		write_event(b->uinput_ptr_fd, EV_REL, REL_HWHEEL_HI_RES, sd.x_hires);
		sent = true;
	}
	if (sd.x_discrete) {
		write_event(b->uinput_ptr_fd, EV_REL, REL_HWHEEL, sd.x_discrete);
		sent = true;
	}
	if (sent) {
		logf_lvl(b, 2, "out scroll: disc=(%d,%d) hires=(%d,%d)",
		         sd.x_discrete, sd.y_discrete, sd.x_hires, sd.y_hires);
		write_event(b->uinput_ptr_fd, EV_SYN, SYN_REPORT, 0);
	}
}

static int
open_uinput_node(void)
{
	int flags = O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW;
	int fd = open("/dev/uinput", flags);
	if (fd < 0 && errno == ENOENT) {
		fd = open("/dev/input/uinput", flags);
	}
	if (fd < 0) return -1;

	struct stat st;
	if (fstat(fd, &st) == -1 || !S_ISCHR(st.st_mode)) {
		close(fd);
		errno = ENODEV;
		return -1;
	}
	return fd;
}

static int
finalize_uinput_device(int fd, const char *name, uint16_t product_id)
{
	struct uinput_setup setup = {0};
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x1d6b;
	setup.id.product = product_id;
	setup.id.version = 1;
	snprintf(setup.name, sizeof(setup.name), "%s", name);

	if (ioctl(fd, UI_DEV_SETUP, &setup) == -1) {
		if (errno != EINVAL) return -1;
		struct uinput_user_dev udev = {0};
		udev.id = setup.id;
		snprintf(udev.name, sizeof(udev.name), "%s", name);
		if (write(fd, &udev, sizeof(udev)) != (ssize_t)sizeof(udev)) {
			return -1;
		}
	}
	if (ioctl(fd, UI_DEV_CREATE) == -1) return -1;
	return 0;
}

static int
create_virtual_keyboard(void)
{
	int fd = open_uinput_node();
	if (fd < 0) return -1;

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) == -1 ||
	    ioctl(fd, UI_SET_EVBIT, EV_SYN) == -1) {
		close(fd);
		return -1;
	}

	// Register standard keyboard keys while excluding BTN_* ranges
	// (0x100..0x15f, 0x220..0x22f, 0x2c0..0x2ff) so udev/libinput classifies
	// this virtual device strictly as a keyboard.
	for (int k = 1; k <= 0xff; k++) {
		ioctl(fd, UI_SET_KEYBIT, k);
	}
	for (int k = 0x160; k <= 0x21f; k++) {
		ioctl(fd, UI_SET_KEYBIT, k);
	}
	for (int k = 0x230; k <= 0x2bf; k++) {
		ioctl(fd, UI_SET_KEYBIT, k);
	}

	if (finalize_uinput_device(fd, VDEV_KBD_NAME, 0x0001) == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
create_virtual_pointer(void)
{
	int fd = open_uinput_node();
	if (fd < 0) return -1;

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) == -1 ||
	    ioctl(fd, UI_SET_EVBIT, EV_REL) == -1 ||
	    ioctl(fd, UI_SET_EVBIT, EV_SYN) == -1) {
		close(fd);
		return -1;
	}

	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);

	static const int buttons[] = {
		BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
	};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		ioctl(fd, UI_SET_KEYBIT, buttons[i]);
	}

	static const int rel_axes[] = {
		REL_X, REL_Y,
		REL_WHEEL, REL_HWHEEL,
		REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES,
	};
	for (size_t i = 0; i < sizeof(rel_axes) / sizeof(rel_axes[0]); i++) {
		ioctl(fd, UI_SET_RELBIT, rel_axes[i]);
	}

	if (finalize_uinput_device(fd, VDEV_PTR_NAME, 0x0002) == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool
is_keyboard_device(const Backend *b, int fd, const char *path, char *name_out, size_t name_len)
{
	char name[256] = {0};
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
		snprintf(name, sizeof(name), "unknown");
	}
	if (name_out && name_len) {
		snprintf(name_out, name_len, "%s", name);
	}
	if (strncmp(name, "uptrkeys virtual ", 17) == 0) {
		logf_lvl(b, 2, "skip %s (\"%s\"): own virtual device", path, name);
		return false;
	}

	unsigned long ev_bits[NBITS(EV_CNT)] = {0};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !TEST_BIT(EV_KEY, ev_bits)) {
		logf_lvl(b, 2, "skip %s (\"%s\"): no EV_KEY", path, name);
		return false;
	}

	unsigned long key_bits[NBITS(KEY_CNT)] = {0};
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return false;

	static const unsigned int required_keys[] = {
		KEY_Q, KEY_W, KEY_E, KEY_R,
		KEY_A, KEY_S, KEY_D, KEY_F,
	};
	for (size_t i = 0; i < sizeof(required_keys) / sizeof(required_keys[0]); i++) {
		if (!TEST_BIT(required_keys[i], key_bits)) {
			logf_lvl(b, 2, "skip %s (\"%s\"): missing alpha keys", path, name);
			return false;
		}
	}
	return true;
}

static void
wait_keys_released(int fd, int max_ms)
{
	unsigned long key_state[NBITS(KEY_CNT)] = {0};
	for (int waited = 0; waited < max_ms; waited += 10) {
		memset(key_state, 0, sizeof(key_state));
		if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) < 0) break;
		bool any_down = false;
		for (size_t i = 0; i < NBITS(KEY_CNT); i++) {
			if (key_state[i] != 0) {
				any_down = true;
				break;
			}
		}
		if (!any_down) return;
		msleep(10);
	}
}

static bool
device_already_open(const Backend *b, dev_t dev, ino_t ino)
{
	for (size_t i = 0; i < b->num_devs; i++) {
		if (b->devs[i].dev == dev && b->devs[i].ino == ino) {
			return true;
		}
	}
	return false;
}

static void
try_open_keyboard(Backend *b, const char *path)
{
	struct stat lst;
	if (lstat(path, &lst) == -1 || !S_ISCHR(lst.st_mode)) return;
	if (device_already_open(b, lst.st_dev, lst.st_ino)) return;
	if (b->num_devs >= MAX_KEYBOARDS) {
		logf_lvl(b, 1, "skip %s: max keyboard limit (%d) reached", path, MAX_KEYBOARDS);
		return;
	}

	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM) {
			if (b->perm_denied == 0) {
				b->first_denied_err = errno;
				snprintf(b->first_denied_path, sizeof(b->first_denied_path), "%s", path);
			}
			b->perm_denied++;
			logf_lvl(b, 1, "permission denied opening %s: %s", path, strerror(errno));
		} else {
			logf_lvl(b, 2, "skip %s: %s", path, strerror(errno));
		}
		return;
	}

	struct stat fst;
	if (fstat(fd, &fst) == -1 || !S_ISCHR(fst.st_mode) ||
	    fst.st_dev != lst.st_dev || fst.st_ino != lst.st_ino) {
		close(fd);
		return;
	}

	char name[256] = {0};
	if (!is_keyboard_device(b, fd, path, name, sizeof(name))) {
		close(fd);
		return;
	}

	wait_keys_released(fd, GRAB_WAIT_RELEASE_MS);
	if (ioctl(fd, EVIOCGRAB, 1) == -1) {
		logf_lvl(b, 1, "grab failed on %s (\"%s\"): %s", path, name, strerror(errno));
		close(fd);
		return;
	}

	KbdDevice *d = &b->devs[b->num_devs++];
	d->fd = fd;
	d->dev = fst.st_dev;
	d->ino = fst.st_ino;
	memset(d->down, 0, sizeof(d->down));
	snprintf(d->path, sizeof(d->path), "%s", path);
	logf_lvl(b, 1, "grabbed keyboard %s (\"%s\")", d->path, name);
}

static void
remove_device_at(Backend *b, Engine *eng, size_t idx)
{
	if (idx >= b->num_devs) return;
	logf_lvl(b, 1, "unplugged keyboard %s", b->devs[idx].path);
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (TEST_BIT(k, b->devs[idx].down)) {
			CLR_BIT(k, b->devs[idx].down);
			if (eng) engine_process_key(eng, k, 0);
		}
	}
	ioctl(b->devs[idx].fd, EVIOCGRAB, 0);
	close(b->devs[idx].fd);
	if (idx + 1 < b->num_devs) {
		b->devs[idx] = b->devs[b->num_devs - 1];
	}
	b->num_devs--;
}

static void
remove_device_by_path(Backend *b, Engine *eng, const char *path)
{
	for (size_t i = 0; i < b->num_devs; i++) {
		if (strcmp(b->devs[i].path, path) == 0) {
			remove_device_at(b, eng, i);
			return;
		}
	}
}

static void
scan_input_dir(Backend *b)
{
	DIR *dir = opendir("/dev/input");
	if (!dir) {
		b->scan_dir_err = errno;
		fprintf(stderr, "uptrkeys: open /dev/input: %s\n", strerror(errno));
		return;
	}
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "event", 5) != 0) continue;
		char path[DEV_PATH_MAX];
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		try_open_keyboard(b, path);
	}
	closedir(dir);
}

static void
handle_inotify(Backend *b, Engine *eng)
{
	_Alignas(struct inotify_event) char buf[4096];
	for (;;) {
		ssize_t n = read(b->inotify_fd, buf, sizeof(buf));
		if (n <= 0) break;
		for (ssize_t off = 0; off < n;) {
			const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
			if (ev->len > 0 && strncmp(ev->name, "event", 5) == 0) {
				char path[DEV_PATH_MAX];
				snprintf(path, sizeof(path), "/dev/input/%s", ev->name);
				if (ev->mask & (IN_CREATE | IN_ATTRIB)) {
					try_open_keyboard(b, path);
				} else if (ev->mask & IN_DELETE) {
					remove_device_by_path(b, eng, path);
				}
			}
			off += (ssize_t)sizeof(*ev) + (ssize_t)ev->len;
		}
	}
}

static void
release_held_virtual_outputs(Backend *b, Engine *eng)
{
	for (unsigned int k = 0; k < KEY_CNT; k++) {
		if (eng->virt_down[k]) {
			backend_emit_key(b, k, 0);
			eng->virt_down[k] = 0;
		}
	}
	for (unsigned int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
		if (eng->btn_down[i]) {
			backend_emit_button(b, BTN_LEFT + i, 0);
			eng->btn_down[i] = 0;
		}
	}
}

static void
backend_cleanup(Backend *b, Engine *eng)
{
	release_held_virtual_outputs(b, eng);
	for (size_t i = 0; i < b->num_devs; i++) {
		ioctl(b->devs[i].fd, EVIOCGRAB, 0);
		close(b->devs[i].fd);
	}
	b->num_devs = 0;

	if (b->inotify_fd >= 0) close(b->inotify_fd);
	if (b->uinput_kbd_fd >= 0) {
		ioctl(b->uinput_kbd_fd, UI_DEV_DESTROY);
		close(b->uinput_kbd_fd);
	}
	if (b->uinput_ptr_fd >= 0) {
		ioctl(b->uinput_ptr_fd, UI_DEV_DESTROY);
		close(b->uinput_ptr_fd);
	}
}

static int
elapsed_usec(struct timespec *prev)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long sec = (long)(now.tv_sec - prev->tv_sec);
	long nsec = now.tv_nsec - prev->tv_nsec;
	*prev = now;
	long long us = (long long)sec * 1000000LL + nsec / 1000LL;
	if (us < 0) return 0;
	if (us > 1000000LL) return 1000000;
	return (int)us;
}

int
backend_run(Engine *eng, int fps, const volatile sig_atomic_t *stop_flag)
{
	Backend b = {
		.uinput_kbd_fd = -1,
		.uinput_ptr_fd = -1,
		.inotify_fd = -1,
	};
	clock_gettime(CLOCK_MONOTONIC, &b.start_ts);
	eng->debug_level = debug_level;

	logf_lvl(&b, 1, "starting (fps=%d base_speed=%.0f base_scroll=%.1f internal_mods=0x%x bindings=%zu debug=%d)",
	         fps, eng->base_speed, eng->base_scroll, eng->internal_mods, eng->num_bindings, debug_level);

	b.uinput_kbd_fd = create_virtual_keyboard();
	if (b.uinput_kbd_fd < 0) {
		fprintf(stderr, "uptrkeys: open /dev/uinput for keyboard failed: %s\n"
		                "Ensure the uinput kernel module is loaded and you have read/write\n"
		                "permissions on /dev/uinput and /dev/input/event*.\n",
		        strerror(errno));
		return 1;
	}

	b.uinput_ptr_fd = create_virtual_pointer();
	if (b.uinput_ptr_fd < 0) {
		fprintf(stderr, "uptrkeys: open /dev/uinput for pointer failed: %s\n",
		        strerror(errno));
		backend_cleanup(&b, eng);
		return 1;
	}
	logf_lvl(&b, 1, "virtual devices ready: \"%s\", \"%s\"", VDEV_KBD_NAME, VDEV_PTR_NAME);

	b.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (b.inotify_fd < 0 ||
	    inotify_add_watch(b.inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
		fprintf(stderr, "uptrkeys: warning: inotify watch on /dev/input failed: %s "
		                "(hotplug disabled)\n", strerror(errno));
		if (b.inotify_fd >= 0) {
			close(b.inotify_fd);
			b.inotify_fd = -1;
		}
	}

	eng->ops = (EngineOps){
		.emit_key = backend_emit_key,
		.emit_button = backend_emit_button,
		.emit_motion = backend_emit_motion,
		.emit_scroll = backend_emit_scroll,
		.log_msg = backend_log,
		.userdata = &b,
	};

	scan_input_dir(&b);
	if (b.num_devs == 0) {
		if (b.scan_dir_err == EACCES || b.scan_dir_err == EPERM) {
			fprintf(stderr, "uptrkeys: error: cannot read /dev/input (%s)\n",
			        strerror(b.scan_dir_err));
			backend_cleanup(&b, eng);
			return 1;
		}
		if (b.perm_denied > 0) {
			fprintf(stderr,
			        "uptrkeys: error: permission denied on %zu /dev/input/event* device(s) "
			        "(e.g. %s: %s).\n"
			        "Ensure the user/service running uptrkeys is in the 'input' group\n"
			        "(log out and back in after usermod -aG input, or set SupplementaryGroups=input).\n",
			        b.perm_denied, b.first_denied_path, strerror(b.first_denied_err));
			backend_cleanup(&b, eng);
			return 1;
		}
		fprintf(stderr, "uptrkeys: warning: no keyboards found in /dev/input/event* "
		                "(waiting for hotplug)\n");
	}

	struct timespec last_tick;
	clock_gettime(CLOCK_MONOTONIC, &last_tick);

	struct pollfd pfds[MAX_KEYBOARDS + 1];

	while (!eng->quitting && (!stop_flag || !*stop_flag)) {
		nfds_t nfds = 0;
		int inotify_slot = -1;
		if (b.inotify_fd >= 0) {
			inotify_slot = (int)nfds;
			pfds[nfds++] = (struct pollfd){.fd = b.inotify_fd, .events = POLLIN};
		}
		size_t dev_start = nfds;
		for (size_t i = 0; i < b.num_devs; i++) {
			pfds[nfds++] = (struct pollfd){.fd = b.devs[i].fd, .events = POLLIN};
		}

		bool was_moving = engine_is_moving(eng);
		int timeout_ms = was_moving ? (1000 / (fps > 0 ? fps : 60)) : -1;
		if (timeout_ms == 0) timeout_ms = 1;

		int rc = poll(pfds, nfds, timeout_ms);
		if (rc < 0) {
			if (errno == EINTR) continue;
			fprintf(stderr, "uptrkeys: poll: %s\n", strerror(errno));
			break;
		}

		// Read keyboard events before handling inotify so b.devs matches pfds indices.
		for (size_t i = b.num_devs; i > 0; i--) {
			size_t idx = i - 1;
			short rev = pfds[dev_start + idx].revents;
			if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
				remove_device_at(&b, eng, idx);
				continue;
			}
			if (!(rev & POLLIN)) continue;

			struct input_event evs[32];
			bool unplugged = false;
			for (;;) {
				ssize_t n = read(b.devs[idx].fd, evs, sizeof(evs));
				if (n < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) break;
					unplugged = true;
					break;
				}
				if (n == 0) {
					unplugged = true;
					break;
				}
				size_t count = (size_t)n / sizeof(evs[0]);
				for (size_t j = 0; j < count; j++) {
					if (evs[j].type == EV_KEY && evs[j].code < KEY_CNT) {
						if (evs[j].value == 1) {
							SET_BIT(evs[j].code, b.devs[idx].down);
						} else if (evs[j].value == 0) {
							CLR_BIT(evs[j].code, b.devs[idx].down);
						}
						engine_process_key(eng, evs[j].code, evs[j].value);
					}
				}
			}
			if (unplugged) {
				remove_device_at(&b, eng, idx);
			}
		}

		if (inotify_slot >= 0 && (pfds[inotify_slot].revents & POLLIN)) {
			handle_inotify(&b, eng);
		}

		if (!was_moving && engine_is_moving(eng)) {
			clock_gettime(CLOCK_MONOTONIC, &last_tick);
			engine_tick(eng, 1);
		} else if (engine_is_moving(eng)) {
			int usec = elapsed_usec(&last_tick);
			engine_tick(eng, usec);
		}
	}

	logf_lvl(&b, 1, "shutting down (quitting=%d stop_signal=%d)",
	         eng->quitting ? 1 : 0, (stop_flag && *stop_flag) ? 1 : 0);
	backend_cleanup(&b, eng);
	return 0;
}
