#include "tui.h"
#include "common.h"
#include "matcher.h"
#include "path_util.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Terminal handle
//
// Everything below talks to /dev/tty rather than stdin/stdout. jrun is almost
// always invoked as `dir=$(jrun --cd foo)` by the shell integration, so stdout
// is a pipe carrying the result; drawing there would corrupt the output and
// gating on isatty(stdout) would disable the selector entirely.
// ---------------------------------------------------------------------------

typedef struct {
    int in_fd;
    int out_fd;
    bool owns_fd;
    struct termios orig;
    bool raw;
} Tty;

static Tty g_tty = { .in_fd = -1, .out_fd = -1, .owns_fd = false, .raw = false };
static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_resized = 0;

static void tty_write(const char *s, size_t len)
{
    if (g_tty.out_fd < 0) return;
    while (len > 0) {
        ssize_t n = write(g_tty.out_fd, s, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += n;
        len -= (size_t)n;
    }
}

static void tty_close(void)
{
    if (g_tty.owns_fd && g_tty.in_fd >= 0) {
        close(g_tty.in_fd);
    }
    g_tty.in_fd = -1;
    g_tty.out_fd = -1;
    g_tty.owns_fd = false;
}

// Opens the controlling terminal, falling back to the standard streams when
// /dev/tty is unavailable (some containers, some CI runners).
static bool tty_open(void)
{
    if (g_tty.in_fd >= 0) return true;

    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        g_tty.in_fd = fd;
        g_tty.out_fd = fd;
        g_tty.owns_fd = true;
        return true;
    }

    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        g_tty.in_fd = STDIN_FILENO;
        g_tty.out_fd = STDOUT_FILENO;
        g_tty.owns_fd = false;
        return true;
    }

    return false;
}

bool tui_available(void)
{
    if (g_tty.in_fd >= 0) return true;

    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static void disable_raw_mode(void)
{
    if (!g_tty.raw) return;
    const char *exit_seq = "\x1b[0m\x1b[?25h\x1b[?1049l";
    tty_write(exit_seq, strlen(exit_seq));
    tcsetattr(g_tty.in_fd, TCSAFLUSH, &g_tty.orig);
    g_tty.raw = false;
    tty_close();
}

// A terminal left in raw mode with the alternate screen active is a wrecked
// shell, so restore on every exit path we can observe.
static void fatal_signal_handler(int sig)
{
    disable_raw_mode();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_exit_guards(void)
{
    static bool installed = false;
    if (installed) return;
    installed = true;
    atexit(disable_raw_mode);
    signal(SIGTERM, fatal_signal_handler);
    signal(SIGHUP, fatal_signal_handler);
    signal(SIGSEGV, fatal_signal_handler);
    signal(SIGABRT, fatal_signal_handler);
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

static bool enable_raw_mode(void)
{
    if (g_tty.raw) return true;
    if (!tty_open()) return false;

    if (tcgetattr(g_tty.in_fd, &g_tty.orig) == -1) {
        tty_close();
        return false;
    }
    install_exit_guards();

    struct termios raw = g_tty.orig;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // Blocking reads; poll() decides when input is available, so the process
    // uses no CPU at all while it waits.
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(g_tty.in_fd, TCSAFLUSH, &raw) == -1) {
        tty_close();
        return false;
    }
    g_tty.raw = true;

    const char *enter_seq = "\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l";
    tty_write(enter_seq, strlen(enter_seq));
    return true;
}

// ---------------------------------------------------------------------------
// Growable render buffer
//
// A fixed buffer silently truncated frames on wide terminals, and truncation
// in the middle of an escape sequence leaves the display garbled.
// ---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} Buf;

static void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->failed = false;
}

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->failed) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t new_cap = b->cap ? b->cap : 8192;
    while (new_cap < b->len + extra + 1) new_cap *= 2;
    char *grown = (char *)realloc(b->data, new_cap);
    if (!grown) { b->failed = true; return; }
    b->data = grown;
    b->cap = new_cap;
}

static void buf_putn(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    if (b->failed) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s)
{
    if (s) buf_putn(b, s, strlen(s));
}

static void buf_putf(Buf *b, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        buf_putn(b, tmp, (size_t)n);
        return;
    }
    // Rare long line: render it again into an exactly-sized allocation.
    buf_reserve(b, (size_t)n);
    if (b->failed) return;
    va_start(args, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, args);
    va_end(args);
    b->len += (size_t)n;
}

static void buf_repeat(Buf *b, const char *unit, int times)
{
    for (int i = 0; i < times; ++i) buf_puts(b, unit);
}

