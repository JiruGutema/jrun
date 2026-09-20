#include "tui.h"
#include "common.h"
#include "matcher.h"
#include "path_util.h"
#include "config.h"

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
static volatile sig_atomic_t g_resized = 0;

static void disable_raw_mode(void)
{
    if (g_raw_mode) {
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
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return false;
    }
    g_raw_mode = true;

    const char *enter_seq = "\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l";
    (void)write(STDOUT_FILENO, enter_seq, strlen(enter_seq));
    return true;
}

static void sigint_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static void sigwinch_handler(int sig)
{
    (void)sig;
    g_resized = 1;
}

enum Tui_Key {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_CLEAR,
    KEY_KILL_WORD,
    KEY_TAB,
    KEY_SAVE,
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
                case 'C': return (Input_Event){ .key = KEY_RIGHT };
                case 'D': return (Input_Event){ .key = KEY_LEFT };
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
    if (c == 3) return (Input_Event){ .key = KEY_ESC };
    if (c == 14) return (Input_Event){ .key = KEY_DOWN };
    if (c == 16) return (Input_Event){ .key = KEY_UP };
    if (c == 21) return (Input_Event){ .key = KEY_CLEAR };
    if (c == 23) return (Input_Event){ .key = KEY_KILL_WORD };
    if (c == 9) return (Input_Event){ .key = KEY_TAB };
    if (c == 19) return (Input_Event){ .key = KEY_SAVE };
    if (c == 4) return (Input_Event){ .key = KEY_ESC };

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
    if (fa->index < fb->index) return -1;
    if (fa->index > fb->index) return 1;
    return 0;
}

static void get_term_size(int *cols, int *rows)
{
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

static void append_hline(char *buf, size_t *pos, size_t cap, const char *left, const char *right, int width)
{
    buf_append_str(buf, pos, cap, left);
    for (int i = 0; i < width - 2; ++i) buf_append_str(buf, pos, cap, "─");
    buf_append_str(buf, pos, cap, right);
    buf_append_str(buf, pos, cap, "\r\n");
}

static void append_labeled_top(char *buf, size_t *pos, size_t cap, const char *title, int width)
{
    buf_appendf(buf, pos, cap, "┌─ %s ", title);
    int fill = width - 4 - (int)strlen(title);
    if (fill < 0) fill = 0;
    for (int i = 0; i < fill; ++i) buf_append_str(buf, pos, cap, "─");
    buf_append_str(buf, pos, cap, "┐\r\n");
}

static void pad_to(char *buf, size_t *pos, size_t cap, int n)
{
    for (int i = 0; i < n; ++i) buf_append_str(buf, pos, cap, " ");
}

static void render_highlighted_path(char *buf, size_t *pos, size_t cap,
                                    const char *path, const char *query,
                                    int max_width, bool selected)
{
    char truncated[PATH_MAX];
    int vis = utf8_visual_width(path);
    if (vis > max_width) {
        int keep = max_width - 3;
        if (keep < 1) keep = 1;
        const char *src = path + strlen(path);
        int taken = 0;
        while (src > path && taken < keep) {
            src--;
            if (((unsigned char)*src & 0xC0) != 0x80) taken++;
        }
        snprintf(truncated, sizeof(truncated), "...%s", src);
    } else {
        strncpy(truncated, path, sizeof(truncated) - 1);
        truncated[sizeof(truncated) - 1] = '\0';
    }

    const char *q = query;
    int drawn = 0;
    for (const char *p = truncated; *p; ++p) {
        bool hit = false;
        if (q && *q && tolower((unsigned char)*p) == tolower((unsigned char)*q) && *p != '/') {
            hit = true;
            q++;
        }
        if (hit) {
            if (selected) buf_append_str(buf, pos, cap, "\x1b[1;97m");
            else buf_append_str(buf, pos, cap, "\x1b[1;33m");
            char ch[2] = { *p, 0 };
            buf_append_str(buf, pos, cap, ch);
            if (selected) buf_append_str(buf, pos, cap, "\x1b[1;37;44m");
            else buf_append_str(buf, pos, cap, "\x1b[0m");
        } else {
            char ch[2] = { *p, 0 };
            buf_append_str(buf, pos, cap, ch);
        }
        if (((unsigned char)*p & 0xC0) != 0x80) drawn++;
    }
    if (drawn < max_width) pad_to(buf, pos, cap, max_width - drawn);
}

static void join_argv(char *out, size_t out_size, char *const argv[])
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; argv && argv[i]; ++i) {
        int n = snprintf(out + pos, out_size - pos, "%s%s", i ? " " : "", argv[i]);
        if (n < 0 || pos + (size_t)n >= out_size) {
            out[out_size - 1] = '\0';
            return;
        }
        pos += (size_t)n;
    }
}

