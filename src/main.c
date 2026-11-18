#include "uptrkeys.h"
#include "config.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define USAGE "usage: uptrkeys [-d|--debug] [-dd|--trace] [-h|--help] [-v|--version]\n"

static volatile sig_atomic_t stop_requested = 0;

static void
on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void
install_signals(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) == -1 ||
	    sigaction(SIGTERM, &sa, NULL) == -1) {
		fprintf(stderr, "uptrkeys: sigaction: %s\n", strerror(errno));
		exit(1);
	}
}

static void
parse_args(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0 ||
		    strcmp(argv[i], "--verbose") == 0) {
			debug_level = (debug_level < 1) ? 1 : 2;
		} else if (strcmp(argv[i], "-dd") == 0 || strcmp(argv[i], "--trace") == 0) {
			debug_level = 2;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf(USAGE);
			exit(0);
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			printf("uptrkeys %s\n", VERSION);
			exit(0);
		} else {
			fprintf(stderr, USAGE);
			exit(1);
		}
	}
}

int
main(int argc, char *argv[])
{
	parse_args(argc, argv);

	size_t num_bindings = sizeof(bindings) / sizeof(bindings[0]);
	char errbuf[256] = {0};
	if (!engine_validate_bindings(bindings, num_bindings, errbuf, sizeof(errbuf))) {
		fprintf(stderr, "uptrkeys: invalid config: %s\n", errbuf);
		return 1;
	}

	install_signals();

	Engine eng;
	EngineOps empty_ops = {0};
	engine_init(&eng, bindings, num_bindings, internal_mods,
	            BASE_SPEED, BASE_SCROLL, empty_ops);

	return backend_run(&eng, FPS, &stop_requested);
}
