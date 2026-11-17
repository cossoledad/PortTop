CC ?= cc
CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw -ltinfo)
CPPFLAGS += -D_GNU_SOURCE -Isrc $(CURSES_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion
LDFLAGS ?=
PREFIX ?= $(HOME)/.local
BUILD_DIR ?= build
DIST_DIR ?= build/dist
SOURCES = src/main.c src/proc.c src/actions.c src/history.c src/text_view.c src/ui.c
OBJECTS = $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean test test-install test-package package sanitize install uninstall
all: $(BUILD_DIR)/porttop
$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/%.o: src/%.c src/porttop.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/porttop: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(CURSES_LIBS) -o $@
$(BUILD_DIR)/test-core: tests/core.c $(BUILD_DIR)/proc.o $(BUILD_DIR)/actions.o $(BUILD_DIR)/history.o $(BUILD_DIR)/text_view.o src/porttop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/core.c $(BUILD_DIR)/proc.o $(BUILD_DIR)/actions.o $(BUILD_DIR)/history.o $(BUILD_DIR)/text_view.o -o $@
test: all $(BUILD_DIR)/test-core
	./$(BUILD_DIR)/test-core
	python3 tests/integration.py ./$(BUILD_DIR)/porttop
test-install:
	mkdir -p build
	python3 tests/install.py
test-package:
	python3 tests/package.py
package:
	python3 scripts/package.py --output "$(DIST_DIR)"
sanitize:
	$(MAKE) BUILD_DIR=build/asan clean
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(MAKE) BUILD_DIR=build/asan CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
install: all
	install -Dm755 "$(BUILD_DIR)/porttop" "$(DESTDIR)$(PREFIX)/bin/porttop"
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/porttop"
clean:
	rm -rf "$(BUILD_DIR)"
-include $(OBJECTS:.o=.d)
