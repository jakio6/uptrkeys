#!/bin/sh
set -eu

BIN=${1:-./uptrkeys}

if [ ! -x "$BIN" ]; then
	echo "audit FAIL: missing binary $BIN" >&2
	exit 1
fi

# 1. Verify ELF dynamic symbol allowlist (including _FORTIFY_SOURCE / musl variants):
#    - Zero network (socket/connect/bind/listen/sendto/recvfrom/getaddrinfo)
#    - Zero process spawn (fork/vfork/clone/exec*/system/popen)
#    - Zero dynamic loading / env / arbitrary file streams (dlopen/getenv/fopen)
#    - Zero dynamic heap allocation (malloc/calloc/realloc/free)
SYMS=$(nm -D -u "$BIN" | awk '{print $NF}' | sed 's/@.*//' | sort -u)

ALLOWED="
__cxa_finalize
__errno_location
__fprintf_chk
__gmon_start__
__libc_start_main
__open64_2
__open_2
__poll_chk
__ppoll_chk
__printf_chk
__read_chk
__snprintf_chk
__stack_chk_fail
__vsnprintf_chk
_ITM_deregisterTMCloneTable
_ITM_registerTMCloneTable
clock_gettime
close
closedir
exit
fprintf
fputc
fstat
fwrite
getegid
geteuid
getgid
getuid
inotify_add_watch
inotify_init1
ioctl
llround
lstat
memcpy
memmove
memset
modf
nanosleep
open
opendir
poll
ppoll
printf
puts
read
readdir
sigaction
sigaddset
sigemptyset
sigprocmask
snprintf
stderr
stdout
strcmp
strerror
strncmp
vsnprintf
write
"

BAD_SYMS=""
for s in $SYMS; do
	case "$ALLOWED" in
		*"
$s
"*) ;;
		*) BAD_SYMS="$BAD_SYMS $s" ;;
	esac
done

if [ -n "$BAD_SYMS" ]; then
	echo "audit FAIL: unexpected imported symbol(s):$BAD_SYMS" >&2
	exit 1
fi

# 2. Verify all absolute filesystem paths compiled into the binary are /dev/input* or /dev/uinput
#    (plus the ELF dynamic linker /lib*/ld-linux* or /lib*/ld-musl*).
BAD_PATHS=$(strings -n 4 "$BIN" | grep '^/' | grep -Ev '^(/dev/uinput$|/dev/input(/uinput|/%s)?$|/+lib(64)?/ld-(linux|musl))' || true)
if [ -n "$BAD_PATHS" ]; then
	echo "audit FAIL: unexpected path string(s): $BAD_PATHS" >&2
	exit 1
fi

# 3. Source-level sanity check across src/: ensure engine.c has zero OS headers and no file uses forbidden calls.
if grep -qE '#include <(fcntl|unistd|dirent|stdlib|sys/)' src/engine.c; then
	echo "audit FAIL: src/engine.c must stay pure (no OS/stdlib/fcntl/unistd headers)" >&2
	exit 1
fi
if grep -rnE '\b(system|popen|fork|vfork|clone|exec[lv][pe]*|socket|connect|fopen|freopen|fdopen|getenv|setenv|dlopen|ptrace)\s*\(' src/; then
	echo "audit FAIL: forbidden function call found in src/" >&2
	exit 1
fi

echo "audit PASS: symbol allowlist ($(echo "$SYMS" | wc -l) syms), path allowlist (/dev/input, /dev/uinput only), zero heap/network/exec"
