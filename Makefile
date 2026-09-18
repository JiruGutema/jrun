CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
INCLUDES ?= -Isrc -Ithirdparty -Ithirdparty/sqlite3
DEPFLAGS = -MMD -MP
LDFLAGS ?=

SQLITE_LIB ?= $(shell pkg-config --libs sqlite3 2>/dev/null || (test -f /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 && echo "/usr/lib/x86_64-linux-gnu/libsqlite3.so.0") || echo "-lsqlite3")
LDLIBS ?= $(SQLITE_LIB) -lm -lpthread

BUILD_DIR ?= build
BIN = $(BUILD_DIR)/jrun
TEST_BIN = $(BUILD_DIR)/test_jrun

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
              src/path_util.c

COMMON_OBJS = $(COMMON_SRCS:src/%.c=$(BUILD_DIR)/%.o)
OBJS = $(BUILD_DIR)/main.o $(COMMON_OBJS)
TEST_OBJS = $(BUILD_DIR)/tests/test_main.o $(COMMON_OBJS)
DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all release debug test clean install uninstall install-bash uninstall-bash

all: release

release: CFLAGS += -O3 -DNDEBUG
release: $(BUILD_DIR)/.mode_release $(BIN)

debug: CFLAGS += -O0 -g3 -DDEBUG
debug: $(BUILD_DIR)/.mode_debug $(BIN)

$(BUILD_DIR)/.mode_release: | $(BUILD_DIR)
	@if [ -f $(BUILD_DIR)/.mode_debug ]; then \
		rm -f $(BUILD_DIR)/.mode_debug $(OBJS) $(BIN); \
	fi
	@touch $@

$(BUILD_DIR)/.mode_debug: | $(BUILD_DIR)
	@if [ -f $(BUILD_DIR)/.mode_release ]; then \
		rm -f $(BUILD_DIR)/.mode_release $(OBJS) $(BIN); \
	fi
	@touch $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/tests

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(DEPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(DEPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

test: $(TEST_BIN)
	@echo "Running test suite..."
	@$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(LDFLAGS) $(TEST_OBJS) -o $@ $(LDLIBS)

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

install: $(BIN)
	$(INSTALL_DIR) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(BIN) $(DESTDIR)$(BINDIR)/jrun
	@echo ""
	@echo "jrun successfully installed to $(DESTDIR)$(BINDIR)/jrun"
	@echo ""
	@echo "Note: 'make install' does NOT automatically modify shell configuration."
	@echo "To enable the 'j' jumping command and shell hooks:"
	@echo "  Bash: Run 'make install-bash' or add to ~/.bashrc:"
	@echo '        eval "$$(jrun --init bash)"'
	@echo "  Zsh:  Add to ~/.zshrc:"
	@echo '        eval "$$(jrun --init zsh)"'
	@echo "  Fish: Add to ~/.config/fish/config.fish:"
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