char *tui_select(const Resolve_Candidate *candidates, size_t count, const char *initial_query)
{
    if (!candidates || count == 0) return NULL;

    if (!enable_raw_mode()) {
        jrun_log_error("failed to initialize terminal raw mode for TUI");
        return NULL;
    }

    struct sigaction sa, old_sa, sa_win, old_win;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, &old_sa);
    memset(&sa_win, 0, sizeof(sa_win));
    sa_win.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_win, &old_win);

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
        sigaction(SIGWINCH, &old_win, NULL);
        return NULL;
    }

    size_t selected = 0;
    size_t scroll_offset = 0;
    char *result_path = NULL;
    bool needs_redraw = true;
    bool needs_filter = true;
    size_t filtered_count = 0;

    while (!g_interrupted) {
        if (g_resized) {
            g_resized = 0;
            needs_redraw = true;
        }

        if (needs_filter) {
            filtered_count = 0;
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
            needs_filter = false;
            needs_redraw = true;
        }

        int cols = 80;
        int rows = 24;
        get_term_size(&cols, &rows);

        int box_width = cols;
        if (box_width < 40) box_width = 40;

        int chrome = 7;
        int list_height = rows - chrome;
        if (list_height < 3) list_height = 3;

        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + (size_t)list_height) {
            scroll_offset = selected - (size_t)list_height + 1;
        }

        if (needs_redraw) {
            char buf[16384];
            size_t buf_pos = 0;
            buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[H\x1b[?25l");

            char title[64];
            snprintf(title, sizeof(title), "Select directory  %zu/%zu", filtered_count, count);
            append_labeled_top(buf, &buf_pos, sizeof(buf), title, box_width);

            char safe_query[256];
            sanitize_display_string(safe_query, query, sizeof(safe_query));
            buf_append_str(buf, &buf_pos, sizeof(buf), "│ Search: \x1b[1m");
            buf_append_str(buf, &buf_pos, sizeof(buf), safe_query);
            buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m");
            int search_fill = box_width - 10 - utf8_visual_width(safe_query);
            if (search_fill < 0) search_fill = 0;
            pad_to(buf, &buf_pos, sizeof(buf), search_fill);
            buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");

            append_hline(buf, &buf_pos, sizeof(buf), "├", "┤", box_width);

            for (int row = 0; row < list_height; ++row) {
                size_t item_idx = scroll_offset + (size_t)row;
                buf_append_str(buf, &buf_pos, sizeof(buf), "│");
                int inner_width = box_width - 2;

                if (item_idx < filtered_count) {
                    size_t cand_idx = filtered[item_idx].index;
                    const Resolve_Candidate *cand = &candidates[cand_idx];
                    char raw_display_path[PATH_MAX];
                    char display_path[PATH_MAX];
                    path_shorten_tilde(cand->path, raw_display_path, sizeof(raw_display_path));
                    sanitize_display_string(display_path, raw_display_path, sizeof(display_path));

                    bool is_sel = (item_idx == selected);
                    char score_txt[16];
                    snprintf(score_txt, sizeof(score_txt), "%5.1f", cand->score);
                    int score_w = 6;
                    int path_max = inner_width - 4 - score_w;
                    if (path_max < 4) path_max = 4;

                    if (is_sel) buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1;37;44m");
                    buf_append_str(buf, &buf_pos, sizeof(buf), is_sel ? "❯ " : "  ");
                    render_highlighted_path(buf, &buf_pos, sizeof(buf), display_path, query,
                                            path_max, is_sel);
                    buf_appendf(buf, &buf_pos, sizeof(buf), " \x1b[2m%s\x1b[0m", score_txt);
                    if (is_sel) buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m");
                } else {
                    pad_to(buf, &buf_pos, sizeof(buf), inner_width);
                }
                buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");
            }

            append_hline(buf, &buf_pos, sizeof(buf), "├", "┤", box_width);

            const char *footer_text = "↑↓/C-n/C-p  Enter select  C-u clear  C-w word  Esc cancel";
            buf_appendf(buf, &buf_pos, sizeof(buf), "│ \x1b[90m%s\x1b[0m", footer_text);
            int footer_fill = box_width - 4 - utf8_visual_width(footer_text);
            if (footer_fill < 0) footer_fill = 0;
            pad_to(buf, &buf_pos, sizeof(buf), footer_fill);
            buf_append_str(buf, &buf_pos, sizeof(buf), " │\r\n");
            append_hline(buf, &buf_pos, sizeof(buf), "└", "┘", box_width);
            buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[J");

            int cursor_col = 10 + utf8_visual_width(safe_query);
            if (cursor_col < box_width - 1) {
                buf_appendf(buf, &buf_pos, sizeof(buf), "\x1b[2;%dH\x1b[?25h", cursor_col + 1);
            }

            (void)write(STDOUT_FILENO, buf, buf_pos);
            needs_redraw = false;
        }

        Input_Event ev = read_input();
        if (ev.key == KEY_NONE) {
            continue;
        }

        if (ev.key == KEY_ESC) {
            break;
        }

        if (ev.key == KEY_ENTER) {
            if (filtered_count > 0 && selected < filtered_count) {
                result_path = jrun_strdup(candidates[filtered[selected].index].path);
            }
            break;
        }

        if (ev.key == KEY_UP) {
            if (selected > 0) selected--;
            needs_redraw = true;
        } else if (ev.key == KEY_DOWN) {
            if (selected + 1 < filtered_count) selected++;
            needs_redraw = true;
        } else if (ev.key == KEY_HOME) {
            selected = 0;
            needs_redraw = true;
        } else if (ev.key == KEY_END) {
            if (filtered_count > 0) selected = filtered_count - 1;
            needs_redraw = true;
        } else if (ev.key == KEY_PAGE_UP) {
            if (selected >= (size_t)list_height) selected -= (size_t)list_height;
            else selected = 0;
            needs_redraw = true;
        } else if (ev.key == KEY_PAGE_DOWN) {
            selected += (size_t)list_height;
            if (selected >= filtered_count && filtered_count > 0) {
                selected = filtered_count - 1;
            }
            needs_redraw = true;
        } else if (ev.key == KEY_BACKSPACE) {
            if (qlen > 0) {
                query[--qlen] = '\0';
                selected = 0;
                needs_filter = true;
            }
        } else if (ev.key == KEY_CLEAR) {
            query[0] = '\0';
            qlen = 0;
            selected = 0;
            needs_filter = true;
        } else if (ev.key == KEY_KILL_WORD) {
            while (qlen > 0 && query[qlen - 1] == ' ') query[--qlen] = '\0';
            while (qlen > 0 && query[qlen - 1] != ' ' && query[qlen - 1] != '/') {
                query[--qlen] = '\0';
            }
            selected = 0;
            needs_filter = true;
        } else if (ev.key == KEY_CHAR) {
            if (isprint((unsigned char)ev.ch) && qlen + 1 < sizeof(query)) {
                query[qlen++] = ev.ch;
                query[qlen] = '\0';
                selected = 0;
                needs_filter = true;
            }
        }
    }

    free(filtered);
    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);
    sigaction(SIGWINCH, &old_win, NULL);

    return result_path;
}