static void buf_flush(Buf *b)
{
    if (!b->failed && b->data) tty_write(b->data, b->len);
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

// ---------------------------------------------------------------------------
// UTF-8 aware measurement
// ---------------------------------------------------------------------------

// Decodes one code point. Always advances by at least one byte so malformed
// input cannot stall a render loop.
static size_t utf8_next(const char *s, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
              (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

// Width tables, rather than wcwidth().
//
// wcwidth() answers according to the process locale: in the "C" locale it
// reports -1 for every non-ASCII code point, so CJK names rendered two columns
// wide by the terminal were measured as one and every row containing one came
// out too long. These ranges are locale independent and cover what actually
// turns up in directory names.

typedef struct { uint32_t lo, hi; } Cp_Range;

// Combining marks and zero-width formatting characters occupy no column.
static const Cp_Range ZERO_WIDTH[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
    { 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 },
    { 0x06D6, 0x06DC }, { 0x0900, 0x0903 }, { 0x093A, 0x094F },
    { 0x0951, 0x0957 }, { 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF },
    { 0x200B, 0x200F }, { 0x2060, 0x2064 }, { 0x20D0, 0x20F0 },
    { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF },
    { 0xE0100, 0xE01EF },
};

// East Asian Wide and Fullwidth, plus the emoji blocks terminals render wide.
static const Cp_Range DOUBLE_WIDTH[] = {
    { 0x1100, 0x115F },   // Hangul Jamo initial consonants
    { 0x2329, 0x232A },
    { 0x2E80, 0x303E },   // CJK radicals, Kangxi, CJK symbols
    { 0x3041, 0x33FF },   // Hiragana, Katakana, Hangul Compatibility, CJK compat
    { 0x3400, 0x4DBF },   // CJK Extension A
    { 0x4E00, 0x9FFF },   // CJK Unified Ideographs
    { 0xA000, 0xA4CF },   // Yi
    { 0xAC00, 0xD7A3 },   // Hangul syllables
    { 0xF900, 0xFAFF },   // CJK Compatibility Ideographs
    { 0xFE10, 0xFE19 },
    { 0xFE30, 0xFE6F },   // CJK Compatibility Forms
    { 0xFF00, 0xFF60 },   // Fullwidth forms
    { 0xFFE0, 0xFFE6 },
    { 0x16FE0, 0x16FE4 },
    { 0x17000, 0x18AFF }, // Tangut
    { 0x1B000, 0x1B2FF },
    { 0x1F004, 0x1F004 },
    { 0x1F0CF, 0x1F0CF },
    { 0x1F18E, 0x1F18E },
    { 0x1F191, 0x1F19A },
    { 0x1F200, 0x1F320 },
    { 0x1F330, 0x1F335 },
    { 0x1F337, 0x1F37C },
    { 0x1F380, 0x1F393 },
    { 0x1F3A0, 0x1F3CA },
    { 0x1F400, 0x1F4FD },
    { 0x1F500, 0x1F53D },
    { 0x1F550, 0x1F567 },
    { 0x1F600, 0x1F64F },
    { 0x1F680, 0x1F6C5 },
    { 0x1F900, 0x1F9FF },
    { 0x20000, 0x3FFFD }, // CJK Extension B and beyond
};

static bool cp_in(const Cp_Range *ranges, size_t n, uint32_t cp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].lo) hi = mid;
        else if (cp > ranges[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

static int cp_width(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp < 0x20 || cp == 0x7f) return 0;
    if (cp < 0x0300) return 1;  // the overwhelmingly common case
    if (cp_in(ZERO_WIDTH, sizeof(ZERO_WIDTH) / sizeof(ZERO_WIDTH[0]), cp)) return 0;
    if (cp_in(DOUBLE_WIDTH, sizeof(DOUBLE_WIDTH) / sizeof(DOUBLE_WIDTH[0]), cp)) return 2;
    return 1;
}

static int str_width(const char *s)
{
    int w = 0;
    while (*s) {
        uint32_t cp;
        s += utf8_next(s, &cp);
        w += cp_width(cp);
    }
    return w;
}

// Byte offset at which the trailing `max_w` columns of `s` begin. Sets
// *needs_ellipsis when the head was cut off.
static size_t fit_tail(const char *s, int max_w, bool *needs_ellipsis)
{
    *needs_ellipsis = false;
    int total = str_width(s);
    if (total <= max_w) return 0;

    int budget = max_w - 1; // one column for the ellipsis
    if (budget < 1) budget = 1;

    // Walk forward recording offsets, then pick the first one whose remaining
    // width fits. Paths are short enough that this stays trivial.
    size_t offset = 0;
    int remaining = total;
    while (s[offset] && remaining > budget) {
        uint32_t cp;
        size_t adv = utf8_next(s + offset, &cp);
        remaining -= cp_width(cp);
        offset += adv;
    }
    *needs_ellipsis = true;
    return offset;
}

static void sanitize_display_string(char *dest, const char *src, size_t dest_size)
{
    if (!dest || dest_size == 0) return;
    if (!src) { dest[0] = '\0'; return; }
    // One byte in, one byte out, so highlight offsets computed against the
    // sanitized string still line up with the original.
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 1 < dest_size; ++s) {
        unsigned char c = (unsigned char)src[s];
        dest[d++] = (c < 32 || c == 127) ? '?' : (char)c;
    }
    dest[d] = '\0';
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

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
    KEY_DELETE,
    KEY_CLEAR,
    KEY_KILL_WORD,
    KEY_LINE_START,
    KEY_LINE_END,
    KEY_TAB,
    KEY_SAVE,
    KEY_REFRESH,
    KEY_ESC,
};

typedef struct {
    int key;
    char ch;
} Input_Event;

// Waits for input, returning early when a signal arrives so a resize repaints
// promptly. timeout_ms < 0 blocks indefinitely; the process is idle meanwhile.
static bool tty_wait(int timeout_ms)
{
    struct pollfd pfd = { .fd = g_tty.in_fd, .events = POLLIN, .revents = 0 };
    int rc = poll(&pfd, 1, timeout_ms);
    return rc > 0 && (pfd.revents & POLLIN);
}

static bool tty_read_byte(char *out, int timeout_ms)
{
    if (!tty_wait(timeout_ms)) return false;
    ssize_t n = read(g_tty.in_fd, out, 1);
    return n == 1;
}

// How long to wait for the rest of an escape sequence before concluding the
// user simply pressed Esc.
#define ESC_SEQ_TIMEOUT_MS 40

static Input_Event read_input(void)
{
    char c = 0;
    if (!tty_read_byte(&c, -1)) return (Input_Event){ .key = KEY_NONE };

    if (c == '\x1b') {
        char seq[8] = {0};
        if (!tty_read_byte(&seq[0], ESC_SEQ_TIMEOUT_MS)) return (Input_Event){ .key = KEY_ESC };

        if (seq[0] == '[') {
            if (!tty_read_byte(&seq[1], ESC_SEQ_TIMEOUT_MS)) return (Input_Event){ .key = KEY_ESC };

            if (seq[1] >= '0' && seq[1] <= '9') {
                // Numeric form: ESC [ <digits> (;<mods>) ~
                char digits[8] = { seq[1], 0 };
                size_t d = 1;
                char t = 0;
                while (d + 1 < sizeof(digits) && tty_read_byte(&t, ESC_SEQ_TIMEOUT_MS)) {
                    if (t >= '0' && t <= '9') { digits[d++] = t; digits[d] = '\0'; continue; }
                    break;
                }
                if (t == ';') {
                    // Modifier parameters; consume and ignore them.
                    while (tty_read_byte(&t, ESC_SEQ_TIMEOUT_MS) && t != '~' &&
                           !(t >= 'A' && t <= 'Z')) {
                        // keep draining
                    }
                }
                int code = atoi(digits);
                switch (code) {
                case 1: case 7: return (Input_Event){ .key = KEY_HOME };
                case 3: return (Input_Event){ .key = KEY_DELETE };
                case 4: case 8: return (Input_Event){ .key = KEY_END };
                case 5: return (Input_Event){ .key = KEY_PAGE_UP };
                case 6: return (Input_Event){ .key = KEY_PAGE_DOWN };
                default: return (Input_Event){ .key = KEY_NONE };
                }
            }

            switch (seq[1]) {
            case 'A': return (Input_Event){ .key = KEY_UP };
            case 'B': return (Input_Event){ .key = KEY_DOWN };
            case 'C': return (Input_Event){ .key = KEY_RIGHT };
            case 'D': return (Input_Event){ .key = KEY_LEFT };
            case 'H': return (Input_Event){ .key = KEY_HOME };
            case 'F': return (Input_Event){ .key = KEY_END };
            default:  return (Input_Event){ .key = KEY_NONE };
            }
        }

        if (seq[0] == 'O') {
            if (!tty_read_byte(&seq[1], ESC_SEQ_TIMEOUT_MS)) return (Input_Event){ .key = KEY_ESC };
            switch (seq[1]) {
            case 'A': return (Input_Event){ .key = KEY_UP };
            case 'B': return (Input_Event){ .key = KEY_DOWN };
            case 'C': return (Input_Event){ .key = KEY_RIGHT };
            case 'D': return (Input_Event){ .key = KEY_LEFT };
            case 'H': return (Input_Event){ .key = KEY_HOME };
            case 'F': return (Input_Event){ .key = KEY_END };
            default:  return (Input_Event){ .key = KEY_NONE };
            }
        }

        return (Input_Event){ .key = KEY_ESC };
    }

    switch (c) {
    case 13:  return (Input_Event){ .key = KEY_ENTER };
    case 127: case 8:  return (Input_Event){ .key = KEY_BACKSPACE };
    case 1:   return (Input_Event){ .key = KEY_LINE_START };  // C-a
    case 3:   return (Input_Event){ .key = KEY_ESC };         // C-c
    case 4:   return (Input_Event){ .key = KEY_ESC };         // C-d
    case 5:   return (Input_Event){ .key = KEY_LINE_END };    // C-e
    case 9:   return (Input_Event){ .key = KEY_TAB };
    case 10:  return (Input_Event){ .key = KEY_DOWN };        // C-j
    case 11:  return (Input_Event){ .key = KEY_UP };          // C-k
    case 12:  return (Input_Event){ .key = KEY_REFRESH };     // C-l
    case 14:  return (Input_Event){ .key = KEY_DOWN };        // C-n
    case 16:  return (Input_Event){ .key = KEY_UP };          // C-p
    case 18:  return (Input_Event){ .key = KEY_REFRESH };     // C-r
    case 19:  return (Input_Event){ .key = KEY_SAVE };        // C-s
    case 21:  return (Input_Event){ .key = KEY_CLEAR };       // C-u
    case 23:  return (Input_Event){ .key = KEY_KILL_WORD };   // C-w
    default: break;
    }

    return (Input_Event){ .key = KEY_CHAR, .ch = c };
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

#define SGR_RESET     "\x1b[0m"
#define SGR_DIM       "\x1b[2m"
#define SGR_BOLD      "\x1b[1m"
#define SGR_SELECTED  "\x1b[7m"
#define SGR_UNSEL     "\x1b[27m"
#define SGR_ACCENT    "\x1b[36m"
#define SGR_MATCH     "\x1b[1;33m"
#define SGR_MATCH_SEL "\x1b[1;4m"
#define SGR_WARN      "\x1b[1;33m"
#define SGR_DANGER    "\x1b[1;31m"

typedef struct {
    int cols;
    int rows;
} Term_Size;

static Term_Size get_term_size(void)
{
    Term_Size ts = { .cols = 80, .rows = 24 };
    struct winsize ws;
    if (g_tty.out_fd >= 0 && ioctl(g_tty.out_fd, TIOCGWINSZ, &ws) != -1 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        ts.cols = ws.ws_col;
        ts.rows = ws.ws_row;
    }
    // Cap the frame on very wide terminals so lines stay scannable. The lower
    // bound is a genuine floor, not a minimum width: forcing 40 columns on a
    // 30-column terminal makes every row wrap and destroys the layout.
    if (ts.cols > 160) ts.cols = 160;
    if (ts.cols < 20) ts.cols = 20;
    if (ts.rows < 6) ts.rows = 6;
    return ts;
}

static void draw_rule(Buf *b, const char *left, const char *right, int width)
{
    buf_puts(b, left);
    buf_repeat(b, "─", width - 2);
    buf_puts(b, right);
    buf_puts(b, "\r\n");
}

// Top border carrying a title on the left and an optional counter on the right.
static void draw_title(Buf *b, const char *title, const char *badge, int width)
{
    int title_w = str_width(title);
    int badge_w = badge ? str_width(badge) + 2 : 0;

    // Drop the badge, then trim the title, rather than overflowing the frame.
    if (4 + title_w + badge_w + 1 > width) {
        badge = NULL;
        badge_w = 0;
    }

    buf_puts(b, "┌─ " SGR_BOLD);
    int room = width - 5 - badge_w;
    if (room < 1) room = 1;
    bool ell = false;
    size_t from = fit_tail(title, room, &ell);
    int drawn = 0;
    if (ell) { buf_puts(b, "…"); drawn = 1; }
    for (size_t i = from; title[i] && drawn < room; ) {
        uint32_t cp;
        size_t adv = utf8_next(title + i, &cp);
        int w = cp_width(cp);
        if (drawn + w > room) break;
        buf_putn(b, title + i, adv);
        drawn += w;
        i += adv;
    }
    buf_puts(b, SGR_RESET " ");

    int used = 4 + drawn;
    int fill = width - used - badge_w - 1;
    if (fill < 0) fill = 0;
    buf_repeat(b, "─", fill);
    if (badge) {
        buf_putf(b, " " SGR_DIM "%s" SGR_RESET " ", badge);
    }
    buf_puts(b, "┐\r\n");
}

static void draw_pad(Buf *b, int n)
{
    for (int i = 0; i < n; ++i) buf_puts(b, " ");
}

// Writes one framed row: "│" + content padded to `inner` columns + "│".
// The caller emits content and reports the columns it used.
static void draw_row_end(Buf *b, int inner, int used)
{
    int pad = inner - used;
    if (pad < 0) pad = 0;
    draw_pad(b, pad);
    buf_puts(b, SGR_RESET "│\r\n");
}

// ---------------------------------------------------------------------------
// Directory selector
// ---------------------------------------------------------------------------

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

// Renders a path, colouring the bytes the query actually matched. `mask` is
// indexed by byte offset into `text`; `from` is where truncation begins.
static int draw_highlighted(Buf *b, const char *text, const unsigned char *mask,
                            size_t from, bool ellipsis, int max_w, bool selected)
{
    int used = 0;
    const char *normal = selected ? SGR_UNSEL SGR_SELECTED : SGR_RESET;
    const char *accent = selected ? SGR_MATCH_SEL : SGR_MATCH;

    if (ellipsis) {
        buf_puts(b, SGR_DIM "…");
        buf_puts(b, normal);
        used += 1;
    }

    bool in_match = false;
    for (size_t i = from; text[i] && used < max_w; ) {
        uint32_t cp;
        size_t adv = utf8_next(text + i, &cp);
        int w = cp_width(cp);
        if (used + w > max_w) break;

        bool hit = mask && mask[i];
        if (hit && !in_match) {
            buf_puts(b, accent);
            in_match = true;
        } else if (!hit && in_match) {
            buf_puts(b, normal);
            in_match = false;
        }

        buf_putn(b, text + i, adv);
        used += w;
        i += adv;
    }
    if (in_match) buf_puts(b, normal);
    return used;
}

// A one-column scrollbar drawn just inside the right border, so a long list
// shows at a glance how much of it is off screen.
static const char *scrollbar_cell(size_t total, size_t visible, size_t offset, int row, int height)
{
    if (total <= visible) return " ";
    // Thumb size and position, both clamped to at least one cell.
    int thumb = (int)((double)visible / (double)total * height);
    if (thumb < 1) thumb = 1;
    int max_off = (int)(total - visible);
    int pos = max_off > 0 ? (int)((double)offset / (double)max_off * (height - thumb)) : 0;
    return (row >= pos && row < pos + thumb) ? "█" : "░";
}

// The selector behind tui_select() and tui_select_many(). With `marks`
// non-NULL, Tab marks rows and marks[i] is set for each marked candidate.
// Returns the index of the candidate under the cursor when Enter is pressed,
// or -1 when cancelled or nothing was left to pick.
static ssize_t run_selector(const Resolve_Candidate *candidates, size_t count,
                            const char *initial_query, unsigned char *marks)
{
    if (!candidates || count == 0) return -1;

    if (!enable_raw_mode()) {
        jrun_log_error("no terminal available for the interactive selector");
        return -1;
    }

    struct sigaction sa, old_sa, sa_win, old_win;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    g_interrupted = 0;
    g_resized = 0;
    sigaction(SIGINT, &sa, &old_sa);
    memset(&sa_win, 0, sizeof(sa_win));
    sa_win.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_win, &old_win);

    char query[256];
    size_t qlen = 0;
    size_t qcursor = 0;
    if (initial_query) {
        snprintf(query, sizeof(query), "%s", initial_query);
        qlen = strlen(query);
    } else {
        query[0] = '\0';
    }
    qcursor = qlen;

    Filtered_Item *filtered = (Filtered_Item *)malloc(count * sizeof(Filtered_Item));
    Buf buf = {0};
    if (!filtered) {
        disable_raw_mode();
        sigaction(SIGINT, &old_sa, NULL);
        sigaction(SIGWINCH, &old_win, NULL);
        return -1;
    }

    size_t selected = 0;
    size_t scroll_offset = 0;
    ssize_t result = -1;
    size_t marked_count = 0;
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
                        // Scale the resolver's score by how well this row
                        // matches the live query. Ranking on an unrelated
                        // formula would order the list in a way the score
                        // column visibly contradicts.
                        double quality = m.quality_score / 150.0;
                        if (quality > 1.0) quality = 1.0;
                        if (quality < 0.05) quality = 0.05;
                        filtered[filtered_count].index = i;
                        filtered[filtered_count].score = candidates[i].score * quality;
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

        Term_Size ts = get_term_size();
        int width = ts.cols;
        int inner = width - 2;

        // Frame chrome: title, query, two rules, footer, bottom.
        const int chrome = 6;
        int list_height = ts.rows - chrome;
        if (list_height < 3) list_height = 3;
        if ((size_t)list_height > filtered_count && filtered_count > 0) {
            list_height = (int)filtered_count;
        }
        if (list_height < 1) list_height = 1;

        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + (size_t)list_height) {
            scroll_offset = selected - (size_t)list_height + 1;
        }
        if (filtered_count <= (size_t)list_height) scroll_offset = 0;

        if (needs_redraw) {
            buf.len = 0;
            buf_puts(&buf, "\x1b[H\x1b[?25l");

            char badge[64];
            if (marked_count > 0) {
                snprintf(badge, sizeof(badge), "%zu/%zu · %zu marked",
                         filtered_count, count, marked_count);
            } else {
                snprintf(badge, sizeof(badge), "%zu/%zu", filtered_count, count);
            }
            draw_title(&buf, marks ? "Run in" : "Jump to", badge, width);

            // Query line.
            char safe_query[256];
            sanitize_display_string(safe_query, query, sizeof(safe_query));
            buf_puts(&buf, "│ ");
            buf_puts(&buf, filtered_count ? SGR_ACCENT "❯ " SGR_RESET : SGR_DANGER "❯ " SGR_RESET);
            buf_puts(&buf, SGR_BOLD);
            buf_puts(&buf, safe_query);
            buf_puts(&buf, SGR_RESET);
            draw_row_end(&buf, inner, 3 + str_width(safe_query));

            draw_rule(&buf, "├", "┤", width);

            for (int row = 0; row < list_height; ++row) {
                size_t item_idx = scroll_offset + (size_t)row;
                buf_puts(&buf, "│");

                if (item_idx >= filtered_count) {
                    draw_pad(&buf, inner);
                    buf_puts(&buf, "│\r\n");
                    continue;
                }

                const Resolve_Candidate *cand = &candidates[filtered[item_idx].index];
                bool is_sel = (item_idx == selected);

                char raw_path[PATH_MAX];
                char disp[PATH_MAX];
                path_shorten_tilde(cand->path, raw_path, sizeof(raw_path));
                sanitize_display_string(disp, raw_path, sizeof(disp));

                unsigned char mask[PATH_MAX];
                size_t disp_len = strlen(disp);
                if (qlen > 0) {
                    matcher_highlight(query, disp, mask, disp_len);
                } else {
                    memset(mask, 0, disp_len);
                }

                // Optional columns are dropped as the terminal narrows; the
                // path itself is the only thing that must always be readable.
                bool show_score = (inner >= 46);
                bool show_kind = (inner >= 34);

                char score_txt[16];
                snprintf(score_txt, sizeof(score_txt), "%6.1f", cand->score);
                const char *kind = show_kind ? cand->project_kind : NULL;
                int kind_w = kind ? (int)strlen(kind) + 1 : 0;
                int score_w = show_score ? 7 : 0;
                int scroll_w = (filtered_count > (size_t)list_height) ? 1 : 0;

                // The cursor gutter, plus a mark column when marking is on.
                int gutter = marks ? 4 : 2;
                int path_w = inner - gutter - score_w - kind_w - scroll_w;
                if (path_w < 6) path_w = 6;

                if (is_sel) buf_puts(&buf, SGR_SELECTED);
                buf_puts(&buf, is_sel ? "❯ " : "  ");
                if (marks) {
                    if (!marks[filtered[item_idx].index]) {
                        buf_puts(&buf, "  ");
                    } else if (is_sel) {
                        buf_puts(&buf, "● ");
                    } else {
                        buf_puts(&buf, SGR_ACCENT "● " SGR_RESET);
                    }
                }

                bool ellipsis = false;
                size_t from = fit_tail(disp, path_w, &ellipsis);
                int used = gutter + draw_highlighted(&buf, disp, mask, from, ellipsis, path_w, is_sel);
                draw_pad(&buf, path_w - (used - gutter));
                used = gutter + path_w;

                if (kind) {
                    if (!is_sel) buf_puts(&buf, SGR_ACCENT);
                    buf_putf(&buf, " %s", kind);
                    if (!is_sel) buf_puts(&buf, SGR_RESET);
                    used += kind_w;
                }

                if (show_score) {
                    if (!is_sel) buf_puts(&buf, SGR_DIM);
                    buf_putf(&buf, " %s", score_txt);
                    used += score_w;
                    if (!is_sel) buf_puts(&buf, SGR_RESET);
                }

                // Pad before clearing the attribute so the selection bar
                // reaches the frame rather than stopping at the score.
                draw_pad(&buf, inner - used - scroll_w);
                if (is_sel) buf_puts(&buf, SGR_RESET);

                if (scroll_w) {
                    buf_puts(&buf, SGR_DIM);
                    buf_puts(&buf, scrollbar_cell(filtered_count, (size_t)list_height,
                                                  scroll_offset, row, list_height));
                    buf_puts(&buf, SGR_RESET);
                }
                buf_puts(&buf, "│\r\n");
            }

            draw_rule(&buf, "├", "┤", width);

            const char *footer;
            if (marks) {
                footer = (width >= 62) ? "⇥ mark · ↑↓ move · ⏎ run · ^U clear · esc cancel"
                       : (width >= 30) ? "⇥ mark · ⏎ run · esc" : "⇥ · ⏎ · esc";
            } else {
                footer = (width >= 62) ? "↑↓ move · ⏎ open · ^W word · ^U clear · esc cancel"
                       : (width >= 30) ? "↑↓ · ⏎ open · esc" : "⏎ · esc";
            }
            buf_puts(&buf, "│ " SGR_DIM);
            buf_puts(&buf, footer);
            buf_puts(&buf, SGR_RESET);
            draw_row_end(&buf, inner, 1 + str_width(footer));

            draw_rule(&buf, "└", "┘", width);
            buf_puts(&buf, "\x1b[J");

            // Park the real cursor in the query so the terminal's own caret
            // marks the edit position.
            char before[256];
            size_t n = qcursor < sizeof(before) ? qcursor : sizeof(before) - 1;
            memcpy(before, safe_query, n);
            before[n] = '\0';
            int cursor_col = 5 + str_width(before);
            buf_putf(&buf, "\x1b[2;%dH\x1b[?25h", cursor_col);

            buf_flush(&buf);
            needs_redraw = false;
        }

        Input_Event ev = read_input();
        if (ev.key == KEY_NONE) continue;

        switch (ev.key) {
        case KEY_ESC:
            goto done;

        case KEY_ENTER:
            if (filtered_count > 0 && selected < filtered_count) {
                result = (ssize_t)filtered[selected].index;
            }
            goto done;

        case KEY_TAB:
            if (marks && filtered_count > 0 && selected < filtered_count) {
                unsigned char *m = &marks[filtered[selected].index];
                *m = !*m;
                marked_count = *m ? marked_count + 1 : marked_count - 1;
                if (selected + 1 < filtered_count) selected++;
                needs_redraw = true;
            }
            break;

        case KEY_UP:
            if (selected > 0) selected--;
            else if (filtered_count) selected = filtered_count - 1;  // wrap
            needs_redraw = true;
            break;

        case KEY_DOWN:
            if (selected + 1 < filtered_count) selected++;
            else selected = 0;  // wrap
            needs_redraw = true;
            break;

        case KEY_HOME:
            selected = 0;
            needs_redraw = true;
            break;

        case KEY_END:
            if (filtered_count > 0) selected = filtered_count - 1;
            needs_redraw = true;
            break;

        case KEY_PAGE_UP:
            selected = (selected >= (size_t)list_height) ? selected - (size_t)list_height : 0;
            needs_redraw = true;
            break;

        case KEY_PAGE_DOWN:
            selected += (size_t)list_height;
            if (filtered_count > 0 && selected >= filtered_count) selected = filtered_count - 1;
            needs_redraw = true;
            break;

        case KEY_LEFT:
            if (qcursor > 0) qcursor--;
            needs_redraw = true;
            break;

        case KEY_RIGHT:
            if (qcursor < qlen) qcursor++;
            needs_redraw = true;
            break;

        case KEY_LINE_START:
            qcursor = 0;
            needs_redraw = true;
            break;

        case KEY_LINE_END:
            qcursor = qlen;
            needs_redraw = true;
            break;

        case KEY_REFRESH:
            needs_redraw = true;
            break;

        case KEY_BACKSPACE:
            if (qcursor > 0) {
                memmove(query + qcursor - 1, query + qcursor, qlen - qcursor + 1);
                qcursor--;
                qlen--;
                selected = 0;
                needs_filter = true;
            }
            break;

        case KEY_DELETE:
            if (qcursor < qlen) {
                memmove(query + qcursor, query + qcursor + 1, qlen - qcursor);
                qlen--;
                selected = 0;
                needs_filter = true;
            }
            break;

        case KEY_CLEAR:
            query[0] = '\0';
            qlen = 0;
            qcursor = 0;
            selected = 0;
            needs_filter = true;
            break;

        case KEY_KILL_WORD: {
            size_t end = qcursor;
            while (qcursor > 0 && query[qcursor - 1] == ' ') qcursor--;
            while (qcursor > 0 && query[qcursor - 1] != ' ' && query[qcursor - 1] != '/') qcursor--;
            memmove(query + qcursor, query + end, qlen - end + 1);
            qlen -= (end - qcursor);
            selected = 0;
            needs_filter = true;
            break;
        }

        case KEY_CHAR:
            if (isprint((unsigned char)ev.ch) && qlen + 1 < sizeof(query)) {
                memmove(query + qcursor + 1, query + qcursor, qlen - qcursor + 1);
                query[qcursor] = ev.ch;
                qcursor++;
                qlen++;
                selected = 0;
                needs_filter = true;
            }
            break;

        default:
            break;
        }
    }

done:
    free(filtered);
    buf_free(&buf);
    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);
    sigaction(SIGWINCH, &old_win, NULL);

    return result;
}

