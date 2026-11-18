# uptrkeys — Smooth Kernel-Level Mouse Keys for X11 & Wayland

> **Disclaimer:** `uptrkeys` is a **vibecoded** (AI-generated) from-scratch C11
> rewrite inspired by [ptrkeys](https://github.com/torbiak/ptrkeys).
> - **Only tested in an X11 environment** so far — Wayland support is based on
>   using kernel `evdev`/`uinput` and has not been live-tested in a Wayland
>   compositor.
> - **Use with care and read the source code yourself** before running it
>   (especially since it reads `/dev/input/event*` with `EVIOCGRAB` and writes to
>   `/dev/uinput`).

`uptrkeys` turns your keyboard into a smooth, chorded pointer and scroll device
on Linux using `evdev` and `uinput`, with physical `CapsLock` acting as the
hold-to-activate prefix layer key.

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
- `src/main.c` — CLI options, binding validation, `SIGINT`/`SIGTERM`/`SIGHUP` handling.
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
make PREFIX=~/.local install
```

`make audit` is a quick sanity check (imported symbols, embedded paths,
forbidden calls), not a proof. The real guarantee is reading the source, which
is small enough to do in one sitting.

Requirements:
- Linux kernel headers (`linux/input.h`, `linux/uinput.h`)
- C11 compiler (`gcc` or `clang`) and GNU `make`
- Optional: `valgrind` (`make check`)

## Permissions

`uptrkeys` requires read access to `/dev/input/event*` (for physical keyboards)
and write access to `/dev/uinput` (with the `uinput` kernel module loaded) to
create its virtual keyboard and pointer devices.

How you grant that access is **your choice and your trust decision**. Any
program with this access can read every keystroke, so pick a setup you
understand and are comfortable with for your system.

For reference only: the author's quick test was a setgid-`input` binary
(`chgrp input` + `chmod g+s`, on a mount without `nosuid`, on a system where
`input` can write `/dev/uinput`). This is **risky** — anyone allowed to execute
that binary gets keyboard access through it — and is not a recommendation.
When running setgid, `-d`/`-dd` are refused because debug logs contain
keystrokes; use `-i`/`--info` for non-sensitive startup/device logs.

## Caveats

- **CapsLock is never sent to applications.** If CapsLock is already on when
  `uptrkeys` starts, you cannot turn it off until `uptrkeys` exits.
- **Keyboards are grabbed exclusively** (`EVIOCGRAB`, which also blocks
  SysRq). If the process hangs, recover with a mouse, ssh, or another
  terminal (`kill` it; the kernel releases the grab when it exits).
- **`x` in pointer mode quits `uptrkeys`.** Remove or rebind it in `config.h`
  if that is too easy to hit.

## Configuration

Customize `PREFIX_KEY` (default `KEY_CAPSLOCK`), pointer-mode bindings, speeds
(`BASE_SPEED`, `BASE_SCROLL`), and frame rate (`FPS`) in `config.h` (copied
from `config.def.h` on first build), then rebuild with `make`.

### Modifiers (`Mod + Mouse`)

Any modifier key bound in `bindings[]` (`KEY_LEFTCTRL`, `KEY_LEFTALT`) is
consumed for pointer speed control while pointer mode is active. Unbound
modifiers (`KEY_LEFTSHIFT`, `KEY_RIGHTSHIFT`, `KEY_LEFTMETA`, `KEY_RIGHTMETA`,
`KEY_RIGHTCTRL`, `KEY_RIGHTALT`) pass straight through to the virtual keyboard,
allowing normal `Mod + Click` / `Mod + Drag` / `Mod + Scroll`.

## Default Bindings

- **Enable/Disable**: Hold physical `CapsLock` (release to exit), `x` (quit)
- **Move**: `w`, `a`, `s`, `d`
- **Scroll**: hold `Tab`
- **Multipliers**: `LeftAlt` (`/8`), `LeftCtrl` (`*32`), `j` (`/8`), `k` (`/2`),
  `l` (`*4`), `;` (`*8`)
- **Click**: `Space` (left), `e`/`n` (right), `r`/`m` (middle)

## Debugging

- `uptrkeys -i` (or `--info`): logs non-sensitive startup, virtual-device, skipped/grabbed/unplugged `/dev/input/event*`, dropped-event resync, and shutdown messages on `stderr` (never logs keystrokes or pointer events; allowed when setuid/setgid).
- `uptrkeys -d` (or `--debug`, `--verbose`): also logs key routing, `CapsLock` prefix transitions, scroll/speed changes, and button/key emissions (refused when setuid/setgid).
- `uptrkeys -dd` (or `--trace`): also logs autorepeat suppression and every motion/scroll tick (refused when setuid/setgid).

## Acknowledgements

Inspired by [ptrkeys](https://github.com/torbiak/ptrkeys) by Jordan Torbiak.