static bool confirm_fallback(const char *working_dir, const char *cmdline)
{
    fprintf(stderr, "jrun: run in %s?\n  %s\nConfirm [y/N]: ", working_dir, cmdline);
    fflush(stderr);
    char line[32];
    if (!fgets(line, sizeof(line), stdin)) return false;
    return line[0] == 'y' || line[0] == 'Y';
}

bool tui_confirm_command(const char *working_dir, char *const argv[])
{
    if (!working_dir || !argv || !argv[0]) return false;

    char cmdline[1024];
    join_argv(cmdline, sizeof(cmdline), argv);

    char short_dir[PATH_MAX];
    path_shorten_tilde(working_dir, short_dir, sizeof(short_dir));

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        jrun_log_error("refusing destructive command '%s' without a TTY (pass -y to skip)", argv[0]);
        return false;
    }

    if (!enable_raw_mode()) {
        return confirm_fallback(short_dir, cmdline);
    }

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    g_interrupted = 0;
    sigaction(SIGINT, &sa, &old_sa);

    bool confirmed = false;
    bool done = false;
    int choice = 0; // 0 = No, 1 = Yes

    while (!g_interrupted && !done) {
        int cols = 80, rows = 24;
        get_term_size(&cols, &rows);
        int box_width = cols;
        if (box_width > 100) box_width = 100;
        if (box_width < 40) box_width = cols;

        char safe_cmd[1024];
        char safe_dir[PATH_MAX];
        sanitize_display_string(safe_cmd, cmdline, sizeof(safe_cmd));
        sanitize_display_string(safe_dir, short_dir, sizeof(safe_dir));

        char buf[8192];
        size_t buf_pos = 0;
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[H\x1b[?25l");
        append_labeled_top(buf, &buf_pos, sizeof(buf), "Confirm destructive command", box_width);

        buf_append_str(buf, &buf_pos, sizeof(buf), "│ Directory: ");
        int dir_fill = box_width - 14 - utf8_visual_width(safe_dir);
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1m");
        buf_append_str(buf, &buf_pos, sizeof(buf), safe_dir);
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m");
        if (dir_fill < 0) dir_fill = 0;
        pad_to(buf, &buf_pos, sizeof(buf), dir_fill);
        buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");

        buf_append_str(buf, &buf_pos, sizeof(buf), "│ Command:   ");
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1;31m");
        int cmd_max = box_width - 14;
        if (utf8_visual_width(safe_cmd) > cmd_max && cmd_max > 3) {
            safe_cmd[cmd_max - 3] = '\0';
            buf_append_str(buf, &buf_pos, sizeof(buf), safe_cmd);
            buf_append_str(buf, &buf_pos, sizeof(buf), "...");
        } else {
            buf_append_str(buf, &buf_pos, sizeof(buf), safe_cmd);
            int cmd_fill = cmd_max - utf8_visual_width(safe_cmd);
            if (cmd_fill < 0) cmd_fill = 0;
            pad_to(buf, &buf_pos, sizeof(buf), cmd_fill);
        }
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m│\r\n");

        append_hline(buf, &buf_pos, sizeof(buf), "├", "┤", box_width);
        buf_append_str(buf, &buf_pos, sizeof(buf), "│ ");
        if (choice == 0) buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1;37;44m❯ No \x1b[0m  Yes ");
        else buf_append_str(buf, &buf_pos, sizeof(buf), "  No  \x1b[1;37;41m❯ Yes\x1b[0m");
        pad_to(buf, &buf_pos, sizeof(buf), box_width - 16);
        buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");

        append_hline(buf, &buf_pos, sizeof(buf), "├", "┤", box_width);
        const char *footer = "←→ choose   y confirm   n/Esc cancel";
        buf_appendf(buf, &buf_pos, sizeof(buf), "│ \x1b[90m%s\x1b[0m", footer);
        int ff = box_width - 4 - utf8_visual_width(footer);
        if (ff < 0) ff = 0;
        pad_to(buf, &buf_pos, sizeof(buf), ff);
        buf_append_str(buf, &buf_pos, sizeof(buf), " │\r\n");
        append_hline(buf, &buf_pos, sizeof(buf), "└", "┘", box_width);
        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[J");
        (void)write(STDOUT_FILENO, buf, buf_pos);
        (void)rows;

        Input_Event ev = read_input();
        if (ev.key == KEY_NONE) continue;
        if (ev.key == KEY_ESC) break;
        if (ev.key == KEY_LEFT || ev.key == KEY_UP) {
            choice = 0;
        } else if (ev.key == KEY_RIGHT || ev.key == KEY_DOWN) {
            choice = 1;
        } else if (ev.key == KEY_ENTER) {
            confirmed = (choice == 1);
            done = true;
        } else if (ev.key == KEY_CHAR) {
            if (ev.ch == 'y' || ev.ch == 'Y') {
                confirmed = true;
                done = true;
            } else if (ev.ch == 'n' || ev.ch == 'N') {
                confirmed = false;
                done = true;
            } else if (ev.ch == 'h') {
                choice = 0;
            } else if (ev.ch == 'l') {
                choice = 1;
            }
        }
    }

    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);
    return confirmed;
}

enum Cfg_Item_Type {
    CFG_HEADER = 0,
    CFG_ADD_ROOT,
    CFG_ROOT,
    CFG_BOOL,
    CFG_INT,
    CFG_DOUBLE,
    CFG_ADD_CMD,
    CFG_CMD,
};

enum Cfg_Bool_Field {
    CFG_FOLLOW_SYMLINKS = 0,
    CFG_FUZZY,
    CFG_INTERACTIVE,
    CFG_CONFIRM,
};

typedef struct {
    int type;
    const char *label;
    size_t index;
    int bool_field;
} Cfg_Item;

enum Cfg_Mode {
    CFG_MODE_NAV = 0,
    CFG_MODE_INPUT,
    CFG_MODE_QUIT,
};

static bool *cfg_bool_ptr(Jrun_Config *cfg, int field)
{
    switch (field) {
    case CFG_FOLLOW_SYMLINKS: return &cfg->follow_symlinks;
    case CFG_FUZZY: return &cfg->fuzzy;
    case CFG_INTERACTIVE: return &cfg->interactive;
    case CFG_CONFIRM: return &cfg->confirm_destructive;
    default: return NULL;
    }
}

static size_t cfg_build_items(Jrun_Config *cfg, Cfg_Item *items, size_t cap)
{
    size_t n = 0;
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_HEADER, .label = "Search roots  [search]" };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_ADD_ROOT, .label = "+ Add root…" };
    for (size_t i = 0; i < cfg->roots_count && n < cap; ++i) {
        items[n++] = (Cfg_Item){ .type = CFG_ROOT, .label = cfg->roots[i], .index = i };
    }
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_HEADER, .label = "Behavior  [behavior]" };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_BOOL, .label = "fuzzy", .bool_field = CFG_FUZZY };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_BOOL, .label = "interactive", .bool_field = CFG_INTERACTIVE };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_BOOL, .label = "follow_symlinks", .bool_field = CFG_FOLLOW_SYMLINKS };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_INT, .label = "max_depth" };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_DOUBLE, .label = "frecency_threshold" };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_HEADER, .label = "Safety  [safety]" };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_BOOL, .label = "confirm", .bool_field = CFG_CONFIRM };
    if (n < cap) items[n++] = (Cfg_Item){ .type = CFG_ADD_CMD, .label = "+ Add command…" };
    for (size_t i = 0; i < cfg->confirm_commands_count && n < cap; ++i) {
        items[n++] = (Cfg_Item){ .type = CFG_CMD, .label = cfg->confirm_commands[i], .index = i };
    }
    return n;
}