char *tui_select(const Resolve_Candidate *candidates, size_t count, const char *initial_query)
{
    ssize_t idx = run_selector(candidates, count, initial_query, NULL);
    return idx >= 0 ? jrun_strdup(candidates[idx].path) : NULL;
}

bool tui_select_many(const Resolve_Candidate *candidates, size_t count, const char *initial_query,
                     size_t **out_indices, size_t *out_count)
{
    *out_indices = NULL;
    *out_count = 0;
    if (!candidates || count == 0) return false;

    unsigned char *marks = (unsigned char *)calloc(count, 1);
    if (!marks) return false;

    ssize_t idx = run_selector(candidates, count, initial_query, marks);
    if (idx < 0) {
        free(marks);
        return false;
    }

    // Enter with nothing marked means just the row under the cursor.
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) n += marks[i];
    if (n == 0) {
        marks[idx] = 1;
        n = 1;
    }

    size_t *indices = (size_t *)malloc(n * sizeof(size_t));
    if (!indices) {
        free(marks);
        return false;
    }
    n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (marks[i]) indices[n++] = i;
    }
    free(marks);
    *out_indices = indices;
    *out_count = n;
    return true;
}

// ---------------------------------------------------------------------------
// Destructive-command confirmation
// ---------------------------------------------------------------------------

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

