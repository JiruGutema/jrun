#include "tui.h"
#include "common.h"
#include "matcher.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>

static void buf_append_str(char *buf, size_t *pos, size_t cap, const char *str)
{
    size_t len = strlen(str);
    if (*pos + len < cap) {
        memcpy(buf + *pos, str, len);
        *pos += len;
        buf[*pos] = '\0';
    }
}

static void buf_appendf(char *buf, size_t *pos, size_t cap, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);
    if (n > 0 && *pos + (size_t)n < cap) {
        *pos += (size_t)n;
    }
}

static int utf8_visual_width(const char *s)
{
    int w = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) != 0x80) {
            w++;
        }
        s++;
    }
    return w;
}

static void sanitize_display_string(char *dest, const char *src, size_t dest_size)
{
    if (!dest || dest_size == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 1 < dest_size; ++s) {
        unsigned char c = (unsigned char)src[s];
        if (c < 32 || c == 127) {
            dest[d++] = '?';
        } else {
            dest[d++] = (char)c;
        }
    }
    dest[d] = '\0';
}

static struct termios g_orig_termios;
static bool g_raw_mode = false;
static volatile sig_atomic_t g_interrupted = 0;

static void disable_raw_mode(void)
{
    if (g_raw_mode) {
        // Exit alternate screen, reset styling, show cursor
        const char *exit_seq = "\x1b[0m\x1b[?25h\x1b[?1049l";
        (void)write(STDOUT_FILENO, exit_seq, strlen(exit_seq));
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = false;
    }
}

static bool enable_raw_mode(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        return false;
    }
    atexit(disable_raw_mode);

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms read timeout

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return false;
    }
    g_raw_mode = true;

    // Enter alternate screen, clear, hide cursor
    const char *enter_seq = "\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l";
    (void)write(STDOUT_FILENO, enter_seq, strlen(enter_seq));
    return true;
}

static void sigint_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

enum Tui_Key {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_ESC,
};

typedef struct {
    int key;
    char ch;
} Input_Event;

static Input_Event read_input(void)
{
    char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return (Input_Event){ .key = KEY_NONE };

    if (c == '\x1b') {
        char seq[4] = {0};
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return (Input_Event){ .key = KEY_ESC };
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return (Input_Event){ .key = KEY_ESC };

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) <= 0) return (Input_Event){ .key = KEY_ESC };
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1': case '7': return (Input_Event){ .key = KEY_HOME };
                    case '3': return (Input_Event){ .key = KEY_BACKSPACE };
                    case '4': case '8': return (Input_Event){ .key = KEY_END };
                    case '5': return (Input_Event){ .key = KEY_PAGE_UP };
                    case '6': return (Input_Event){ .key = KEY_PAGE_DOWN };
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return (Input_Event){ .key = KEY_UP };
                case 'B': return (Input_Event){ .key = KEY_DOWN };
                case 'H': return (Input_Event){ .key = KEY_HOME };
                case 'F': return (Input_Event){ .key = KEY_END };
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': return (Input_Event){ .key = KEY_HOME };
            case 'F': return (Input_Event){ .key = KEY_END };
            }
        }
        return (Input_Event){ .key = KEY_ESC };
    }

    if (c == 13 || c == 10) return (Input_Event){ .key = KEY_ENTER };
    if (c == 127 || c == 8) return (Input_Event){ .key = KEY_BACKSPACE };
    if (c == 3) return (Input_Event){ .key = KEY_ESC }; // Ctrl-C
    if (c == 14) return (Input_Event){ .key = KEY_DOWN }; // Ctrl-N
    if (c == 16) return (Input_Event){ .key = KEY_UP }; // Ctrl-P

    return (Input_Event){ .key = KEY_CHAR, .ch = c };
}

typedef struct {
    size_t index;
    double score;
} Filtered_Item;

static int compare_filtered_desc(const void *a, const void *b)
{
    const Filtered_Item *fa = (const Filtered_Item *)a;
    const Filtered_Item *fb = (const Filtered_Item *)b;
    if (fb->score > fa->score) return 1;
    if (fb->score < fa->score) return -1;
    return 0;
}

