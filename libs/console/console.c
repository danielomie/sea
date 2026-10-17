/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

/*
 * libs/console/console.c — Terminal manipulation library for Sea.
 *
 * Build:  cd libs/console && make
 * Usage:  local console = require("./libs/console/console")
 *
 * Exports: goto, up, down, left, right, save, restore, home,
 *          clear, clear_line, clear_eol, hide_cursor, show_cursor,
 *          write, writeln, reset,
 *          readline, readkey, readpass,
 *          progress,
 *          shimmer
 */

#include "sea.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ---- Internal helpers ----------------------------------------------------- */

static void out(const char *s)         { fputs(s, stdout); }
static void outf(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
}
static void flush(void) { fflush(stdout); }

static int64_t int_arg(SeaValue v) {
    return sea_is_int(v) ? sea_as_int(v) : (int64_t)sea_as_number(v);
}

static int term_width(void) __attribute__((unused));
static int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 80;
}

/* fn_size: returns {width, height} of the terminal */
static SeaValue fn_size(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    struct winsize ws;
    int w = 80, h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        w = (int)ws.ws_col;
        h = (int)ws.ws_row;
    }
    SeaTable *t = sea_table_new(TABLE_MAP);
    sea_table_set_str(t, "width",  sea_int_val(w));
    sea_table_set_str(t, "height", sea_int_val(h));
    return sea_obj_val(&t->obj);
}

/* ---- Cursor --------------------------------------------------------------- */