bool tui_confirm_command(const char *working_dir, char *const argv[])
{
    if (!working_dir || !argv || !argv[0]) return false;

    char cmdline[1024];
    join_argv(cmdline, sizeof(cmdline), argv);

    char short_dir[PATH_MAX];
    path_shorten_tilde(working_dir, short_dir, sizeof(short_dir));

    if (!enable_raw_mode()) {
        jrun_log_error("refusing destructive command '%s' without a terminal (pass -y to skip)",
                       argv[0]);
        return false;
    }

    struct sigaction sa, old_sa, sa_win, old_win;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    g_interrupted = 0;
    g_resized = 0;
    sigaction(SIGINT, &sa, &old_sa);
    memset(&sa_win, 0, sizeof(sa_win));
    sa_win.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_win, &old_win);

    Buf buf = {0};
    bool confirmed = false;
    bool done = false;
    // Defaults to "No": a mis-typed Enter must never run the command.
    int choice = 0;
    bool needs_redraw = true;

    while (!g_interrupted && !done) {
        if (g_resized) { g_resized = 0; needs_redraw = true; }

        Term_Size ts = get_term_size();
        int width = ts.cols > 100 ? 100 : ts.cols;
        int inner = width - 2;

        if (needs_redraw) {
            buf.len = 0;
            buf_puts(&buf, "\x1b[H\x1b[?25l");
            draw_title(&buf, "Confirm destructive command", NULL, width);

            char safe_dir[PATH_MAX];
            char safe_cmd[1024];
            sanitize_display_string(safe_dir, short_dir, sizeof(safe_dir));
            sanitize_display_string(safe_cmd, cmdline, sizeof(safe_cmd));

            const char *dir_label = " Directory  ";
            buf_puts(&buf, "│");
            buf_puts(&buf, SGR_DIM);
            buf_puts(&buf, dir_label);
            buf_puts(&buf, SGR_RESET);
            int avail = inner - (int)strlen(dir_label) - 1;
            bool ell = false;
            size_t from = fit_tail(safe_dir, avail, &ell);
            int used = (int)strlen(dir_label) + draw_highlighted(&buf, safe_dir, NULL, from, ell, avail, false);
            draw_row_end(&buf, inner, used);

            const char *cmd_label = " Command    ";
            buf_puts(&buf, "│");
            buf_puts(&buf, SGR_DIM);
            buf_puts(&buf, cmd_label);
            buf_puts(&buf, SGR_DANGER);
            avail = inner - (int)strlen(cmd_label) - 1;
            ell = false;
            from = fit_tail(safe_cmd, avail, &ell);
            used = (int)strlen(cmd_label) + draw_highlighted(&buf, safe_cmd, NULL, from, ell, avail, false);
            buf_puts(&buf, SGR_RESET);
            draw_row_end(&buf, inner, used);

            draw_rule(&buf, "├", "┤", width);

            buf_puts(&buf, "│  ");
            if (choice == 0) {
                buf_puts(&buf, SGR_SELECTED " No " SGR_RESET "   Yes ");
            } else {
                buf_puts(&buf, " No " SGR_DANGER SGR_SELECTED "   Yes " SGR_RESET);
            }
            // "  " + " No " + "   Yes " -- both branches draw the same 13
            // visible columns and differ only in attributes.
            draw_row_end(&buf, inner, 2 + 4 + 7);

            draw_rule(&buf, "├", "┤", width);
            const char *footer = "←→ choose · y confirm · n/esc cancel";
            buf_puts(&buf, "│ " SGR_DIM);
            buf_puts(&buf, footer);
            buf_puts(&buf, SGR_RESET);
            draw_row_end(&buf, inner, 1 + str_width(footer));
            draw_rule(&buf, "└", "┘", width);
            buf_puts(&buf, "\x1b[J");
            buf_flush(&buf);
            needs_redraw = false;
        }

        Input_Event ev = read_input();
        if (ev.key == KEY_NONE) continue;
        needs_redraw = true;

        switch (ev.key) {
        case KEY_ESC:
            done = true;
            break;
        case KEY_LEFT: case KEY_UP:
            choice = 0;
            break;
        case KEY_RIGHT: case KEY_DOWN: case KEY_TAB:
            choice = 1;
            break;
        case KEY_ENTER:
            confirmed = (choice == 1);
            done = true;
            break;
        case KEY_CHAR:
            if (ev.ch == 'y' || ev.ch == 'Y') { confirmed = true; done = true; }
            else if (ev.ch == 'n' || ev.ch == 'N' || ev.ch == 'q') { confirmed = false; done = true; }
            else if (ev.ch == 'h') choice = 0;
            else if (ev.ch == 'l') choice = 1;
            break;
        default:
            break;
        }
    }

    buf_free(&buf);
    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);
    sigaction(SIGWINCH, &old_win, NULL);
    return confirmed;
}

