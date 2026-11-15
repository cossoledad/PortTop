CC ?= cc
CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw -ltinfo)
CPPFLAGS += -D_GNU_SOURCE -Isrc $(CURSES_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion
LDFLAGS ?=
PREFIX ?= /usr/local
SOURCES = src/main.c src/proc.c src/actions.c src/ui.c
OBJECTS = $(SOURCES:src/%.c=build/%.o)

.PHONY: all clean test sanitize install uninstall
all: build/porttop
build:
	mkdir -p $@
build/%.o: src/%.c src/porttop.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
build/porttop: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(CURSES_LIBS) -o $@
build/test-core: tests/core.c build/proc.o build/actions.o src/porttop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/core.c build/proc.o build/actions.o -o $@
test: all build/test-core
	./build/test-core
	python3 tests/integration.py ./build/porttop
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
	ASAN_OPTIONS=detect_leaks=1 $(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
install: all
	install -Dm755 build/porttop $(DESTDIR)$(PREFIX)/bin/porttop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/porttop
clean:
	rm -rf build
-include $(OBJECTS:.o=.d)