char *tui_select(const Resolve_Candidate *candidates, size_t count, const char *initial_query)
{
    if (!candidates || count == 0) return NULL;

    if (!enable_raw_mode()) {
        jrun_log_error("failed to initialize terminal raw mode for TUI");
        return NULL;
    }

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, &old_sa);

    char query[256];
    size_t qlen = 0;
    if (initial_query) {
        strncpy(query, initial_query, sizeof(query) - 1);
        query[sizeof(query) - 1] = '\0';
        qlen = strlen(query);
    } else {
        query[0] = '\0';
    }

    Filtered_Item *filtered = (Filtered_Item *)malloc(count * sizeof(Filtered_Item));
    if (!filtered) {
        disable_raw_mode();
        sigaction(SIGINT, &old_sa, NULL);
        return NULL;
    }

    size_t selected = 0;
    size_t scroll_offset = 0;
    char *result_path = NULL;

    while (!g_interrupted) {
        // 1. Refilter candidates
        size_t filtered_count = 0;
        for (size_t i = 0; i < count; ++i) {
            if (qlen == 0) {
                filtered[filtered_count].index = i;
                filtered[filtered_count].score = candidates[i].score;
                filtered_count++;
            } else {
                Match_Result m = matcher_evaluate(query, candidates[i].path);
                if (m.is_match) {
                    filtered[filtered_count].index = i;
                    filtered[filtered_count].score = (candidates[i].frecency + 0.5) * m.quality_score;
                    filtered_count++;
                }
            }
        }

        if (filtered_count > 1) {
            qsort(filtered, filtered_count, sizeof(Filtered_Item), compare_filtered_desc);
        }

        if (selected >= filtered_count) {
            selected = filtered_count > 0 ? filtered_count - 1 : 0;
        }

        // 2. Get terminal dimensions
        struct winsize ws;
        int cols = 80;
        int rows = 24;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) {
            cols = ws.ws_col;
            rows = ws.ws_row;
        }

        int box_width = cols - 4;
        if (box_width > 120) box_width = 120;
        if (box_width < 45) box_width = 45;

        int list_height = rows - 8;
        if (list_height < 3) list_height = 3;
        if (list_height > 15) list_height = 15;

        // Keep selected item within scroll viewport
        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + (size_t)list_height) {
            scroll_offset = selected - (size_t)list_height + 1;
        }

        // 3. Render frame to buffer
        char buf[8192];
        size_t buf_pos = 0;
        // Reset cursor to top-left
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[H\x1b[?25l");

        // Top border: ┌─ Select directory ─────────────────────────────────┐
        buf_append_str(buf, &buf_pos, sizeof(buf), "┌─ Select directory ");
        int top_fill = box_width - 21;
        for (int i = 0; i < top_fill; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), "─");
        buf_append_str(buf, &buf_pos, sizeof(buf), "┐\r\n");

        // Search line: │ Search: <query>                                    │
        char safe_query[256];
        sanitize_display_string(safe_query, query, sizeof(safe_query));
        buf_appendf(buf, &buf_pos, sizeof(buf), "│ Search: \x1b[1m%s\x1b[0m", safe_query);
        int search_fill = box_width - 10 - (int)qlen;
        for (int i = 0; i < search_fill; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), " ");
        buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");

        // Separator: ├────────────────────────────────────────────────────┤
        buf_append_str(buf, &buf_pos, sizeof(buf), "├");
        for (int i = 0; i < box_width - 2; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), "─");
        buf_append_str(buf, &buf_pos, sizeof(buf), "┤\r\n");

        // Empty line
        buf_append_str(buf, &buf_pos, sizeof(buf), "│");
        for (int i = 0; i < box_width - 2; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), " ");
        buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");

        // List items
        for (int row = 0; row < list_height; ++row) {
            size_t item_idx = scroll_offset + (size_t)row;
            buf_append_str(buf, &buf_pos, sizeof(buf), "│ ");
            int inner_width = box_width - 4;

            if (item_idx < filtered_count) {
                size_t cand_idx = filtered[item_idx].index;
                const char *orig_path = candidates[cand_idx].path;
                char raw_display_path[PATH_MAX];
                char display_path[PATH_MAX];
                path_shorten_tilde(orig_path, raw_display_path, sizeof(raw_display_path));
                sanitize_display_string(display_path, raw_display_path, sizeof(display_path));

                bool is_sel = (item_idx == selected);
                int path_max_len = inner_width - 4;
                char truncated[PATH_MAX];

                if ((int)strlen(display_path) > path_max_len) {
                    // Ellipsize middle or start
                    int keep = path_max_len - 3;
                    if (keep > 0) {
                        snprintf(truncated, sizeof(truncated), "...%s", display_path + strlen(display_path) - keep);
                    } else {
                        strncpy(truncated, display_path, path_max_len);
                        truncated[path_max_len] = '\0';
                    }
                } else {
                    strncpy(truncated, display_path, sizeof(truncated) - 1);
                    truncated[sizeof(truncated) - 1] = '\0';
                }

                if (is_sel) {
                    buf_appendf(buf, &buf_pos, sizeof(buf), "\x1b[1;36m❯ %-*s\x1b[0m", inner_width - 2, truncated);
                } else {
                    buf_appendf(buf, &buf_pos, sizeof(buf), "  %-*s", inner_width - 2, truncated);
                }
            } else {
                for (int i = 0; i < inner_width; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), " ");
            }
            buf_append_str(buf, &buf_pos, sizeof(buf), " │\r\n");
        }

        // Empty line
        buf_append_str(buf, &buf_pos, sizeof(buf), "│");
        for (int i = 0; i < box_width - 2; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), " ");
        buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");

        // Separator: ├────────────────────────────────────────────────────┤
        buf_append_str(buf, &buf_pos, sizeof(buf), "├");
        for (int i = 0; i < box_width - 2; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), "─");
        buf_append_str(buf, &buf_pos, sizeof(buf), "┤\r\n");

        // Footer: │ ↑↓ Navigate   Enter Select   Esc Cancel            │
        const char *footer_text = "↑↓ Navigate   Enter Select   Esc Cancel";
        buf_appendf(buf, &buf_pos, sizeof(buf), "│ \x1b[90m%s\x1b[0m", footer_text);
        int footer_fill = box_width - 4 - utf8_visual_width(footer_text);
        if (footer_fill < 0) footer_fill = 0;
        for (int i = 0; i < footer_fill; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), " ");
        buf_append_str(buf, &buf_pos, sizeof(buf), " │\r\n");

        // Bottom border: └────────────────────────────────────────────────────┘
        buf_append_str(buf, &buf_pos, sizeof(buf), "└");
        for (int i = 0; i < box_width - 2; ++i) buf_append_str(buf, &buf_pos, sizeof(buf), "─");
        buf_append_str(buf, &buf_pos, sizeof(buf), "┘\r\n");

        (void)write(STDOUT_FILENO, buf, buf_pos);

        // 4. Handle input event
        Input_Event ev = read_input();
        if (ev.key == KEY_NONE) {
            continue;
        }

        if (ev.key == KEY_ESC) {
            break; // Cancel
        }

        if (ev.key == KEY_ENTER) {
            if (filtered_count > 0 && selected < filtered_count) {
                result_path = strdup(candidates[filtered[selected].index].path);
            }
            break;
        }

        if (ev.key == KEY_UP) {
            if (selected > 0) selected--;
        } else if (ev.key == KEY_DOWN) {
            if (selected + 1 < filtered_count) selected++;
        } else if (ev.key == KEY_HOME) {
            selected = 0;
        } else if (ev.key == KEY_END) {
            if (filtered_count > 0) selected = filtered_count - 1;
        } else if (ev.key == KEY_PAGE_UP) {
            if (selected >= (size_t)list_height) selected -= (size_t)list_height;
            else selected = 0;
        } else if (ev.key == KEY_PAGE_DOWN) {
            selected += (size_t)list_height;
            if (selected >= filtered_count && filtered_count > 0) {
                selected = filtered_count - 1;
            }
        } else if (ev.key == KEY_BACKSPACE) {
            if (qlen > 0) {
                query[--qlen] = '\0';
                selected = 0;
            }
        } else if (ev.key == KEY_CHAR) {
            if (isprint((unsigned char)ev.ch) && qlen + 1 < sizeof(query)) {
                query[qlen++] = ev.ch;
                query[qlen] = '\0';
                selected = 0;
            }
        }
    }

    free(filtered);
    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);

    return result_path;
}