// ---------------------------------------------------------------------------
// Configuration editor
// ---------------------------------------------------------------------------

enum Cfg_Item_Type {
    CFG_HEADER = 0,
    CFG_ADD_ROOT,
    CFG_ROOT,
    CFG_ADD_IGNORE,
    CFG_IGNORE,
    CFG_BOOL,
    CFG_INT,
    CFG_DOUBLE,
    CFG_ADD_CMD,
    CFG_CMD,
};

enum Cfg_Field {
    CFG_FOLLOW_SYMLINKS = 0,
    CFG_FUZZY,
    CFG_INTERACTIVE,
    CFG_PREFER_PROJECTS,
    CFG_CONFIRM,
    CFG_MAX_DEPTH,
    CFG_CACHE_TTL,
    CFG_FRECENCY,
};

typedef struct {
    int type;
    const char *label;
    const char *help;
    size_t index;
    int field;
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
    case CFG_FUZZY:           return &cfg->fuzzy;
    case CFG_INTERACTIVE:     return &cfg->interactive;
    case CFG_PREFER_PROJECTS: return &cfg->prefer_projects;
    case CFG_CONFIRM:         return &cfg->confirm_destructive;
    default: return NULL;
    }
}

static int *cfg_int_ptr(Jrun_Config *cfg, int field)
{
    switch (field) {
    case CFG_MAX_DEPTH: return &cfg->max_depth;
    case CFG_CACHE_TTL: return &cfg->cache_ttl;
    default: return NULL;
    }
}

