# uptrkeys — Smooth Kernel-Level Mouse Keys for X11 & Wayland

`uptrkeys` is a minimal C11 daemon that turns your keyboard into a smooth,
chorded pointer and scroll device on Linux using `evdev` and `uinput`, with
physical `CapsLock` acting as the hold-to-activate prefix layer key.

Because it operates at the kernel input layer rather than X11 grabs:

- **Works everywhere**: Identical behavior on X11 (dwm, i3, bspwm, GNOME, KDE),
  Wayland (Sway, Hyprland, River, GNOME, KDE), and TTY/KMS apps.
- **Physical `CapsLock` prefix layer**: Hold `CapsLock` to move, click, scroll,
  and scale speed; release `CapsLock` to return immediately to normal typing.
  No `xmodmap` or `xkb_options` remapping needed, and `CapsLock` state never
  toggles.
- **Native `Mod + Mouse` combinations**: Hold `Super`, `Shift`, or right-side
  `Ctrl`/`Alt` **before or during** holding `CapsLock` to perform `Super +
  LeftClick` / `Super + RightClick` window move/resize, `Shift + Click`,
  `Ctrl + Click`, `Ctrl + Scroll`, etc. — just like a physical mouse. No tiling
  WM hotkeys are stolen.
- **High-resolution smooth scrolling**: Emits both `REL_WHEEL_HI_RES` /
  `REL_HWHEEL_HI_RES` (`120` units/notch) and discrete `REL_WHEEL` / `REL_HWHEEL`.
- **Automatic keyboard hotplugging**: Monitors `/dev/input` with `inotify`.

## Project Layout & Auditability

The production code is intentionally tiny and split into 4 files in `src/` plus
`config.def.h`:

- `src/uptrkeys.h` — Shared structs and declarations.
- `src/main.c` — CLI options, binding validation, `SIGINT`/`SIGTERM` flags.
- `src/engine.c` — Pure state machine and kinematics (**zero OS syscalls, zero heap**).
- `src/backend.c` — Kernel I/O (**zero heap**, **zero network/exec/env/config I/O**,
  opens only `/dev/uinput` `O_WRONLY` and `/dev/input/event*` `O_RDONLY` with
  `O_NOFOLLOW | O_CLOEXEC` and `S_ISCHR` checks, and never logs keystrokes unless
  `-d` or `-dd` is passed).
- `tests/` — Dependency-free unit tests (`unit_test.c`) and ELF symbol/path
  audit (`audit.sh`).

## Building, Auditing & Testing

```sh
make
make audit     # verifies ELF dynamic symbol allowlist & path allowlist
make check     # runs unit tests + audit
make sanitize  # runs unit tests under ASan + UBSan
sudo make install
```

Requirements:
- Linux kernel headers (`linux/input.h`, `linux/uinput.h`)
- C11 compiler (`gcc` or `clang`) and GNU `make`
- Optional: `valgrind` (`make check`)

## Permissions

`uptrkeys` needs read access to `/dev/input/event*` and write access to
`/dev/uinput`. Typically you add your user to the `input` group and create a
udev rule for `/dev/uinput`:

```sh
sudo usermod -aG input "$USER"
echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"' | \
  sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger /dev/uinput
```

Log out and back in for group changes to apply. Alternatively, run `uptrkeys`
under a dedicated `uptrkeys` systemd service (`SupplementaryGroups=input`) so
your login user needs no `input`/`uinput` privileges.

## Configuration

Customize bindings, speeds (`BASE_SPEED`, `BASE_SCROLL`), frame rate (`FPS`),
and `internal_mods` in `config.h` (copied from `config.def.h` on first build),
then rebuild with `make`.

### `internal_mods` vs `Mod + Mouse`

`internal_mods` (default `MOD_SHIFT | MOD_CTRL | MOD_ALT`) prevents pointer
control modifiers that are bound in `bindings[]` (`KEY_LEFTCTRL`,
`KEY_LEFTALT`), or used as a global scroll-mode chord (`Shift+CapsLock`), from
leaking to applications. Any modifier not listed in `internal_mods` (such as
`MOD_SUPER`) and any modifier key not bound in `bindings[]` (such as
`KEY_LEFTSHIFT`, `KEY_RIGHTSHIFT`, `KEY_RIGHTCTRL`, `KEY_RIGHTALT`) passes
straight through to the virtual keyboard, allowing normal `Mod + Click` /
`Mod + Drag` / `Mod + Scroll`.

## Default Bindings

- **Enable/Disable**: Hold physical `CapsLock` (release to exit), `Shift+CapsLock`
  (sticky scroll mode), `q` (deactivate), `x` (quit)
- **Move**: `w`, `a`, `s`, `d`
- **Scroll**: hold `Tab` or toggle `f`
- **Multipliers**: `LeftAlt` (`/8`), `LeftCtrl` (`*32`), `j` (`/8`), `k` (`/2`),
  `l` (`*4`), `;` (`*8`)
- **Click**: `Space` (left), `e`/`n` (right), `r`/`m` (middle)

## Debugging

- `uptrkeys -d` (or `--debug`, `--verbose`): logs keyboard grabs/unplugs, key events, `CapsLock` prefix transitions, speed/scroll toggles, and button/key emissions with timestamps on `stderr`.
- `uptrkeys -dd` (or `--trace`): also logs skipped `/dev/input/event*` devices, autorepeat suppression, and every 120Hz motion/scroll tick.