static bool cfg_item_selectable(int type)
{
    return type != CFG_HEADER;
}

static void cfg_skip(Cfg_Item *items, size_t count, size_t *sel, int dir)
{
    if (count == 0) return;
    for (size_t step = 0; step < count; ++step) {
        if (dir > 0) {
            *sel = (*sel + 1) % count;
        } else {
            *sel = (*sel == 0) ? count - 1 : *sel - 1;
        }
        if (cfg_item_selectable(items[*sel].type)) return;
    }
}

static bool cfg_commit_input(Jrun_Config *cfg, Cfg_Item *cur, const char *text, char *status, size_t status_sz)
{
    if (!text || text[0] == '\0') {
        snprintf(status, status_sz, "cancelled");
        return false;
    }
    if (cur->type == CFG_ADD_ROOT) {
        char shortp[PATH_MAX];
        char norm[PATH_MAX];
        const char *store = text;
        if (path_normalize(text, norm, sizeof(norm)) &&
            path_shorten_tilde(norm, shortp, sizeof(shortp))) {
            store = shortp;
        }
        if (config_add_root(cfg, store)) {
            snprintf(status, status_sz, "added root %s", store);
            return true;
        }
        snprintf(status, status_sz, "failed to add root");
        return false;
    }
    if (cur->type == CFG_ADD_CMD) {
        if (config_add_confirm_command(cfg, text)) {
            snprintf(status, status_sz, "added command %s", text);
            return true;
        }
        snprintf(status, status_sz, "failed to add command");
        return false;
    }
    if (cur->type == CFG_INT) {
        int v = atoi(text);
        if (v < 1 || v > 32) {
            snprintf(status, status_sz, "max_depth must be 1–32");
            return false;
        }
        cfg->max_depth = v;
        snprintf(status, status_sz, "max_depth = %d", v);
        return true;
    }
    if (cur->type == CFG_DOUBLE) {
        double v = atof(text);
        if (v <= 0.0) {
            snprintf(status, status_sz, "frecency_threshold must be > 0");
            return false;
        }
        cfg->frecency_threshold = v;
        snprintf(status, status_sz, "frecency_threshold = %.2f", v);
        return true;
    }
    return false;
}