static void cfg_int_range(int field, int *lo, int *hi, int *step)
{
    switch (field) {
    case CFG_MAX_DEPTH: *lo = 1; *hi = 32;    *step = 1;  break;
    case CFG_CACHE_TTL: *lo = 0; *hi = 86400; *step = 60; break;
    default:            *lo = 0; *hi = 0;     *step = 1;  break;
    }
}

#define CFG_MAX_ITEMS 512

static size_t cfg_build_items(Jrun_Config *cfg, Cfg_Item *items, size_t cap)
{
    size_t n = 0;
    #define PUSH(...) do { if (n < cap) items[n++] = (Cfg_Item){ __VA_ARGS__ }; } while (0)

    PUSH(.type = CFG_HEADER, .label = "SEARCH ROOTS");
    PUSH(.type = CFG_ADD_ROOT, .label = "+ add a root",
         .help = "directories jrun indexes");
    for (size_t i = 0; i < cfg->roots_count && n < cap; ++i) {
        PUSH(.type = CFG_ROOT, .label = cfg->roots[i], .index = i);
    }

    PUSH(.type = CFG_HEADER, .label = "IGNORED DIRECTORY NAMES");
    PUSH(.type = CFG_ADD_IGNORE, .label = "+ add an ignore",
         .help = "never descended into during a scan");
    for (size_t i = 0; i < cfg->ignore_count && n < cap; ++i) {
        PUSH(.type = CFG_IGNORE, .label = cfg->ignore[i], .index = i);
    }

    PUSH(.type = CFG_HEADER, .label = "BEHAVIOR");
    PUSH(.type = CFG_BOOL, .label = "fuzzy", .field = CFG_FUZZY,
         .help = "match loose subsequences, not just prefixes");
    PUSH(.type = CFG_BOOL, .label = "interactive", .field = CFG_INTERACTIVE,
         .help = "show the selector when several directories match");
    PUSH(.type = CFG_BOOL, .label = "prefer_projects", .field = CFG_PREFER_PROJECTS,
         .help = "rank .git / Cargo.toml / package.json directories higher");
    PUSH(.type = CFG_BOOL, .label = "follow_symlinks", .field = CFG_FOLLOW_SYMLINKS,
         .help = "descend through symlinked directories");
    PUSH(.type = CFG_INT, .label = "max_depth", .field = CFG_MAX_DEPTH,
         .help = "how deep below each root to index");
    PUSH(.type = CFG_INT, .label = "cache_ttl", .field = CFG_CACHE_TTL,
         .help = "seconds the directory index stays valid; 0 rescans each time");
    PUSH(.type = CFG_DOUBLE, .label = "frecency_threshold", .field = CFG_FRECENCY,
         .help = "how far ahead the top match must be to skip the selector");

    PUSH(.type = CFG_HEADER, .label = "SAFETY");
    PUSH(.type = CFG_BOOL, .label = "confirm", .field = CFG_CONFIRM,
         .help = "prompt before running the commands listed below");
    PUSH(.type = CFG_ADD_CMD, .label = "+ add a command",
         .help = "commands that require confirmation");
    for (size_t i = 0; i < cfg->confirm_commands_count && n < cap; ++i) {
        PUSH(.type = CFG_CMD, .label = cfg->confirm_commands[i], .index = i);
    }
    #undef PUSH
    return n;
}

static bool cfg_item_selectable(int type)
{
    return type != CFG_HEADER;
}

static bool cfg_is_list_entry(int type)
{
    return type == CFG_ROOT || type == CFG_IGNORE || type == CFG_CMD;
}

static void cfg_skip(Cfg_Item *items, size_t count, size_t *sel, int dir)
{
    if (count == 0) return;
    for (size_t step = 0; step < count; ++step) {
        if (dir > 0) *sel = (*sel + 1) % count;
        else *sel = (*sel == 0) ? count - 1 : *sel - 1;
        if (cfg_item_selectable(items[*sel].type)) return;
    }
}

static bool cfg_commit_input(Jrun_Config *cfg, int kind, int field, const char *text,
                             char *status, size_t status_sz)
{
    if (!text || text[0] == '\0') {
        snprintf(status, status_sz, "cancelled");
        return false;
    }

    if (kind == CFG_ADD_ROOT) {
        char shortp[PATH_MAX];
        char norm[PATH_MAX];
        const char *store = text;
        if (path_normalize(text, norm, sizeof(norm)) &&
            path_shorten_tilde(norm, shortp, sizeof(shortp))) {
            store = shortp;
        }
        if (config_add_root(cfg, store)) {
            // %.120s: the status line is 256 bytes and a root can be PATH_MAX.
            snprintf(status, status_sz, "added root %.120s", store);
            return true;
        }
        snprintf(status, status_sz, "could not add that root");
        return false;
    }

    if (kind == CFG_ADD_IGNORE) {
        if (config_add_ignore(cfg, text)) {
            snprintf(status, status_sz, "ignoring %.120s", text);
            return true;
        }
        snprintf(status, status_sz, "could not add that ignore");
        return false;
    }

    if (kind == CFG_ADD_CMD) {
        if (config_add_confirm_command(cfg, text)) {
            snprintf(status, status_sz, "%.120s now needs confirmation", text);
            return true;
        }
        snprintf(status, status_sz, "could not add that command");
        return false;
    }

    if (kind == CFG_INT) {
        int *p = cfg_int_ptr(cfg, field);
        if (!p) return false;
        int lo, hi, step;
        cfg_int_range(field, &lo, &hi, &step);
        int v = atoi(text);
        if (v < lo || v > hi) {
            snprintf(status, status_sz, "value must be between %d and %d", lo, hi);
            return false;
        }
        *p = v;
        snprintf(status, status_sz, "set to %d", v);
        return true;
    }

    if (kind == CFG_DOUBLE) {
        double v = atof(text);
        if (v <= 0.0) {
            snprintf(status, status_sz, "frecency_threshold must be greater than 0");
            return false;
        }
        cfg->frecency_threshold = v;
        snprintf(status, status_sz, "frecency_threshold = %.2f", v);
        return true;
    }

    return false;
}

