CC ?= gcc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -pedantic -Wall -Wextra -O2
LDFLAGS ?= -s -lm
PREFIX ?= /usr/local
DESTDIR ?=

SYS_CPPFLAGS := -I. -Isrc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
VERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo "0.1.0")

SRC := src/main.c src/engine.c src/backend.c
TEST_BIN := unit_test

all: uptrkeys

config.h: config.def.h
	cp config.def.h $@

uptrkeys: config.h src/uptrkeys.h $(SRC)
	$(CC) -o $@ $(SYS_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) -DVERSION=\"$(VERSION)\" $(SRC) $(LDFLAGS)

$(TEST_BIN): config.h src/uptrkeys.h src/engine.c tests/unit_test.c
	$(CC) -o $@ $(SYS_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) src/engine.c tests/unit_test.c $(LDFLAGS)

audit: uptrkeys tests/audit.sh
	sh ./tests/audit.sh ./uptrkeys

check: $(TEST_BIN) audit
	@if command -v valgrind >/dev/null 2>&1 && valgrind -q true >/dev/null 2>&1; then \
		valgrind --leak-check=full --error-exitcode=1 -q ./$(TEST_BIN); \
	else \
		./$(TEST_BIN); \
	fi

sanitize: config.h src/uptrkeys.h src/engine.c tests/unit_test.c
	$(CC) -o $(TEST_BIN)_san $(SYS_CPPFLAGS) $(CPPFLAGS) -std=c11 -pedantic -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer src/engine.c tests/unit_test.c -lm
	./$(TEST_BIN)_san
	rm -f $(TEST_BIN)_san

clean:
	rm -f uptrkeys $(TEST_BIN) $(TEST_BIN)_san *.o

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	cp uptrkeys $(DESTDIR)$(PREFIX)/bin/uptrkeys
	chmod 755 $(DESTDIR)$(PREFIX)/bin/uptrkeys
	cp uptrkeys.1 $(DESTDIR)$(PREFIX)/share/man/man1/uptrkeys.1
	chmod 644 $(DESTDIR)$(PREFIX)/share/man/man1/uptrkeys.1

.PHONY: all audit check clean install sanitize