bool tui_edit_config(Jrun_Config *config, const char *filepath)
{
    if (!config || !filepath) return false;
    if (!enable_raw_mode()) {
        jrun_log_error("failed to initialize terminal raw mode for config TUI");
        return false;
    }

    struct sigaction sa, old_sa, sa_win, old_win;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    g_interrupted = 0;
    sigaction(SIGINT, &sa, &old_sa);
    memset(&sa_win, 0, sizeof(sa_win));
    sa_win.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_win, &old_win);

    Cfg_Item items[256];
    size_t item_count = cfg_build_items(config, items, 256);
    size_t selected = 1;
    size_t scroll_offset = 0;
    int mode = CFG_MODE_NAV;
    int input_kind = CFG_ADD_ROOT;
    char input[PATH_MAX];
    size_t ilen = 0;
    input[0] = '\0';
    char status[256];
    snprintf(status, sizeof(status), "editing %s", filepath);
    bool dirty = false;
    bool running = true;
    bool needs_redraw = true;

    if (selected >= item_count || !cfg_item_selectable(items[selected].type)) {
        selected = 0;
        cfg_skip(items, item_count, &selected, 1);
    }

    while (!g_interrupted && running) {
        if (g_resized) {
            g_resized = 0;
            needs_redraw = true;
        }

        item_count = cfg_build_items(config, items, 256);
        if (selected >= item_count) selected = item_count ? item_count - 1 : 0;
        if (item_count > 0 && !cfg_item_selectable(items[selected].type)) {
            cfg_skip(items, item_count, &selected, 1);
        }

        int cols = 80, rows = 24;
        get_term_size(&cols, &rows);
        int box_width = cols;
        if (box_width < 40) box_width = 40;
        int list_height = rows - 6;
        if (list_height < 5) list_height = 5;

        if (selected < scroll_offset) scroll_offset = selected;
        else if (selected >= scroll_offset + (size_t)list_height) {
            scroll_offset = selected - (size_t)list_height + 1;
        }

        if (needs_redraw) {
            char buf[16384];
            size_t buf_pos = 0;
            buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[H\x1b[?25l");
            char title[96];
            snprintf(title, sizeof(title), "jrun config%s", dirty ? "  • modified" : "  • saved");
            append_labeled_top(buf, &buf_pos, sizeof(buf), title, box_width);

            for (int row = 0; row < list_height; ++row) {
                size_t idx = scroll_offset + (size_t)row;
                buf_append_str(buf, &buf_pos, sizeof(buf), "│");
                int inner = box_width - 2;
                if (idx < item_count) {
                    Cfg_Item *it = &items[idx];
                    bool is_sel = (idx == selected && mode != CFG_MODE_QUIT);
                    char line[PATH_MAX + 64];
                    line[0] = '\0';
                    if (it->type == CFG_HEADER) {
                        snprintf(line, sizeof(line), "%s", it->label);
                    } else if (it->type == CFG_BOOL) {
                        bool *p = cfg_bool_ptr(config, it->bool_field);
                        snprintf(line, sizeof(line), "%-22s %s", it->label, (p && *p) ? "true" : "false");
                    } else if (it->type == CFG_INT) {
                        snprintf(line, sizeof(line), "%-22s %d", it->label, config->max_depth);
                    } else if (it->type == CFG_DOUBLE) {
                        snprintf(line, sizeof(line), "%-22s %.2f", it->label, config->frecency_threshold);
                    } else if (it->type == CFG_ROOT) {
                        char expanded[PATH_MAX];
                        const char *note = "";
                        if (path_expand_tilde(it->label, expanded, sizeof(expanded))) {
                            note = path_is_dir(expanded) ? "" : "  (missing)";
                        }
                        snprintf(line, sizeof(line), "  %s%s", it->label, note);
                    } else if (it->type == CFG_CMD) {
                        snprintf(line, sizeof(line), "  %s", it->label);
                    } else {
                        snprintf(line, sizeof(line), "%s", it->label);
                    }

                    char safe[PATH_MAX + 64];
                    sanitize_display_string(safe, line, sizeof(safe));
                    int vis = utf8_visual_width(safe);
                    if (vis > inner - 2) {
                        safe[inner - 5] = '\0';
                        vis = utf8_visual_width(safe);
                    }

                    if (it->type == CFG_HEADER) {
                        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1;36m ");
                        buf_append_str(buf, &buf_pos, sizeof(buf), safe);
                        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m");
                        pad_to(buf, &buf_pos, sizeof(buf), inner - 1 - vis);
                    } else if (is_sel) {
                        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1;37;44m❯");
                        buf_append_str(buf, &buf_pos, sizeof(buf), safe);
                        pad_to(buf, &buf_pos, sizeof(buf), inner - 1 - vis);
                        buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m");
                    } else {
                        buf_append_str(buf, &buf_pos, sizeof(buf), " ");
                        buf_append_str(buf, &buf_pos, sizeof(buf), safe);
                        pad_to(buf, &buf_pos, sizeof(buf), inner - 1 - vis);
                    }
                } else {
                    pad_to(buf, &buf_pos, sizeof(buf), inner);
                }
                buf_append_str(buf, &buf_pos, sizeof(buf), "│\r\n");
            }

            append_hline(buf, &buf_pos, sizeof(buf), "├", "┤", box_width);

            char prompt[PATH_MAX + 32];
            if (mode == CFG_MODE_INPUT) {
                snprintf(prompt, sizeof(prompt), "%s%s",
                         (input_kind == CFG_ADD_ROOT) ? "Root path: " :
                         (input_kind == CFG_ADD_CMD) ? "Command: " : "Value: ",
                         input);
            } else if (mode == CFG_MODE_QUIT) {
                snprintf(prompt, sizeof(prompt), "Unsaved changes. Save?  y / n / esc");
            } else {
                snprintf(prompt, sizeof(prompt), "%s", status);
            }
            char safe_p[PATH_MAX + 32];
            sanitize_display_string(safe_p, prompt, sizeof(safe_p));
            buf_append_str(buf, &buf_pos, sizeof(buf), "│ ");
            if (mode == CFG_MODE_QUIT) buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1;33m");
            else if (mode == CFG_MODE_INPUT) buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[1m");
            buf_append_str(buf, &buf_pos, sizeof(buf), safe_p);
            buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[0m");
            int pf = box_width - 4 - utf8_visual_width(safe_p);
            if (pf < 0) pf = 0;
            pad_to(buf, &buf_pos, sizeof(buf), pf);
            buf_append_str(buf, &buf_pos, sizeof(buf), " │\r\n");

            append_hline(buf, &buf_pos, sizeof(buf), "├", "┤", box_width);
            const char *footer = "↑↓  Enter/Space  a add  d del  +/-  C-s save  q quit";
            buf_appendf(buf, &buf_pos, sizeof(buf), "│ \x1b[90m%s\x1b[0m", footer);
            int ff = box_width - 4 - utf8_visual_width(footer);
            if (ff < 0) ff = 0;
            pad_to(buf, &buf_pos, sizeof(buf), ff);
            buf_append_str(buf, &buf_pos, sizeof(buf), " │\r\n");
            append_hline(buf, &buf_pos, sizeof(buf), "└", "┘", box_width);
            buf_append_str(buf, &buf_pos, sizeof(buf), "\x1b[J");

            if (mode == CFG_MODE_INPUT) {
                int col = 3 + utf8_visual_width(safe_p);
                buf_appendf(buf, &buf_pos, sizeof(buf), "\x1b[%d;%dH\x1b[?25h", rows - 3, col);
            }

            (void)write(STDOUT_FILENO, buf, buf_pos);
            needs_redraw = false;
        }

        Input_Event ev = read_input();
        if (ev.key == KEY_NONE) continue;
        needs_redraw = true;

        if (mode == CFG_MODE_INPUT) {
            if (ev.key == KEY_ESC) {
                mode = CFG_MODE_NAV;
                snprintf(status, sizeof(status), "cancelled");
            } else if (ev.key == KEY_ENTER) {
                Cfg_Item dummy = { .type = input_kind };
                if (selected < item_count &&
                    (items[selected].type == CFG_INT || items[selected].type == CFG_DOUBLE ||
                     items[selected].type == CFG_ADD_ROOT || items[selected].type == CFG_ADD_CMD)) {
                    dummy = items[selected];
                } else {
                    dummy.type = input_kind;
                }
                if (cfg_commit_input(config, &dummy, input, status, sizeof(status))) {
                    dirty = true;
                }
                mode = CFG_MODE_NAV;
                ilen = 0;
                input[0] = '\0';
            } else if (ev.key == KEY_BACKSPACE) {
                if (ilen > 0) input[--ilen] = '\0';
            } else if (ev.key == KEY_CLEAR) {
                ilen = 0;
                input[0] = '\0';
            } else if (ev.key == KEY_CHAR && isprint((unsigned char)ev.ch) && ilen + 1 < sizeof(input)) {
                input[ilen++] = ev.ch;
                input[ilen] = '\0';
            }
            continue;
        }

        if (mode == CFG_MODE_QUIT) {
            if (ev.key == KEY_ESC) {
                mode = CFG_MODE_NAV;
                snprintf(status, sizeof(status), "still editing");
            } else if (ev.key == KEY_CHAR && (ev.ch == 'y' || ev.ch == 'Y' || ev.ch == 's' || ev.ch == 'S')) {
                if (config_save(config, filepath)) {
                    dirty = false;
                    running = false;
                } else {
                    snprintf(status, sizeof(status), "save failed");
                    mode = CFG_MODE_NAV;
                }
            } else if (ev.key == KEY_CHAR && (ev.ch == 'n' || ev.ch == 'N')) {
                config_free(config);
                config_load(config, filepath);
                dirty = false;
                running = false;
            }
            continue;
        }

        Cfg_Item *cur = (selected < item_count) ? &items[selected] : NULL;

        if (ev.key == KEY_ESC || (ev.key == KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q'))) {
            if (dirty) mode = CFG_MODE_QUIT;
            else running = false;
        } else if (ev.key == KEY_SAVE || (ev.key == KEY_CHAR && ev.ch == 's')) {
            if (config_save(config, filepath)) {
                dirty = false;
                snprintf(status, sizeof(status), "wrote %s", filepath);
            } else {
                snprintf(status, sizeof(status), "save failed");
            }
        } else if (ev.key == KEY_UP) {
            cfg_skip(items, item_count, &selected, -1);
        } else if (ev.key == KEY_DOWN) {
            cfg_skip(items, item_count, &selected, 1);
        } else if (ev.key == KEY_TAB) {
            size_t start = selected;
            do {
                cfg_skip(items, item_count, &selected, 1);
                if (items[selected].type == CFG_ADD_ROOT ||
                    (items[selected].type == CFG_BOOL && items[selected].bool_field == CFG_FUZZY) ||
                    (items[selected].type == CFG_BOOL && items[selected].bool_field == CFG_CONFIRM)) {
                    break;
                }
            } while (selected != start);
        } else if (ev.key == KEY_HOME) {
            selected = 0;
            cfg_skip(items, item_count, &selected, 1);
        } else if (ev.key == KEY_END) {
            selected = item_count ? item_count - 1 : 0;
            if (item_count && !cfg_item_selectable(items[selected].type)) {
                cfg_skip(items, item_count, &selected, -1);
            }
        } else if (!cur) {
            continue;
        } else if (ev.key == KEY_ENTER || (ev.key == KEY_CHAR && ev.ch == ' ')) {
            if (cur->type == CFG_BOOL) {
                bool *p = cfg_bool_ptr(config, cur->bool_field);
                if (p) {
                    *p = !*p;
                    dirty = true;
                    snprintf(status, sizeof(status), "%s = %s", cur->label, *p ? "true" : "false");
                }
            } else if (cur->type == CFG_ADD_ROOT || cur->type == CFG_ADD_CMD ||
                       cur->type == CFG_INT || cur->type == CFG_DOUBLE) {
                mode = CFG_MODE_INPUT;
                input_kind = cur->type;
                ilen = 0;
                input[0] = '\0';
                if (cur->type == CFG_INT) {
                    ilen = (size_t)snprintf(input, sizeof(input), "%d", config->max_depth);
                } else if (cur->type == CFG_DOUBLE) {
                    ilen = (size_t)snprintf(input, sizeof(input), "%.2f", config->frecency_threshold);
                }
            }
        } else if (ev.key == KEY_CHAR && ev.ch == 'a') {
            mode = CFG_MODE_INPUT;
            input_kind = (cur->type == CFG_CMD || cur->type == CFG_ADD_CMD) ? CFG_ADD_CMD : CFG_ADD_ROOT;
            ilen = 0;
            input[0] = '\0';
        } else if ((ev.key == KEY_BACKSPACE || (ev.key == KEY_CHAR && (ev.ch == 'd' || ev.ch == 'D' || ev.ch == 'x')))
                   && (cur->type == CFG_ROOT || cur->type == CFG_CMD)) {
            if (cur->type == CFG_ROOT) {
                snprintf(status, sizeof(status), "removed root %s", cur->label);
                config_remove_root_at(config, cur->index);
            } else {
                snprintf(status, sizeof(status), "removed command %s", cur->label);
                config_remove_confirm_command_at(config, cur->index);
            }
            dirty = true;
            cfg_skip(items, item_count, &selected, -1);
        } else if (cur->type == CFG_INT && (ev.key == KEY_LEFT || ev.key == KEY_RIGHT ||
                   (ev.key == KEY_CHAR && (ev.ch == '-' || ev.ch == '+')))) {
            int delta = (ev.key == KEY_LEFT || ev.ch == '-') ? -1 : 1;
            int v = config->max_depth + delta;
            if (v < 1) v = 1;
            if (v > 32) v = 32;
            if (v != config->max_depth) {
                config->max_depth = v;
                dirty = true;
                snprintf(status, sizeof(status), "max_depth = %d", v);
            }
        } else if (cur->type == CFG_DOUBLE && (ev.key == KEY_LEFT || ev.key == KEY_RIGHT ||
                   (ev.key == KEY_CHAR && (ev.ch == '-' || ev.ch == '+')))) {
            double delta = (ev.key == KEY_LEFT || ev.ch == '-') ? -0.25 : 0.25;
            double v = config->frecency_threshold + delta;
            if (v < 0.25) v = 0.25;
            if (v != config->frecency_threshold) {
                config->frecency_threshold = v;
                dirty = true;
                snprintf(status, sizeof(status), "frecency_threshold = %.2f", v);
            }
        } else if (cur->type == CFG_BOOL && (ev.key == KEY_LEFT || ev.key == KEY_RIGHT)) {
            bool *p = cfg_bool_ptr(config, cur->bool_field);
            if (p) {
                *p = !*p;
                dirty = true;
                snprintf(status, sizeof(status), "%s = %s", cur->label, *p ? "true" : "false");
            }
        }
    }

    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);
    sigaction(SIGWINCH, &old_win, NULL);
    return true;
}