// Formats the value column for one row.
static void cfg_value_text(Jrun_Config *cfg, const Cfg_Item *it, char *out, size_t out_size)
{
    out[0] = '\0';
    switch (it->type) {
    case CFG_BOOL: {
        bool *p = cfg_bool_ptr(cfg, it->field);
        snprintf(out, out_size, "%s", (p && *p) ? "on" : "off");
        break;
    }
    case CFG_INT: {
        int *p = cfg_int_ptr(cfg, it->field);
        if (p) snprintf(out, out_size, "%d", *p);
        break;
    }
    case CFG_DOUBLE:
        snprintf(out, out_size, "%.2f", cfg->frecency_threshold);
        break;
    case CFG_ROOT: {
        char expanded[PATH_MAX];
        if (path_expand_tilde(it->label, expanded, sizeof(expanded)) && !path_is_dir(expanded)) {
            snprintf(out, out_size, "missing");
        }
        break;
    }
    default:
        break;
    }
}

bool tui_edit_config(Jrun_Config *config, const char *filepath)
{
    if (!config || !filepath) return false;
    if (!enable_raw_mode()) {
        jrun_log_error("no terminal available for the configuration editor");
        return false;
    }

    struct sigaction sa, old_sa, sa_win, old_win;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    g_interrupted = 0;
    g_resized = 0;
    sigaction(SIGINT, &sa, &old_sa);
    memset(&sa_win, 0, sizeof(sa_win));
    sa_win.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_win, &old_win);

    Cfg_Item *items = (Cfg_Item *)malloc(CFG_MAX_ITEMS * sizeof(Cfg_Item));
    Buf buf = {0};
    if (!items) {
        disable_raw_mode();
        sigaction(SIGINT, &old_sa, NULL);
        sigaction(SIGWINCH, &old_win, NULL);
        return false;
    }

    size_t item_count = cfg_build_items(config, items, CFG_MAX_ITEMS);
    size_t selected = 0;
    size_t scroll_offset = 0;
    int mode = CFG_MODE_NAV;
    int input_kind = CFG_ADD_ROOT;
    int input_field = 0;
    char input[PATH_MAX];
    size_t ilen = 0;
    input[0] = '\0';
    char status[256];
    snprintf(status, sizeof(status), "%s", filepath);
    bool dirty = false;
    bool running = true;
    bool needs_redraw = true;

    cfg_skip(items, item_count, &selected, 1);

    while (!g_interrupted && running) {
        if (g_resized) { g_resized = 0; needs_redraw = true; }

        item_count = cfg_build_items(config, items, CFG_MAX_ITEMS);
        if (item_count == 0) break;
        if (selected >= item_count) selected = item_count - 1;
        if (!cfg_item_selectable(items[selected].type)) {
            cfg_skip(items, item_count, &selected, 1);
        }

        Term_Size ts = get_term_size();
        int width = ts.cols > 110 ? 110 : ts.cols;
        int inner = width - 2;
        int list_height = ts.rows - 6;
        if (list_height < 5) list_height = 5;

        if (selected < scroll_offset) scroll_offset = selected;
        else if (selected >= scroll_offset + (size_t)list_height) {
            scroll_offset = selected - (size_t)list_height + 1;
        }

        if (needs_redraw) {
            buf.len = 0;
            buf_puts(&buf, "\x1b[H\x1b[?25l");
            draw_title(&buf, "jrun config", dirty ? "unsaved" : "saved", width);

            int scroll_w = (item_count > (size_t)list_height) ? 1 : 0;

            for (int row = 0; row < list_height; ++row) {
                size_t idx = scroll_offset + (size_t)row;
                buf_puts(&buf, "│");

                if (idx >= item_count) {
                    draw_pad(&buf, inner);
                    buf_puts(&buf, "│\r\n");
                    continue;
                }

                Cfg_Item *it = &items[idx];
                bool is_sel = (idx == selected && mode == CFG_MODE_NAV);
                int used = 0;

                if (it->type == CFG_HEADER) {
                    // Blank spacer above every section but the first.
                    buf_puts(&buf, SGR_DIM SGR_BOLD " ");
                    char safe[128];
                    sanitize_display_string(safe, it->label, sizeof(safe));
                    buf_puts(&buf, safe);
                    buf_puts(&buf, SGR_RESET);
                    used = 1 + str_width(safe);
                    draw_pad(&buf, inner - used - scroll_w);
                } else {
                    char safe_label[PATH_MAX];
                    char value[64];
                    sanitize_display_string(safe_label, it->label, sizeof(safe_label));
                    cfg_value_text(config, it, value, sizeof(value));

                    int value_w = (int)strlen(value);
                    int label_w = inner - 4 - (value_w ? value_w + 2 : 0) - scroll_w;
                    if (label_w < 8) label_w = 8;

                    if (is_sel) buf_puts(&buf, SGR_SELECTED);
                    buf_puts(&buf, is_sel ? " ❯ " : "   ");
                    used = 3;

                    if (cfg_is_list_entry(it->type) && !is_sel) buf_puts(&buf, SGR_DIM);
                    bool ell = false;
                    size_t from = fit_tail(safe_label, label_w, &ell);
                    used += draw_highlighted(&buf, safe_label, NULL, from, ell, label_w, is_sel);
                    if (cfg_is_list_entry(it->type) && !is_sel) buf_puts(&buf, SGR_RESET);
                    draw_pad(&buf, label_w - (used - 3));
                    used = 3 + label_w;

                    if (value_w) {
                        bool off = (strcmp(value, "off") == 0 || strcmp(value, "missing") == 0);
                        if (!is_sel) buf_puts(&buf, off ? SGR_DIM : SGR_ACCENT);
                        buf_putf(&buf, "  %s", value);
                        if (!is_sel) buf_puts(&buf, SGR_RESET);
                        used += value_w + 2;
                    }
                    // Pad first, clear the attribute after, so the selection
                    // bar runs the full width of the row.
                    draw_pad(&buf, inner - used - scroll_w);
                    if (is_sel) buf_puts(&buf, SGR_RESET);
                }

                if (scroll_w) {
                    buf_puts(&buf, SGR_DIM);
                    buf_puts(&buf, scrollbar_cell(item_count, (size_t)list_height,
                                                  scroll_offset, row, list_height));
                    buf_puts(&buf, SGR_RESET);
                }
                buf_puts(&buf, "│\r\n");
            }

            draw_rule(&buf, "├", "┤", width);

            // Status line: the prompt when editing, otherwise the selected
            // item's one-line explanation, falling back to the last action.
            char prompt[PATH_MAX + 64];
            const char *prefix = "";
            if (mode == CFG_MODE_INPUT) {
                prefix = (input_kind == CFG_ADD_ROOT)   ? "root path: "
                       : (input_kind == CFG_ADD_IGNORE) ? "ignore name: "
                       : (input_kind == CFG_ADD_CMD)    ? "command: "
                       : "value: ";
                snprintf(prompt, sizeof(prompt), "%s%s", prefix, input);
            } else if (mode == CFG_MODE_QUIT) {
                snprintf(prompt, sizeof(prompt), "Unsaved changes — save before leaving?  y / n / esc");
            } else if (selected < item_count && items[selected].help) {
                snprintf(prompt, sizeof(prompt), "%s", items[selected].help);
            } else {
                snprintf(prompt, sizeof(prompt), "%s", status);
            }

            char safe_p[PATH_MAX + 64];
            sanitize_display_string(safe_p, prompt, sizeof(safe_p));
            buf_puts(&buf, "│ ");
            if (mode == CFG_MODE_QUIT) buf_puts(&buf, SGR_WARN);
            else if (mode == CFG_MODE_INPUT) buf_puts(&buf, SGR_BOLD);
            else buf_puts(&buf, SGR_DIM);

            bool ell = false;
            size_t from = fit_tail(safe_p, inner - 2, &ell);
            int used = 1 + draw_highlighted(&buf, safe_p, NULL, from, ell, inner - 2, false);
            buf_puts(&buf, SGR_RESET);
            draw_row_end(&buf, inner, used);

            draw_rule(&buf, "├", "┤", width);
            const char *footer = (mode == CFG_MODE_INPUT)
                ? "⏎ accept · esc cancel"
                : "↑↓ move · ⏎ edit · a add · d delete · ←→ adjust · ^S save · q quit";
            buf_puts(&buf, "│ " SGR_DIM);
            buf_puts(&buf, footer);
            buf_puts(&buf, SGR_RESET);
            draw_row_end(&buf, inner, 1 + str_width(footer));
            draw_rule(&buf, "└", "┘", width);
            buf_puts(&buf, "\x1b[J");

            if (mode == CFG_MODE_INPUT) {
                int col = 3 + str_width(safe_p);
                if (col > width - 1) col = width - 1;
                buf_putf(&buf, "\x1b[%d;%dH\x1b[?25h", ts.rows - 2, col);
            }

            buf_flush(&buf);
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
                if (cfg_commit_input(config, input_kind, input_field, input, status, sizeof(status))) {
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
            } else if (ev.key == KEY_CHAR && (ev.ch == 'y' || ev.ch == 'Y' || ev.ch == 's')) {
                if (config_save(config, filepath)) {
                    dirty = false;
                    running = false;
                } else {
                    snprintf(status, sizeof(status), "could not write %s", filepath);
                    mode = CFG_MODE_NAV;
                }
            } else if (ev.key == KEY_CHAR && (ev.ch == 'n' || ev.ch == 'N')) {
                // Discard: reload from disk so the caller keeps a config that
                // matches what is actually stored.
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
            continue;
        }
        if (ev.key == KEY_SAVE || (ev.key == KEY_CHAR && ev.ch == 's')) {
            if (config_save(config, filepath)) {
                dirty = false;
                snprintf(status, sizeof(status), "saved to %s", filepath);
            } else {
                snprintf(status, sizeof(status), "could not write %s", filepath);
            }
            continue;
        }
        if (ev.key == KEY_UP)   { cfg_skip(items, item_count, &selected, -1); continue; }
        if (ev.key == KEY_DOWN) { cfg_skip(items, item_count, &selected, 1);  continue; }
        if (ev.key == KEY_PAGE_UP) {
            for (int i = 0; i < list_height; ++i) cfg_skip(items, item_count, &selected, -1);
            continue;
        }
        if (ev.key == KEY_PAGE_DOWN) {
            for (int i = 0; i < list_height; ++i) cfg_skip(items, item_count, &selected, 1);
            continue;
        }
        if (ev.key == KEY_HOME) {
            selected = 0;
            cfg_skip(items, item_count, &selected, 1);
            continue;
        }
        if (ev.key == KEY_END) {
            selected = item_count - 1;
            if (!cfg_item_selectable(items[selected].type)) {
                cfg_skip(items, item_count, &selected, -1);
            }
            continue;
        }
        if (!cur) continue;

        bool is_add = (cur->type == CFG_ADD_ROOT || cur->type == CFG_ADD_IGNORE ||
                       cur->type == CFG_ADD_CMD);

        if (ev.key == KEY_ENTER || (ev.key == KEY_CHAR && ev.ch == ' ')) {
            if (cur->type == CFG_BOOL) {
                bool *p = cfg_bool_ptr(config, cur->field);
                if (p) {
                    *p = !*p;
                    dirty = true;
                    snprintf(status, sizeof(status), "%s = %s", cur->label, *p ? "true" : "false");
                }
            } else if (is_add || cur->type == CFG_INT || cur->type == CFG_DOUBLE) {
                mode = CFG_MODE_INPUT;
                input_kind = cur->type;
                input_field = cur->field;
                ilen = 0;
                input[0] = '\0';
                if (cur->type == CFG_INT) {
                    int *p = cfg_int_ptr(config, cur->field);
                    if (p) ilen = (size_t)snprintf(input, sizeof(input), "%d", *p);
                } else if (cur->type == CFG_DOUBLE) {
                    ilen = (size_t)snprintf(input, sizeof(input), "%.2f", config->frecency_threshold);
                }
            }
            continue;
        }

        if (ev.key == KEY_CHAR && ev.ch == 'a') {
            mode = CFG_MODE_INPUT;
            input_kind = (cur->type == CFG_CMD || cur->type == CFG_ADD_CMD)       ? CFG_ADD_CMD
                       : (cur->type == CFG_IGNORE || cur->type == CFG_ADD_IGNORE) ? CFG_ADD_IGNORE
                       : CFG_ADD_ROOT;
            ilen = 0;
            input[0] = '\0';
            continue;
        }

        if ((ev.key == KEY_BACKSPACE || ev.key == KEY_DELETE ||
             (ev.key == KEY_CHAR && (ev.ch == 'd' || ev.ch == 'D' || ev.ch == 'x'))) &&
            cfg_is_list_entry(cur->type)) {
            // Copy the label before removing it: `cur->label` points straight
            // into the array being mutated.
            // Sized to what the status line can show, so the compiler can
            // see the copy below cannot truncate.
            char removed[200];
            snprintf(removed, sizeof(removed), "%s", cur->label);
            if (cur->type == CFG_ROOT)        config_remove_root_at(config, cur->index);
            else if (cur->type == CFG_IGNORE) config_remove_ignore_at(config, cur->index);
            else                              config_remove_confirm_command_at(config, cur->index);
            snprintf(status, sizeof(status), "removed %s", removed);
            dirty = true;
            if (selected > 0) selected--;
            continue;
        }

        bool dec = (ev.key == KEY_LEFT) || (ev.key == KEY_CHAR && ev.ch == '-');
        bool inc = (ev.key == KEY_RIGHT) || (ev.key == KEY_CHAR && ev.ch == '+');
        if (!dec && !inc) continue;

        if (cur->type == CFG_BOOL) {
            bool *p = cfg_bool_ptr(config, cur->field);
            if (p) {
                *p = inc;
                dirty = true;
                snprintf(status, sizeof(status), "%s = %s", cur->label, *p ? "true" : "false");
            }
        } else if (cur->type == CFG_INT) {
            int *p = cfg_int_ptr(config, cur->field);
            int lo, hi, step;
            cfg_int_range(cur->field, &lo, &hi, &step);
            if (p) {
                int v = *p + (inc ? step : -step);
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                if (v != *p) {
                    *p = v;
                    dirty = true;
                    snprintf(status, sizeof(status), "%s = %d", cur->label, v);
                }
            }
        } else if (cur->type == CFG_DOUBLE) {
            double v = config->frecency_threshold + (inc ? 0.25 : -0.25);
            if (v < 0.25) v = 0.25;
            if (v != config->frecency_threshold) {
                config->frecency_threshold = v;
                dirty = true;
                snprintf(status, sizeof(status), "frecency_threshold = %.2f", v);
            }
        }
    }

    free(items);
    buf_free(&buf);
    disable_raw_mode();
    sigaction(SIGINT, &old_sa, NULL);
    sigaction(SIGWINCH, &old_win, NULL);
    return true;
}
