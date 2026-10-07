CC ?= gcc
WARNINGS = -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
BASE_CFLAGS = -std=c11 $(WARNINGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= $(BASE_CFLAGS)
INCLUDES ?= -Isrc -Ithirdparty

HAVE_SYSTEM_SQLITE_H := $(shell printf '\#include <sqlite3.h>\nint main(void){return 0;}' \
	| $(CC) -x c - -fsyntax-only 2>/dev/null && echo yes)
ifneq ($(HAVE_SYSTEM_SQLITE_H),yes)
INCLUDES += -Ithirdparty/sqlite3
endif
DEPFLAGS = -MMD -MP
LDFLAGS ?=

SQLITE_LIB ?= $(shell \
	pkg-config --libs sqlite3 2>/dev/null \
	|| (printf 'int main(void){return 0;}' | $(CC) -x c - -lsqlite3 -o /dev/null 2>/dev/null && echo "-lsqlite3") \
	|| ls /usr/lib/*/libsqlite3.so.0 /usr/lib/libsqlite3.so.0 2>/dev/null | head -n1 \
	|| echo "-lsqlite3")
SQLITE_CFLAGS ?= $(shell pkg-config --cflags sqlite3 2>/dev/null)
LDLIBS ?= $(SQLITE_LIB) -lm -lpthread

BUILD_DIR ?= build
BIN = $(BUILD_DIR)/jrun
TEST_BIN = $(BUILD_DIR)/test_jrun
ASAN_BIN = $(BUILD_DIR)/test_jrun_asan

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL) -m 755
INSTALL_DIR ?= $(INSTALL) -d

COMMON_SRCS = src/cli.c \
              src/config.c \
              src/database.c \
              src/scanner.c \
              src/matcher.c \
              src/resolver.c \
              src/tui.c \
              src/executor.c \
              src/shell.c \
              src/session.c \
              src/path_util.c

COMMON_OBJS = $(COMMON_SRCS:src/%.c=$(BUILD_DIR)/%.o)
OBJS = $(BUILD_DIR)/main.o $(COMMON_OBJS)
TEST_OBJS = $(BUILD_DIR)/tests/test_main.o $(COMMON_OBJS)
DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all release debug test asan check clean install uninstall install-bash uninstall-bash help

all: release

help:
	@echo "jrun build targets:"
	@echo "  make            Build an optimized binary into $(BUILD_DIR)/"
	@echo "  make debug      Build unoptimized with debug symbols"
	@echo "  make test       Build and run the test suite"
	@echo "  make asan       Run the test suite under AddressSanitizer + UBSan"
	@echo "  make check      Run both test and asan"
	@echo "  make install    Install to $(BINDIR) (run 'make' first)"
	@echo "  make clean      Remove $(BUILD_DIR)/"

release: CFLAGS += -O2 -DNDEBUG
release: $(BUILD_DIR)/.mode_release $(BIN)

debug: CFLAGS += -O0 -g3 -DDEBUG
debug: $(BUILD_DIR)/.mode_debug $(BIN)

$(BUILD_DIR)/.mode_release: | $(BUILD_DIR)
	@if [ -f $(BUILD_DIR)/.mode_debug ]; then \
		rm -f $(BUILD_DIR)/.mode_debug $(OBJS) $(TEST_OBJS) $(BIN) $(TEST_BIN); \
	fi
	@touch $@

$(BUILD_DIR)/.mode_debug: | $(BUILD_DIR)
	@if [ -f $(BUILD_DIR)/.mode_release ]; then \
		rm -f $(BUILD_DIR)/.mode_release $(OBJS) $(TEST_OBJS) $(BIN) $(TEST_BIN); \
	fi
	@touch $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/tests

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(DEPFLAGS) $(CFLAGS) $(INCLUDES) $(SQLITE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(DEPFLAGS) $(CFLAGS) $(INCLUDES) $(SQLITE_CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

test: $(TEST_BIN)
	@echo "Running test suite..."
	@$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(LDFLAGS) $(TEST_OBJS) -o $@ $(LDLIBS)

# Sanitizers are built in one shot rather than reusing $(BUILD_DIR) objects,
# which are compiled without instrumentation.
asan: | $(BUILD_DIR)
	$(CC) $(BASE_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(SQLITE_CFLAGS) -o $(ASAN_BIN) tests/test_main.c $(COMMON_SRCS) $(LDLIBS)
	@echo "Running test suite under AddressSanitizer + UBSan..."
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(ASAN_BIN)

check: test asan

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

# Deliberately does NOT depend on $(BIN): `sudo make install` would otherwise
# compile as root and leave a build directory the user can no longer clean.
install:
	@if [ ! -x "$(BIN)" ]; then \
		echo "error: $(BIN) does not exist."; \
		echo "Build it first as your normal user, then install:"; \
		echo "    make && sudo make install"; \
		exit 1; \
	fi
	$(INSTALL_DIR) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(BIN) $(DESTDIR)$(BINDIR)/jrun
	@echo ""
	@echo "jrun installed to $(DESTDIR)$(BINDIR)/jrun"
	@echo ""
	@echo "'make install' does not modify your shell configuration."
	@echo "To enable the 'j' command and directory tracking:"
	@echo "  Bash: make install-bash, or add to ~/.bashrc:"
	@echo '        eval "$$(jrun --init bash)"'
	@echo "  Zsh:  add to ~/.zshrc:"
	@echo '        eval "$$(jrun --init zsh)"'
	@echo "  Fish: add to ~/.config/fish/config.fish:"
	@echo "        jrun --init fish | source"
	@echo ""

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jrun

install-bash:
	@USER_HOME="$${HOME}"; \
	if [ -n "$${SUDO_USER}" ] && [ "$${SUDO_USER}" != "root" ]; then \
		USER_HOME=$$(getent passwd "$${SUDO_USER}" | cut -d: -f6); \
	fi; \
	BASHRC="$${USER_HOME}/.bashrc"; \
	LINE='eval "$$(jrun --init bash)"'; \
	if [ ! -f "$$BASHRC" ]; then \
		touch "$$BASHRC"; \
	fi; \
	if grep -qF "$$LINE" "$$BASHRC" 2>/dev/null; then \
		echo "jrun shell integration is already present in $$BASHRC"; \
	else \
		printf '\n# jrun shell integration\n%s\n' "$$LINE" >> "$$BASHRC"; \
		echo "Added jrun shell integration to $$BASHRC"; \
		echo "Run 'source ~/.bashrc' or restart your terminal to activate."; \
	fi

uninstall-bash:
	@USER_HOME="$${HOME}"; \
	if [ -n "$${SUDO_USER}" ] && [ "$${SUDO_USER}" != "root" ]; then \
		USER_HOME=$$(getent passwd "$${SUDO_USER}" | cut -d: -f6); \
	fi; \
	BASHRC="$${USER_HOME}/.bashrc"; \
	if [ -f "$$BASHRC" ]; then \
		sed -i '/# jrun shell integration/d' "$$BASHRC"; \
		sed -i '/eval "\$$(jrun --init bash)"/d' "$$BASHRC"; \
		echo "Removed jrun shell integration from $$BASHRC"; \
	fi