static SeaValue fn_goto(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int r = argc >= 1 ? (int)int_arg(args[0]) : 1;
    int c = argc >= 2 ? (int)int_arg(args[1]) : 1;
    outf("\x1b[%d;%dH", r, c); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_up(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int n = argc >= 1 ? (int)int_arg(args[0]) : 1;
    outf("\x1b[%dA", n); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_down(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int n = argc >= 1 ? (int)int_arg(args[0]) : 1;
    outf("\x1b[%dB", n); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_right(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int n = argc >= 1 ? (int)int_arg(args[0]) : 1;
    outf("\x1b[%dC", n); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_left(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int n = argc >= 1 ? (int)int_arg(args[0]) : 1;
    outf("\x1b[%dD", n); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_save(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[s"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_restore(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[u"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_home(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[H"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_clear(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[2J\x1b[H"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_clear_line(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[2K\r"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_clear_eol(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[K"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_hide_cursor(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[?25l"); flush();
    return SEA_NIL_VAL;
}

static SeaValue fn_show_cursor(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[?25h"); flush();
    return SEA_NIL_VAL;
}

/* ---- Color resolution ----------------------------------------------------- */

typedef struct { int r, g, b; int ansi; } Color;
/* ansi >= 0 → use \x1b[{ansi}m ; ansi == -1 → use RGB true-color */

static int name_to_fg(const char *name) {
    struct { const char *name; int code; } map[] = {
        {"black",30},   {"red",31},     {"green",32},   {"yellow",33},
        {"blue",34},    {"magenta",35}, {"cyan",36},    {"white",37},
        {"bright_black",90},   {"bright_red",91},   {"bright_green",92},
        {"bright_yellow",93},  {"bright_blue",94},  {"bright_magenta",95},
        {"bright_cyan",96},    {"bright_white",97}, {"default",39},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        if (strcmp(map[i].name, name) == 0) return map[i].code;
    return -1;
}

static Color resolve_color(SeaValue v, int bg) {
    Color c = {255, 255, 255, bg ? 49 : 39};
    if (sea_is_nil(v)) return c;

    if (sea_is_string(v)) {
        const char *s = SEA_AS_STRING(v)->data;
        int code = name_to_fg(s);
        if (code >= 0) { c.ansi = bg ? code + 10 : code; return c; }
        if (s[0] == '#' && strlen(s) == 7) {
            unsigned int rgb = 0;
            sscanf(s + 1, "%06x", &rgb);
            c.r = (int)((rgb >> 16) & 0xFF);
            c.g = (int)((rgb >>  8) & 0xFF);
            c.b = (int)( rgb        & 0xFF);
            c.ansi = -1; return c;
        }
    }
    if (sea_is_table(v)) {
        SeaTable *t = SEA_AS_TABLE(v);
        SeaValue rv, gv, bv;
        c.r = sea_table_get_str(t, "r", &rv) ? (int)int_arg(rv) : 0;
        c.g = sea_table_get_str(t, "g", &gv) ? (int)int_arg(gv) : 0;
        c.b = sea_table_get_str(t, "b", &bv) ? (int)int_arg(bv) : 0;
        c.ansi = -1; return c;
    }
    return c;
}

static void apply_color(Color c, int bg) {
    if (c.ansi >= 0) {
        outf("\x1b[%dm", c.ansi);
    } else {
        outf(bg ? "\x1b[48;2;%d;%d;%dm" : "\x1b[38;2;%d;%d;%dm",
             c.r, c.g, c.b);
    }
}

/* ---- Style application ---------------------------------------------------- */

/* Parse style table: {color, bg, bold, dim, italic, underline, reverse} */
static void apply_style(SeaTable *opts) {
    if (!opts) return;

    SeaValue v;
    if (sea_table_get_str(opts, "bold",      &v) && sea_is_truthy(v)) out("\x1b[1m");
    if (sea_table_get_str(opts, "dim",       &v) && sea_is_truthy(v)) out("\x1b[2m");
    if (sea_table_get_str(opts, "italic",    &v) && sea_is_truthy(v)) out("\x1b[3m");
    if (sea_table_get_str(opts, "underline", &v) && sea_is_truthy(v)) out("\x1b[4m");
    if (sea_table_get_str(opts, "blink",     &v) && sea_is_truthy(v)) out("\x1b[5m");
    if (sea_table_get_str(opts, "reverse",   &v) && sea_is_truthy(v)) out("\x1b[7m");

    if (sea_table_get_str(opts, "color", &v) && !sea_is_nil(v))
        apply_color(resolve_color(v, 0), 0);
    if (sea_table_get_str(opts, "bg", &v) && !sea_is_nil(v))
        apply_color(resolve_color(v, 1), 1);
}

static SeaValue fn_reset(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    out("\x1b[0m"); flush();
    return SEA_NIL_VAL;
}

/* write(text [, opts]) */
static SeaValue fn_write(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    SeaTable *opts = (argc >= 2 && sea_is_table(args[1])) ? SEA_AS_TABLE(args[1]) : NULL;
    if (opts) apply_style(opts);
    out(SEA_AS_STRING(args[0])->data);
    if (opts) out("\x1b[0m");
    flush();
    return SEA_NIL_VAL;
}

/* writeln(text [, opts]) */
static SeaValue fn_writeln(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) { out("\n"); flush(); return SEA_NIL_VAL; }
    SeaTable *opts = (argc >= 2 && sea_is_table(args[1])) ? SEA_AS_TABLE(args[1]) : NULL;
    if (opts) apply_style(opts);
    out(SEA_AS_STRING(args[0])->data);
    if (opts) out("\x1b[0m");
    out("\n"); flush();
    return SEA_NIL_VAL;
}

/* ---- Input ---------------------------------------------------------------- */

static struct termios g_orig_termios;

static void raw_on(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios t = g_orig_termios;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void raw_off(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static void noecho_on(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios t = g_orig_termios;
    t.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

/* readline(prompt?) → string */
static SeaValue fn_readline(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc >= 1 && sea_is_string(args[0])) {
        out(SEA_AS_STRING(args[0])->data); flush();
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return SEA_NIL_VAL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') len--;
    return sea_obj_val(&sea_string_new(buf, len)->obj);
}

/* readkey() → string (single keypress, no enter needed) */
static SeaValue fn_readkey(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    raw_on();
    char c = (char)getchar();
    raw_off();
    return sea_obj_val(&sea_string_new(&c, 1)->obj);
}

/* readpass(prompt?) → string (input hidden) */
static SeaValue fn_readpass(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc >= 1 && sea_is_string(args[0])) {
        out(SEA_AS_STRING(args[0])->data); flush();
    }
    noecho_on();
    char buf[4096];
    char *result = fgets(buf, sizeof(buf), stdin);
    raw_off();
    out("\n"); flush();
    if (!result) return SEA_NIL_VAL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') len--;
    return sea_obj_val(&sea_string_new(buf, len)->obj);
}

/* ---- Progress bar --------------------------------------------------------- */

/* progress(current, total [, opts])
 * opts: { width=30, fill="█", empty="░", color="green",
 *         show_percent=true, show_count=false } */
static SeaValue fn_progress(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    int64_t cur   = int_arg(args[0]);
    int64_t total = int_arg(args[1]);
    if (total <= 0) total = 1;
    if (cur < 0) cur = 0;
    if (cur > total) cur = total;

    SeaTable *opts = (argc >= 3 && sea_is_table(args[2])) ? SEA_AS_TABLE(args[2]) : NULL;

    int width = 30;
    const char *fill  = "\xe2\x96\x88";  /* █ */
    const char *empty = "\xe2\x96\x91";  /* ░ */
    int show_percent = 1, show_count = 0;

    SeaValue v;
    if (opts) {
        if (sea_table_get_str(opts, "width", &v))         width        = (int)int_arg(v);
        if (sea_table_get_str(opts, "fill",  &v) && sea_is_string(v)) fill  = SEA_AS_STRING(v)->data;
        if (sea_table_get_str(opts, "empty", &v) && sea_is_string(v)) empty = SEA_AS_STRING(v)->data;
        if (sea_table_get_str(opts, "show_percent", &v))  show_percent = sea_is_truthy(v);
        if (sea_table_get_str(opts, "show_count",   &v))  show_count   = sea_is_truthy(v);
    }

    int filled = (int)((double)cur / (double)total * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;

    /* Apply color */
    if (opts && sea_table_get_str(opts, "color", &v) && !sea_is_nil(v))
        apply_color(resolve_color(v, 0), 0);
    else
        out("\x1b[32m");  /* default: green */

    out("[");
    for (int i = 0; i < filled; i++)  out(fill);
    out("\x1b[0m");
    out("\x1b[2m");  /* dim for empty portion */
    for (int i = filled; i < width; i++) out(empty);
    out("\x1b[0m");
    out("]");

    if (show_percent) {
        int pct = (int)((double)cur / (double)total * 100.0);
        outf(" %3d%%", pct);
    }
    if (show_count) outf(" %lld/%lld", (long long)cur, (long long)total);

    flush();
    return SEA_NIL_VAL;
}

/* ---- Shimmer / glow effect ------------------------------------------------ */

/*
 * Renders text with an animated brightness wave that sweeps left to right,
 * simulating the shimmering "live" text effect seen in Claude CLI.
 *
 * Uses true-color (24-bit) ANSI codes for a smooth gradient.
 * The wave profile is a Gaussian bell curve.
 *
 * shimmer(text [, opts])
 * opts: { color={r,g,b} or "white", duration=1.5, fps=30, wave_width=0.35 }
 */

typedef struct { int r, g, b; } RGB;

static RGB lerp_rgb(RGB a, RGB b, double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (RGB){
        (int)(a.r + (b.r - a.r) * t),
        (int)(a.g + (b.g - a.g) * t),
        (int)(a.b + (b.b - a.b) * t),
    };
}

/* Gaussian profile: returns 0..1, peak at center */
static double gauss(double x, double sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma));
}

/* Characters (display cells) in a UTF-8 string; an invalid byte is one. */
static int utf8_display_len(const char *s, size_t bytes) {
    return (int)sea_utf8_len(s, bytes);
}

/* Byte offset of the n-th cell (0-indexed). */
static size_t utf8_cell_offset(const char *s, size_t bytes, int cell) {
    size_t i = 0;
    for (int n = 0; n < cell && i < bytes; n++) i = sea_utf8_next(s, bytes, i);
    return i;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static SeaValue fn_shimmer(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;

    const char *text    = SEA_AS_STRING(args[0])->data;
    size_t text_bytes   = SEA_AS_STRING(args[0])->len;

    /* Defaults */
    RGB   peak_color  = {255, 255, 255};  /* bright white */
    RGB   base_color  = { 70,  70,  70};  /* dark gray */
    RGB   final_color = {220, 220, 220};  /* resting off-white */
    double duration   = 1.5;
    int    fps        = 30;
    double wave_w     = 0.30;             /* wave sigma as fraction of text length */

    /* Parse opts */
    if (argc >= 2 && sea_is_table(args[1])) {
        SeaTable *opts = SEA_AS_TABLE(args[1]);
        SeaValue v;
        if (sea_table_get_str(opts, "duration",   &v)) duration = sea_is_number(v) ? sea_as_number(v) : (double)int_arg(v);
        if (sea_table_get_str(opts, "fps",        &v)) fps      = (int)int_arg(v);
        if (sea_table_get_str(opts, "wave_width", &v)) wave_w   = sea_is_number(v) ? sea_as_number(v) : (double)int_arg(v);
        if (sea_table_get_str(opts, "color",      &v) && !sea_is_nil(v)) {
            Color c = resolve_color(v, 0);
            if (c.ansi < 0) { peak_color.r = c.r; peak_color.g = c.g; peak_color.b = c.b; }
        }
        if (sea_table_get_str(opts, "base",  &v) && !sea_is_nil(v)) {
            Color c = resolve_color(v, 0);
            if (c.ansi < 0) { base_color.r = c.r; base_color.g = c.g; base_color.b = c.b; }
        }
    }

    int n_cells = utf8_display_len(text, text_bytes);
    if (n_cells == 0) return SEA_NIL_VAL;

    int total_frames = (int)(duration * fps);
    if (total_frames < 1) total_frames = 1;
    int frame_ms = 1000 / (fps > 0 ? fps : 30);
    double sigma = wave_w * n_cells;

    /* Hide cursor during animation */
    out("\x1b[?25l");

    for (int frame = 0; frame < total_frames; frame++) {
        /* Wave position: sweeps from -wave_width to 1+wave_width */
        double progress = (double)frame / (double)total_frames;
        double wave_pos = -wave_w + progress * (1.0 + 2.0 * wave_w);
        wave_pos *= n_cells;

        out("\r");  /* return to start of line */

        for (int cell = 0; cell < n_cells; cell++) {
            double dist  = (double)cell - wave_pos;
            double t     = gauss(dist, sigma);

            /* Boost: add a secondary sharper peak for crisp highlight */
            double sharp = gauss(dist, sigma * 0.3) * 0.5;
            double blend = t + sharp;
            if (blend > 1.0) blend = 1.0;

            RGB color = lerp_rgb(base_color, peak_color, blend);
            outf("\x1b[38;2;%d;%d;%dm", color.r, color.g, color.b);

            /* Bold for the peak cells */
            if (blend > 0.7) out("\x1b[1m"); else out("\x1b[22m");

            size_t off = utf8_cell_offset(text, text_bytes, cell);
            size_t cb  = sea_utf8_next(text, text_bytes, off) - off;
            fwrite(text + off, 1, cb, stdout);
        }
        out("\x1b[0m");
        flush();
        sleep_ms(frame_ms);
    }

    /* Final render: show text in resting color */
    out("\r");
    outf("\x1b[38;2;%d;%d;%dm", final_color.r, final_color.g, final_color.b);
    out(text);
    out("\x1b[0m");
    out("\x1b[?25h");  /* show cursor */
    flush();

    return SEA_NIL_VAL;
}

/* ---- Background shimmer (pthread-based) ----------------------------------- */

#define BG_TEXT_MAX 512

typedef struct {
    pthread_t       tid;
    pthread_mutex_t mu;
    volatile int    running;

    RGB    peak;
    RGB    base;
    int    fps;
    double wave_w;
    int    bar_width;

    char   text[BG_TEXT_MAX];
    int    cur;
    int    total;
} BgShimmer;

static BgShimmer g_bg = { .running = 0 };

static double mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void render_progress_inline(int cur, int total, int width) {
    if (total <= 0 || width <= 0) return;
    int filled = (int)((double)cur / (double)total * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;

    if (cur >= total) out("\x1b[32m");
    else              out("\x1b[38;2;100;180;255m");
    out("[");
    for (int i = 0; i < filled; i++)  out("\xe2\x96\x88");  /* █ */
    out("\x1b[0m\x1b[2m");
    for (int i = filled; i < width; i++) out("\xe2\x96\x91"); /* ░ */
    out("\x1b[0m]");
}

static void render_shimmer_frame_raw(const char *text, size_t tbytes,
                                      double wave_pos, RGB peak, RGB base,
                                      double sigma) {
    int n = utf8_display_len(text, tbytes);
    for (int cell = 0; cell < n; cell++) {
        double dist  = (double)cell - wave_pos;
        double t     = gauss(dist, sigma);
        double sharp = gauss(dist, sigma * 0.3) * 0.5;
        double blend = t + sharp;
        if (blend > 1.0) blend = 1.0;
        RGB c = lerp_rgb(base, peak, blend);
        outf("\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
        if (blend > 0.7) out("\x1b[1m"); else out("\x1b[22m");
        size_t off = utf8_cell_offset(text, tbytes, cell);
        size_t cb  = sea_utf8_next(text, tbytes, off) - off;
        fwrite(text + off, 1, cb, stdout);
    }
    out("\x1b[0m");
}

static void *bg_shimmer_thread(void *arg) {
    BgShimmer *s = (BgShimmer *)arg;
    int    frame_ms = 1000 / (s->fps > 0 ? s->fps : 30);
    double cycle    = 1.6;   /* seconds per full sweep */
    double t0       = mono_sec();

    while (s->running) {
        double elapsed  = mono_sec() - t0;
        double phase    = fmod(elapsed / cycle, 1.0);

        pthread_mutex_lock(&s->mu);
        char text[BG_TEXT_MAX];
        strncpy(text, s->text, BG_TEXT_MAX - 1);
        text[BG_TEXT_MAX - 1] = '\0';
        int cur   = s->cur;
        int total = s->total;
        pthread_mutex_unlock(&s->mu);

        size_t tbytes  = strlen(text);
        int    n_cells = utf8_display_len(text, tbytes);
        double sigma   = s->wave_w * (n_cells > 0 ? n_cells : 1);
        double wave_pos = (-s->wave_w + phase * (1.0 + 2.0 * s->wave_w)) * n_cells;

        out("\r  ");
        if (s->bar_width > 0)
            render_progress_inline(cur, total, s->bar_width);
        out("  ");
        render_shimmer_frame_raw(text, tbytes, wave_pos, s->peak, s->base, sigma);
        out("\x1b[K");
        flush();

        sleep_ms(frame_ms);
    }

    out("\r\x1b[K\x1b[?25h");
    flush();
    return NULL;
}

static SeaValue fn_shimmer_start(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (g_bg.running) return SEA_NIL_VAL;

    const char *text = (argc >= 1 && sea_is_string(args[0]))
                       ? SEA_AS_STRING(args[0])->data : "";

    g_bg.peak      = (RGB){200, 220, 255};
    g_bg.base      = (RGB){ 40,  55,  80};
    g_bg.fps       = 30;
    g_bg.wave_w    = 0.35;
    g_bg.bar_width = 20;
    g_bg.cur       = 0;
    g_bg.total     = 0;

    if (argc >= 2 && sea_is_table(args[1])) {
        SeaTable *opts = SEA_AS_TABLE(args[1]);
        SeaValue v;
        if (sea_table_get_str(opts, "fps",        &v)) g_bg.fps       = (int)int_arg(v);
        if (sea_table_get_str(opts, "wave_width",  &v)) g_bg.wave_w    = sea_is_number(v) ? sea_as_number(v) : (double)int_arg(v);
        if (sea_table_get_str(opts, "bar_width",   &v)) g_bg.bar_width = (int)int_arg(v);
        if (sea_table_get_str(opts, "bar_total",   &v)) g_bg.total     = (int)int_arg(v);
        if (sea_table_get_str(opts, "color", &v) && !sea_is_nil(v)) {
            Color c = resolve_color(v, 0);
            if (c.ansi < 0) g_bg.peak = (RGB){c.r, c.g, c.b};
        }
        if (sea_table_get_str(opts, "base",  &v) && !sea_is_nil(v)) {
            Color c = resolve_color(v, 0);
            if (c.ansi < 0) g_bg.base = (RGB){c.r, c.g, c.b};
        }
    }

    strncpy(g_bg.text, text, BG_TEXT_MAX - 1);
    g_bg.text[BG_TEXT_MAX - 1] = '\0';

    pthread_mutex_init(&g_bg.mu, NULL);
    g_bg.running = 1;
    out("\x1b[?25l");
    flush();
    pthread_create(&g_bg.tid, NULL, bg_shimmer_thread, &g_bg);
    return SEA_NIL_VAL;
}

/* shimmer_update(text [, cur, total]) */
static SeaValue fn_shimmer_update(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (!g_bg.running) return SEA_NIL_VAL;

    const char *text = (argc >= 1 && sea_is_string(args[0]))
                       ? SEA_AS_STRING(args[0])->data : g_bg.text;
    int cur   = argc >= 2 ? (int)int_arg(args[1]) : g_bg.cur;
    int total = argc >= 3 ? (int)int_arg(args[2]) : g_bg.total;

    pthread_mutex_lock(&g_bg.mu);
    strncpy(g_bg.text, text, BG_TEXT_MAX - 1);
    g_bg.text[BG_TEXT_MAX - 1] = '\0';
    g_bg.cur   = cur;
    g_bg.total = total;
    pthread_mutex_unlock(&g_bg.mu);
    return SEA_NIL_VAL;
}

/* shimmer_stop() — stops the background thread and clears the line */
static SeaValue fn_shimmer_stop(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    if (!g_bg.running) return SEA_NIL_VAL;
    g_bg.running = 0;
    pthread_join(g_bg.tid, NULL);
    pthread_mutex_destroy(&g_bg.mu);
    return SEA_NIL_VAL;
}

/*
 * shimmer_run(text, fn [, opts]) → return value of fn
 *
 * Inicia o shimmer, chama fn() e para quando fn() terminar.
 */
static SeaValue fn_shimmer_run(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_closure(args[1])) {
        sea_vm_error(vm, "shimmer_run: esperado (texto, função [, opts])");
        return SEA_NIL_VAL;
    }

    SeaValue start_args[2] = { args[0], argc >= 3 ? args[2] : SEA_NIL_VAL };
    fn_shimmer_start(vm, argc >= 3 ? 2 : 1, start_args);

    SeaValue result = sea_vm_call(vm, args[1], NULL, 0);

    fn_shimmer_stop(vm, 0, NULL);
    return result;
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();

    /* Cursor */
    sea_mod_set(mod, "goto",        fn_goto);
    sea_mod_set(mod, "up",          fn_up);
    sea_mod_set(mod, "down",        fn_down);
    sea_mod_set(mod, "left",        fn_left);
    sea_mod_set(mod, "right",       fn_right);
    sea_mod_set(mod, "save",        fn_save);
    sea_mod_set(mod, "restore",     fn_restore);
    sea_mod_set(mod, "home",        fn_home);
    sea_mod_set(mod, "clear",       fn_clear);
    sea_mod_set(mod, "clear_line",  fn_clear_line);
    sea_mod_set(mod, "clear_eol",   fn_clear_eol);
    sea_mod_set(mod, "hide_cursor", fn_hide_cursor);
    sea_mod_set(mod, "show_cursor", fn_show_cursor);

    /* Text */
    sea_mod_set(mod, "write",       fn_write);
    sea_mod_set(mod, "writeln",     fn_writeln);
    sea_mod_set(mod, "reset",       fn_reset);

    /* Input */
    sea_mod_set(mod, "readline",    fn_readline);
    sea_mod_set(mod, "readkey",     fn_readkey);
    sea_mod_set(mod, "readpass",    fn_readpass);

    /* Progress */
    sea_mod_set(mod, "progress",    fn_progress);

    /* Shimmer */
    sea_mod_set(mod, "shimmer",         fn_shimmer);
    sea_mod_set(mod, "shimmer_start",   fn_shimmer_start);
    sea_mod_set(mod, "shimmer_update",  fn_shimmer_update);
    sea_mod_set(mod, "shimmer_stop",    fn_shimmer_stop);
    sea_mod_set(mod, "shimmer_run",     fn_shimmer_run);

    /* Terminal info */
    sea_mod_set(mod, "size",        fn_size);

    return sea_obj_val(&mod->obj);
}
