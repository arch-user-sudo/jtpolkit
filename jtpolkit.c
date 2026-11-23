/*
 * jtpolkit — a small, zen-styled polkit authentication agent.
 *
 * Wayland + cairo + Pango + polkit-agent, no X11, no GTK, no Qt.
 *
 *   make
 *   ./jtpolkit                       register as the session's agent
 *   ./jtpolkit '#378484'             optional accent override
 *   ./jtpolkitctl accent '#378484'   change the accent while running
 *   ./jtpolkitctl quit               unregister and exit
 *
 * Enter submits a password, Escape cancels.  Windows are server-side
 * decorated, modal in spirit, and only visible while an authentication
 * request is outstanding.
 *
 * Palette from ~/JTLab/config.h (same as jtspeedtest):
 *   bg #030303, fg #FFFFFF, borders #404040,
 *   accent #E8A03C (overridable), success #66B366, font Liberation.
 */

#define _GNU_SOURCE
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"

#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>

#include <glib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Look & feel                                                        */
/* ------------------------------------------------------------------ */

#define DESIGN_W 470.0
#define DESIGN_H 332.0

#define FONT_FAMILY "Liberation"

#define C_BG        0x030303
#define C_BG_TOP    0x070708
#define C_FG        0xFFFFFF
#define C_DIM       0x8C8C8C
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_CARD      0x0F1011
#define C_GREEN     0x66B366
#define C_ERROR     0xD97A7A

/* The accent can be overridden at launch or through the control socket.
 * These aliases keep the drawing code unchanged while allowing the
 * values to be replaced at runtime. */
static unsigned accent_rgb = 0xE8A03C;
static unsigned accent_hi_rgb = 0xF2B25C;
static unsigned accent_lo_rgb = 0xCE8A2F;
static unsigned on_accent_rgb = 0x14100A;
#define C_ACCENT    accent_rgb
#define C_ACCENT_HI accent_hi_rgb
#define C_ACCENT_LO accent_lo_rgb
#define C_ON_ACCENT on_accent_rgb

#define PI 3.14159265358979323846

#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

static double mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int accent_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int accent_parse(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = accent_hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static double accent_linear_channel(unsigned channel)
{
    double v = (double)channel / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double accent_luminance(unsigned rgb)
{
    return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * accent_linear_channel(rgb & 0xFF);
}

static double accent_contrast(double a, double b)
{
    if (a < b) {
        double t = a;
        a = b;
        b = t;
    }
    return (a + 0.05) / (b + 0.05);
}

static unsigned accent_ink(unsigned rgb)
{
    const unsigned dark = 0x14100A;
    const unsigned light = 0xF7F5F0;
    double background = accent_luminance(rgb);
    double dark_contrast = accent_contrast(background,
                                           accent_luminance(dark));
    double light_contrast = accent_contrast(background,
                                            accent_luminance(light));
    return light_contrast > dark_contrast ? light : dark;
}

static unsigned accent_mix(unsigned rgb, unsigned toward, double amount)
{
    int r1 = (int)((rgb >> 16) & 0xFF);
    int g1 = (int)((rgb >> 8) & 0xFF);
    int b1 = (int)(rgb & 0xFF);
    int r2 = (int)((toward >> 16) & 0xFF);
    int g2 = (int)((toward >> 8) & 0xFF);
    int b2 = (int)(toward & 0xFF);
    int r = (int)lround(r1 + (r2 - r1) * amount);
    int g = (int)lround(g1 + (g2 - g1) * amount);
    int b = (int)lround(b1 + (b2 - b1) * amount);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void accent_set(unsigned rgb)
{
    accent_rgb = rgb;
    accent_hi_rgb = accent_mix(rgb, 0xFFFFFF, 0.16);
    accent_lo_rgb = accent_mix(rgb, 0x000000, 0.14);
    on_accent_rgb = accent_ink(rgb);
}

/* Remove a leading '#RRGGBB', a bare RRGGBB, or an explicit --accent COLOR
 * from argv.  Same command-line convention as jtspeedtest. */
static int accent_cli(int *argc, char **argv)
{
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;

        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --accent requires a color\n", argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            value = argv[i] + 9;
            consumed = 1;
        } else if (argv[i][0] == '#') {
            value = argv[i];
            consumed = 1;
        } else if (i == 1) {
            unsigned candidate;
            if (accent_parse(argv[i], &candidate)) {
                value = argv[i];
                consumed = 1;
            }
        }

        if (!value)
            continue;

        unsigned rgb;
        if (!accent_parse(value, &rgb)) {
            fprintf(stderr,
                    "%s: invalid accent color '%s' (expected #RRGGBB)\n",
                    argv[0], value);
            return 0;
        }
        accent_set(rgb);

        for (int j = i; j + consumed < *argc; j++)
            argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct {
    int key;
    PangoFontDescription *fd;
} FontEnt;

#define FONT_CACHE 256
static FontEnt g_fonts[FONT_CACHE];
static int g_font_count;

static PangoFontDescription *font_get(double size, int weight)
{
    int key = (int)lround(size * 10.0) * 10000 + weight;

    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i].key == key)
            return g_fonts[i].fd;

    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);

    if (g_font_count < FONT_CACHE) {
        g_fonts[g_font_count].key = key;
        g_fonts[g_font_count].fd = fd;
        g_font_count++;
    } else {
        /* The cache owns every FontEnt.  Evict one rather than returning an
         * unowned description that no caller frees. */
        pango_font_description_free(g_fonts[0].fd);
        g_fonts[0].key = key;
        g_fonts[0].fd = fd;
    }
    return fd;
}

/* Explicit Pango font options override anything derived from the target
 * surface, so every glyph renders without antialiasing. */
static void
font_antialias_off(PangoLayout *layout)
{
    cairo_font_options_t *opts = cairo_font_options_create();
    cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), opts);
    cairo_font_options_destroy(opts);
}

static PangoLayout *layout_make(cairo_t *cr, const char *text,
                                double size, int weight, double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    font_antialias_off(l);
    pango_layout_set_font_description(l, font_get(size, weight));
    pango_layout_set_text(l, text ? text : "", -1);

    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double text_draw(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        int align, double spacing)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);

    double w = logical.width;
    if (align == ALIGN_CENTER)
        x -= w / 2.0;
    else if (align == ALIGN_RIGHT)
        x -= w;

    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

/* Word-wrapped text, used for the polkit action description. */
static double text_wrap(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        double max_w, double leading, double spacing)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    pango_layout_set_width(l, (int)lround(max_w * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);

    int lines = pango_layout_get_line_count(l);
    for (int i = 0; i < lines; i++) {
        PangoLayoutLine *line = pango_layout_get_line_readonly(l, i);
        cairo_move_to(cr, x, y + i * leading);
        rgba(cr, hex, alpha);
        pango_cairo_show_layout_line(cr, line);
    }

    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double h = logical.height;
    g_object_unref(l);
    return h;
}

/* ------------------------------------------------------------------ */
/*  Authentication state                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char action[256];
    char message[512];
    char user[128];
    char prompt[128];
    char info[512];
    char error[512];
    char password[256];

    int active;         /* a request is outstanding */
    int awaiting;       /* PAM asked for input */
    int echo_on;        /* echo the response (otherwise show dots) */
    int complete;       /* authentication finished, window lingering */
    int ok;             /* finished successfully */
    int initiated;      /* polkit session was started */

    double hide_at;     /* when the lingering window is unmapped */

    PolkitAgentSession *session;
    GTask *task;
    GCancellable *cancellable;
    gulong cancel_id;
} Auth;

typedef struct App App;

static Auth g_auth;
static App *g_app;
static volatile sig_atomic_t g_signal_quit;

/* ---- daemon <-> gui IPC ------------------------------------------- */
/* The daemon owns the polkit registration and never touches Wayland.
 * It forks a short-lived GUI process when a prompt is needed.  Closing
 * the GUI window kills only that child; the daemon stays registered. */

static int g_is_gui;                 /* set in the forked GUI process */
static int g_ipc = -1;               /* socketpair end used by this process */
static pid_t g_gui_pid = -1;         /* daemon: current GUI child */

enum {
    IPC_INIT,       /* daemon -> gui: initial state struct */
    IPC_REQUEST,    /* daemon -> gui: request/prompt */
    IPC_INFO,       /* daemon -> gui: info text */
    IPC_ERROR,      /* daemon -> gui: error text */
    IPC_DONE,       /* daemon -> gui: auth finished */
    IPC_ACCENT,     /* daemon -> gui: new accent */
    IPC_QUIT,       /* daemon -> gui: exit */
    IPC_PASSWORD,   /* gui -> daemon: password response */
    IPC_CANCEL,     /* gui -> daemon: user cancelled */
    IPC_CLOSE       /* gui -> daemon: window closed */
};

typedef struct {
    int type;
    char payload[512];
} IpcMsg;

static int ipc_send(int fd, int type, const char *payload)
{
    IpcMsg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = type;
    if (payload)
        snprintf(msg.payload, sizeof msg.payload, "%s", payload);

    /* The socketpair is blocking and SOCK_SEQPACKET: retry only EINTR and
     * require one complete record rather than splitting it across writes. */
    ssize_t n;
    do {
        n = write(fd, &msg, sizeof msg);
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof msg;
}

/* Returns 1 on a message, 0 on EOF, -1 on error (errno set). */
static int ipc_recv(int fd, IpcMsg *msg)
{
    char *p = (char *)msg;
    size_t left = sizeof *msg;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n > 0) { p += n; left -= (size_t)n; continue; }
        if (n == 0)
            return left == sizeof *msg ? 0 : -1;
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 1;
}

static void gui_spawn(void);
static void gui_stop(void);
static int gui_main(void);

static void app_show_window(void);
static void app_hide_window(void);
static void auth_submit(void);
static void auth_cancel(void);
static void auth_complete(gboolean ok, const char *message);
static void auth_mark_dirty(void);

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

#define GLOW_SIZE 256

static cairo_surface_t *g_glow;
static unsigned g_glow_accent;

static cairo_surface_t *glow_make(int size, double inner_ratio,
                                  unsigned rgb, double amid)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    size, size);
    cairo_t *cr = cairo_create(s);
    double c = size / 2.0;
    cairo_pattern_t *p = cairo_pattern_create_radial(
        c, c, c * inner_ratio, c, c, c);
    cairo_pattern_add_color_stop_rgba(p, 0.0,
        R_(rgb), G_(rgb), B_(rgb), 1.0);
    if (amid >= 0.0)
        cairo_pattern_add_color_stop_rgba(p, 0.55,
            R_(rgb), G_(rgb), B_(rgb), amid);
    cairo_pattern_add_color_stop_rgba(p, 1.0,
        R_(rgb), G_(rgb), B_(rgb), 0.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_destroy(cr);
    return s;
}

static void glow_paint(cairo_t *cr, cairo_surface_t *s,
                       double cx, double cy, double radius, double alpha)
{
    int size = cairo_image_surface_get_width(s);
    double k = (2.0 * radius) / size;
    cairo_save(cr);
    cairo_translate(cr, cx - radius, cy - radius);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, s, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

static void ambient_ensure(void)
{
    if (g_glow && g_glow_accent == accent_rgb)
        return;
    if (g_glow)
        cairo_surface_destroy(g_glow);
    g_glow = glow_make(GLOW_SIZE, 18.0 / 232.0, accent_rgb, 0.28);
    g_glow_accent = accent_rgb;
}

static void paint_ambient(cairo_t *cr, double t)
{
    double breath = 0.5 + 0.5 * sin(t * 0.55);
    double alpha = 0.048 * (0.72 + 0.28 * breath);
    ambient_ensure();
    glow_paint(cr, g_glow, DESIGN_W / 2.0, 112.0, 208.0, alpha);
}

static void paint_dust(cairo_t *cr, double t)
{
    const double x_margin = 24.0;
    const double x_span = DESIGN_W - 2.0 * x_margin;
    const double y_span = 210.0;
    const double y_bottom = 214.0;

    for (int i = 0; i < 18; i++) {
        double seed = (double)(i + 1) * 12.9898;
        double base_x = x_margin + fmod(seed * 37.719, x_span);
        double speed = 5.0 + fmod(seed * 11.13, 13.0);
        double ph = fmod((seed * 19.19 + t * speed) / y_span, 1.0);
        if (ph < 0.0)
            ph += 1.0;
        double y = y_bottom - ph * y_span;
        double x = base_x + sin(t * 0.33 + i * 1.7) *
                   (4.0 + fmod(seed * 3.3, 6.0));
        double a = 0.030 + 0.055 * (0.5 + 0.5 * sin(t * 0.5 + i * 2.1));
        double rad = 0.6 + fmod(seed * 5.7, 1.0);

        cairo_new_sub_path(cr);
        cairo_arc(cr, x, y, rad, 0.0, 2.0 * PI);
        rgba(cr, C_FG, a);
        cairo_fill(cr);
    }
}

static void paint_lock(cairo_t *cr, double cx, double cy, double s)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s, s);

    /* shackle */
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0.0, -2.0, 7.5, PI, 0.0);
    rgba(cr, C_ACCENT, 1.0);
    cairo_set_line_width(cr, 3.0);
    cairo_stroke(cr);

    /* body */
    rounded_rect(cr, -11.0, -2.0, 22.0, 17.0, 3.0);
    rgba(cr, C_ACCENT, 1.0);
    cairo_fill(cr);

    /* keyhole */
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0.0, 4.2, 2.0, 0.0, 2.0 * PI);
    rgba(cr, C_ON_ACCENT, 0.92);
    cairo_fill(cr);
    rounded_rect(cr, -0.9, 4.6, 1.8, 5.0, 0.8);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void paint_header(cairo_t *cr)
{
    paint_lock(cr, 36.0, 30.0, 1.05);

    text_draw(cr, "Authentication", 15.0, 600, C_FG, 0.96,
              60.0, 15.0, ALIGN_LEFT, 0.2);

    char sub[80];
    if (g_auth.action[0]) {
        snprintf(sub, sizeof sub, "%s", g_auth.action);
        if (strlen(sub) > 56) {
            sub[53] = '.';
            sub[54] = '.';
            sub[55] = '.';
            sub[56] = '\0';
        }
    } else {
        snprintf(sub, sizeof sub, "org.freedesktop.policykit");
    }
    text_draw(cr, sub, 9.5, 400, C_DIM, 0.78,
              61.0, 35.0, ALIGN_LEFT, 0.25);

    rgba(cr, C_FG, 0.045);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 24.0, 57.5);
    cairo_line_to(cr, DESIGN_W - 24.0, 57.5);
    cairo_stroke(cr);
}

static void paint_input(cairo_t *cr, double t)
{
    double y = 152.0;
    double h = 48.0;

    text_draw(cr, g_auth.prompt[0] ? g_auth.prompt : "Password: ",
              10.5, 600, C_DIM, 0.92,
              25.0, 124.0, ALIGN_LEFT, 0.4);

    rounded_rect(cr, 24.0, y, DESIGN_W - 48.0, h, 10.0);
    rgba(cr, C_CARD, 0.96);
    cairo_fill(cr);

    int focused = g_auth.active && !g_auth.complete && g_auth.awaiting;
    rgba(cr, focused ? C_ACCENT : C_BORDER, focused ? 0.85 : 0.65);
    cairo_set_line_width(cr, focused ? 1.3 : 1.0);
    rounded_rect(cr, 24.5, y + 0.5, DESIGN_W - 49.0, h - 1.0, 9.5);
    cairo_stroke(cr);

    char shown[512];
    if (!g_auth.password[0]) {
        snprintf(shown, sizeof shown, "%s",
                 focused ? "" : "Enter password");
    } else if (g_auth.echo_on) {
        snprintf(shown, sizeof shown, "%s", g_auth.password);
    } else {
        glong chars = g_utf8_strlen(g_auth.password, -1);
        size_t at = 0;
        for (glong i = 0; i < chars && at + 4 < sizeof shown; i++) {
            memcpy(shown + at, "\xE2\x80\xA2", 3);
            at += 3;
        }
        shown[at] = '\0';
    }

    if (shown[0]) {
        text_draw(cr, shown, 16.0, 400, C_FG, 0.95,
                  38.0, y + 15.0, ALIGN_LEFT, 0.0);
    }

    if (focused && !shown[0]) {
        double blink = 0.5 + 0.5 * sin(t * 3.6);
        rgba(cr, C_ACCENT, 0.30 + 0.55 * blink);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, 38.0, y + 14.0);
        cairo_line_to(cr, 38.0, y + 33.0);
        cairo_stroke(cr);
    }
}

#define BTN_CANCEL_X 154.0
#define BTN_OK_X     278.0
#define BTN_Y        254.0
#define BTN_CANCEL_W 112.0
#define BTN_OK_W     168.0
#define BTN_H        42.0

static int hit_cancel(double x, double y)
{
    return x >= BTN_CANCEL_X && x <= BTN_CANCEL_X + BTN_CANCEL_W &&
           y >= BTN_Y && y <= BTN_Y + BTN_H;
}

static int hit_ok(double x, double y)
{
    return x >= BTN_OK_X && x <= BTN_OK_X + BTN_OK_W &&
           y >= BTN_Y && y <= BTN_Y + BTN_H;
}

static void paint_button(cairo_t *cr, double x, double y, double w, double h,
                         const char *label, int primary,
                         int hover, int press)
{
    if (primary) {
        cairo_pattern_t *fill = cairo_pattern_create_linear(x, y, x, y + h);
        cairo_pattern_add_color_stop_rgba(fill, 0.0,
            R_(C_ACCENT_HI), G_(C_ACCENT_HI), B_(C_ACCENT_HI),
            press ? 0.86 : (hover ? 1.0 : 0.96));
        cairo_pattern_add_color_stop_rgba(fill, 1.0,
            R_(C_ACCENT_LO), G_(C_ACCENT_LO), B_(C_ACCENT_LO),
            press ? 0.86 : (hover ? 1.0 : 0.96));
        rounded_rect(cr, x, y, w, h, 9.0);
        cairo_set_source(cr, fill);
        cairo_fill(cr);
        cairo_pattern_destroy(fill);
        text_draw(cr, label, 12.5, 700, C_ON_ACCENT, 0.96,
                  x + w / 2.0, y + 13.0, ALIGN_CENTER, 0.6);
    } else {
        rounded_rect(cr, x, y, w, h, 9.0);
        rgba(cr, C_CARD, press ? 0.98 : (hover ? 0.92 : 0.78));
        cairo_fill(cr);
        rgba(cr, C_BORDER, hover ? 0.95 : 0.72);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 8.5);
        cairo_stroke(cr);
        text_draw(cr, label, 12.0, 600, C_FG, hover ? 0.94 : 0.78,
                  x + w / 2.0, y + 13.5, ALIGN_CENTER, 0.5);
    }
}

static void paint_footer(cairo_t *cr)
{
    text_draw(cr, "POLKIT  ·  JTLAB", 9.0, 400, C_FAINT, 0.70,
              24.0, DESIGN_H - 22.0, ALIGN_LEFT, 1.0);
    text_draw(cr, "ENTER  AUTHENTICATE     ESC  CANCEL", 9.0, 600,
              C_FAINT, 0.60, DESIGN_W - 24.0, DESIGN_H - 22.0,
              ALIGN_RIGHT, 1.1);
}

static void paint_status(cairo_t *cr)
{
    const char *text;
    unsigned color;
    double alpha;

    if (g_auth.error[0]) {
        text = g_auth.error;
        color = C_ERROR;
        alpha = 0.94;
    } else if (g_auth.info[0]) {
        text = g_auth.info;
        color = C_ACCENT;
        alpha = 0.92;
    } else if (g_auth.complete) {
        text = g_auth.ok ? "Authentication successful"
                         : "Authentication cancelled";
        color = g_auth.ok ? C_GREEN : C_FAINT;
        alpha = 0.92;
    } else {
        text = "Verify your identity to continue.";
        color = C_FAINT;
        alpha = 0.82;
    }

    text_draw(cr, text, 10.0, 400, color, alpha,
              25.0, 218.0, ALIGN_LEFT, 0.15);
}

/* Pointer state used by the button paints. */
static double g_hover_cancel, g_press_cancel;
static double g_hover_ok, g_press_ok;

static void paint_content(cairo_t *cr, double t)
{
    cairo_pattern_t *bg = cairo_pattern_create_linear(0.0, 0.0, 0.0, DESIGN_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0,
        R_(C_BG_TOP), G_(C_BG_TOP), B_(C_BG_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 0.55,
        R_(0x050506), G_(0x050506), B_(0x050506), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0,
        R_(C_BG), G_(C_BG), B_(C_BG), 1.0);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    paint_ambient(cr, t);
    paint_dust(cr, t);
    paint_header(cr);

    text_wrap(cr, g_auth.message[0] ? g_auth.message
                   : "Authentication is required to perform this action.",
              12.5, 400, C_DIM, 0.92,
              24.0, 80.0, DESIGN_W - 48.0, 18.0, 0.1);

    paint_input(cr, t);
    paint_status(cr);

    int busy = !g_auth.active || g_auth.complete;
    paint_button(cr, BTN_CANCEL_X, BTN_Y, BTN_CANCEL_W, BTN_H,
                 "Cancel", 0, (int)g_hover_cancel && !busy,
                 (int)g_press_cancel && !busy);
    paint_button(cr, BTN_OK_X, BTN_Y, BTN_OK_W, BTN_H,
                 "Authenticate", 1, (int)g_hover_ok && !busy,
                 (int)g_press_ok && !busy);

    paint_footer(cr);
}

/* ------------------------------------------------------------------ */
/*  Authentication listener                                            */
/* ------------------------------------------------------------------ */

static void session_request(PolkitAgentSession *session, gchar *request,
                            gboolean echo_on, gpointer user_data)
{
    (void)session;
    (void)user_data;

    g_snprintf(g_auth.prompt, sizeof g_auth.prompt, "%s",
               request ? request : "Password: ");
    g_auth.echo_on = !!echo_on;
    g_auth.awaiting = 1;
    g_auth.password[0] = '\0';

    gui_spawn();

    char payload[sizeof g_auth.prompt + 16];
    snprintf(payload, sizeof payload, "%d|%s", g_auth.echo_on, g_auth.prompt);
    ipc_send(g_ipc, IPC_REQUEST, payload);
}

static void session_info(PolkitAgentSession *session, gchar *text,
                         gpointer user_data)
{
    (void)session;
    (void)user_data;
    g_snprintf(g_auth.info, sizeof g_auth.info, "%s", text ? text : "");
    ipc_send(g_ipc, IPC_INFO, g_auth.info);
}

static void session_error(PolkitAgentSession *session, gchar *text,
                          gpointer user_data)
{
    (void)session;
    (void)user_data;
    g_snprintf(g_auth.error, sizeof g_auth.error, "%s", text ? text : "");
    ipc_send(g_ipc, IPC_ERROR, g_auth.error);
}

static void session_completed(PolkitAgentSession *session,
                              gboolean gained_authorization,
                              gpointer user_data)
{
    (void)session;
    (void)user_data;
    auth_complete(gained_authorization, NULL);
}

static void auth_submit(void)
{
    if (!g_auth.active || g_auth.complete || !g_auth.awaiting)
        return;
    if (!g_is_gui && !g_auth.session)
        return;

    g_auth.awaiting = 0;

    if (g_is_gui) {
        ipc_send(g_ipc, IPC_PASSWORD, g_auth.password);
        memset(g_auth.password, 0, sizeof g_auth.password);
        g_auth.info[0] = '\0';
        auth_mark_dirty();
        return;
    }

    char *response = g_strdup(g_auth.password);
    polkit_agent_session_response(g_auth.session, response ? response : "");
    if (response) {
        memset(response, 0, strlen(response));
        g_free(response);
    }
    memset(g_auth.password, 0, sizeof g_auth.password);
    g_auth.info[0] = '\0';
    auth_mark_dirty();
}

static void auth_cancel(void)
{
    if (!g_auth.active || g_auth.complete)
        return;
    if (g_is_gui) {
        ipc_send(g_ipc, IPC_CANCEL, "");
        return;
    }
    if (g_auth.session)
        polkit_agent_session_cancel(g_auth.session);
    else
        auth_complete(FALSE, "Authentication cancelled");
}

static void auth_complete(gboolean ok, const char *message)
{
    if (!g_auth.active)
        return;

    g_auth.active = 0;
    g_auth.awaiting = 0;
    g_auth.complete = 1;
    g_auth.ok = !!ok;
    g_auth.hide_at = mono_sec() + (ok ? 0.7 : 1.2);

    if (ok)
        g_auth.error[0] = '\0';
    else if (message && *message)
        g_snprintf(g_auth.error, sizeof g_auth.error, "%s", message);
    else if (!g_auth.error[0])
        g_snprintf(g_auth.error, sizeof g_auth.error,
                   "Authentication failed");

    if (g_auth.cancel_id && g_auth.cancellable) {
        g_signal_handler_disconnect(g_auth.cancellable, g_auth.cancel_id);
        g_auth.cancel_id = 0;
    }

    PolkitAgentSession *session = g_auth.session;
    g_auth.session = NULL;
    if (session) {
        g_signal_handlers_disconnect_by_func(session,
            G_CALLBACK(session_request), NULL);
        g_signal_handlers_disconnect_by_func(session,
            G_CALLBACK(session_info), NULL);
        g_signal_handlers_disconnect_by_func(session,
            G_CALLBACK(session_error), NULL);
        g_signal_handlers_disconnect_by_func(session,
            G_CALLBACK(session_completed), NULL);
        g_object_unref(session);
    }

    GTask *task = g_auth.task;
    g_auth.task = NULL;
    GCancellable *cancellable = g_auth.cancellable;
    g_auth.cancellable = NULL;

    if (task) {
        if (ok)
            g_task_return_boolean(task, TRUE);
        else
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "%s", g_auth.error[0] ? g_auth.error
                                                         : "Authentication failed");
        g_object_unref(task);
    }
    if (cancellable)
        g_object_unref(cancellable);

    memset(g_auth.password, 0, sizeof g_auth.password);
    auth_mark_dirty();

    if (!g_is_gui) {
        char payload[64];
        snprintf(payload, sizeof payload, "%d|%s", ok,
                 g_auth.error[0] ? g_auth.error : "");
        ipc_send(g_ipc, IPC_DONE, payload);
    }
}

static void on_cancelled(GCancellable *cancellable, gpointer user_data)
{
    (void)cancellable;
    (void)user_data;
    /* g_cancellable_connect() invokes this synchronously when already
     * cancelled.  With no initiated session there is nothing to hand to
     * polkit, so finish the task here instead. */
    if (!g_auth.initiated)
        auth_complete(FALSE, "Authentication cancelled");
    else
        auth_cancel();
}

typedef struct _JtListener JtListener;
typedef struct _JtListenerClass JtListenerClass;

struct _JtListener {
    PolkitAgentListener parent_instance;
};

struct _JtListenerClass {
    PolkitAgentListenerClass parent_class;
};

G_DEFINE_TYPE(JtListener, jt_listener, POLKIT_AGENT_TYPE_LISTENER)

static void jt_initiate_authentication(PolkitAgentListener *listener,
                                       const gchar *action_id,
                                       const gchar *message,
                                       const gchar *icon_name,
                                       PolkitDetails *details,
                                       const gchar *cookie,
                                       GList *identities,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    (void)icon_name;
    (void)details;

    if (g_auth.active)
        auth_complete(FALSE, "Another authentication request is in progress");

    /* Do not let a lingering GUI from the previous request display this
     * one's password prompt under the previous action and user text. */
    gui_stop();

    memset(&g_auth, 0, sizeof g_auth);
    g_auth.active = 1;

    g_snprintf(g_auth.action, sizeof g_auth.action, "%s",
               action_id ? action_id : "org.freedesktop.policykit");
    g_snprintf(g_auth.message, sizeof g_auth.message, "%s",
               message ? message : "Authentication is required to perform this action.");
    g_snprintf(g_auth.prompt, sizeof g_auth.prompt, "%s", "Password: ");

    PolkitIdentity *identity = NULL;
    gboolean owned_identity = FALSE;
    for (GList *l = identities; l; l = l->next) {
        if (POLKIT_IS_UNIX_USER(l->data)) {
            identity = POLKIT_IDENTITY(l->data);
            const gchar *name = polkit_unix_user_get_name(
                POLKIT_UNIX_USER(identity));
            if (name)
                g_snprintf(g_auth.user, sizeof g_auth.user, "%s", name);
            break;
        }
    }
    if (!g_auth.user[0])
        g_snprintf(g_auth.user, sizeof g_auth.user, "%s",
                   g_get_user_name());
    if (!identity) {
        identity = polkit_unix_user_new((gint)getuid());
        owned_identity = TRUE;
    }

    g_auth.task = g_task_new(listener, cancellable, callback, user_data);
    g_task_set_source_tag(g_auth.task,
                          (gpointer)jt_initiate_authentication);
    /* on_cancelled() completes the task explicitly; do not also let GTask
     * complete it from the cancellable and race with our own completion. */
    g_task_set_check_cancellable(g_auth.task, FALSE);

    g_auth.session = polkit_agent_session_new(identity, cookie);
    if (owned_identity)
        g_object_unref(identity);
    g_signal_connect(g_auth.session, "request",
                     G_CALLBACK(session_request), NULL);
    g_signal_connect(g_auth.session, "show-info",
                     G_CALLBACK(session_info), NULL);
    g_signal_connect(g_auth.session, "show-error",
                     G_CALLBACK(session_error), NULL);
    g_signal_connect(g_auth.session, "completed",
                     G_CALLBACK(session_completed), NULL);

    if (cancellable) {
        g_auth.cancellable = g_object_ref(cancellable);
        g_auth.cancel_id = g_cancellable_connect(cancellable,
            G_CALLBACK(on_cancelled), NULL, NULL);
        /* Already cancelled (or cancelled from another thread): the handler
         * finished and cleared g_auth, so do not start a live session. */
        if (!g_auth.active)
            return;
    }

    app_show_window();
    g_auth.initiated = 1;
    polkit_agent_session_initiate(g_auth.session);

    auth_mark_dirty();
}

static gboolean jt_initiate_authentication_finish(
    PolkitAgentListener *listener, GAsyncResult *res, GError **error)
{
    (void)listener;
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void jt_listener_class_init(JtListenerClass *klass)
{
    PolkitAgentListenerClass *base = POLKIT_AGENT_LISTENER_CLASS(klass);
    base->initiate_authentication = jt_initiate_authentication;
    base->initiate_authentication_finish =
        jt_initiate_authentication_finish;
}

static void jt_listener_init(JtListener *self)
{
    (void)self;
}

/* ------------------------------------------------------------------ */
/*  Wayland backend                                                    */
/* ------------------------------------------------------------------ */

#define NBUFS 4
#define MAX_OUTPUTS 8
#define BTN_LEFT 0x110

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height;
    int busy;
    int retired;
} ShmBuf;

typedef struct {
    struct wl_output *output;
    uint32_t registry_name;
    int32_t scale;
    int used;
    int entered;
    App *app;
} Output;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wl_seat *seat;
    uint32_t seat_name;
    struct wl_pointer *pointer;
    uint32_t pointer_serial;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    uint32_t cursor_shape;

    int configured;
    int dirty;
    int visible;
    struct wl_callback *frame_cb;

    int scale;
    int fixed_scale;
    int has_argb;
    ShmBuf bufs[NBUFS];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    Output outputs[MAX_OUTPUTS];
    int output_count;

    double t0;
    double last_draw;
    double pointer_x, pointer_y;

    int quit;

    int ctl_fd;
    int ctl_client_fd;
    char ctl_buf[256];
    size_t ctl_len;
};

static int create_anon_file(size_t size)
{
    int fd = memfd_create("jtpolkit", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0)
            return fd;
        close(fd);
    }

    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/jtpolkit-XXXXXX",
             (dir && *dir) ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void shm_buffer_destroy(ShmBuf *b)
{
    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        b->buffer = NULL;
    }
    if (b->data) {
        munmap(b->data, b->size);
        b->data = NULL;
    }
    b->size = 0;
    b->width = b->height = 0;
    b->busy = b->retired = 0;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ShmBuf *b = data;
    b->busy = 0;
    if (b->retired)
        shm_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int shm_buffer_create(App *a, ShmBuf *b, int w, int h)
{
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = create_anon_file(size);
    if (fd < 0)
        return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) {
        munmap(data, size);
        close(fd);
        return -1;
    }

    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b->buffer) {
        munmap(data, size);
        return -1;
    }

    b->data = data;
    b->size = size;
    b->width = w;
    b->height = h;
    b->busy = 0;
    b->retired = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static ShmBuf *next_buffer(App *a, int w, int h)
{
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired &&
            b->width == w && b->height == h)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer && shm_buffer_create(a, b, w, h) == 0)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired) {
            shm_buffer_destroy(b);
            if (shm_buffer_create(a, b, w, h) == 0)
                return b;
        }
    }

    return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    App *a = data;
    wl_callback_destroy(cb);
    a->frame_cb = NULL;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void request_frame(App *a)
{
    if (!a->surface || a->frame_cb)
        return;
    a->frame_cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(a->frame_cb, &frame_listener, a);
}

static int app_draw(App *a, double t)
{
    if (!a->configured || !a->surface || !a->shm || !a->visible)
        return 0;

    int w = (int)lround(DESIGN_W * a->scale);
    int h = (int)lround(DESIGN_H * a->scale);
    if (w < 1 || h < 1)
        return 0;

    ShmBuf *b = next_buffer(a, w, h);
    if (!b)
        return 0;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, R_(C_BG), G_(C_BG), B_(C_BG), 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    paint_content(cr, t);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_attach(a->surface, b->buffer, 0, 0);
    wl_surface_damage(a->surface, 0, 0,
                      (int)DESIGN_W, (int)DESIGN_H);
    request_frame(a);
    wl_surface_commit(a->surface);
    b->busy = 1;
    return 1;
}

static void auth_mark_dirty(void)
{
    if (g_app)
        g_app->dirty = 1;
}

static void app_show_window(void)
{
    App *a = g_app;
    if (!a || !a->surface)
        return;
    a->visible = 1;
    a->dirty = 1;
    g_hover_cancel = g_press_cancel = 0.0;
    g_hover_ok = g_press_ok = 0.0;
}

static void app_hide_window(void)
{
    App *a = g_app;
    if (!a || !a->surface || !a->visible)
        return;
    a->visible = 0;
    a->dirty = 0;
    g_hover_cancel = g_press_cancel = 0.0;
    g_hover_ok = g_press_ok = 0.0;
    if (a->frame_cb) {
        wl_callback_destroy(a->frame_cb);
        a->frame_cb = NULL;
    }
    wl_surface_attach(a->surface, NULL, 0, 0);
    wl_surface_damage(a->surface, 0, 0, (int)DESIGN_W, (int)DESIGN_H);
    wl_surface_commit(a->surface);
}

static void recalc_scale(App *a)
{
    if (a->fixed_scale)
        return;

    int s = 0;
    for (int i = 0; i < a->output_count; i++) {
        if (!a->outputs[i].used)
            continue;
        if (a->outputs[i].entered && a->outputs[i].scale > s)
            s = a->outputs[i].scale;
    }
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    if (s == a->scale)
        return;

    a->scale = s;
    a->dirty = 1;
    if (a->surface && wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, s);

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer)
            continue;
        if (b->busy)
            b->retired = 1;
        else
            shm_buffer_destroy(b);
    }
}

/* ---- protocol listeners ------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)shm;
    App *a = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        a->has_argb = 1;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial)
{
    App *a = data;
    xdg_surface_ack_configure(xs, serial);
    a->configured = 1;
    a->dirty = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)t;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    App *a = data;
    /* Tell the daemon we were closed; it cancels the polkit request.
     * Only this GUI process exits -- the daemon stays registered. */
    ipc_send(g_ipc, IPC_CLOSE, "");
    a->quit = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void decoration_configure(void *data,
                                 struct zxdg_toplevel_decoration_v1 *dec,
                                 uint32_t mode)
{
    (void)data;
    (void)dec;
    (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t phys_w,
                            int32_t phys_h, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y;
    (void)phys_w; (void)phys_h; (void)subpixel;
    (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh)
{
    (void)data; (void)output; (void)flags; (void)w; (void)h; (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    Output *o = data;
    o->scale = factor;
    recalc_scale(o->app);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (!a->outputs[i].used)
            continue;
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 1;
            recalc_scale(a);
            break;
        }
    }
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (!a->outputs[i].used)
            continue;
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 0;
            recalc_scale(a);
            break;
        }
    }
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void update_cursor(App *a)
{
    if (!a->cursor_shape_manager || !a->pointer)
        return;
    if (!a->cursor_shape_device) {
        a->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
            a->cursor_shape_manager, a->pointer);
        a->cursor_shape = 0;
    }

    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    int busy = !g_auth.active || g_auth.complete;
    if (!busy && (hit_cancel(a->pointer_x, a->pointer_y) ||
                  hit_ok(a->pointer_x, a->pointer_y)))
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    else if (!busy && a->pointer_y >= 152.0 && a->pointer_y <= 200.0 &&
             a->pointer_x >= 24.0 && a->pointer_x <= DESIGN_W - 24.0)
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    else if (a->pointer_y < 57.0)
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;

    if (shape != a->cursor_shape) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_shape_device,
                                            a->pointer_serial, shape);
        a->cursor_shape = shape;
    }
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer;
    (void)surface;
    App *a = data;
    a->pointer_serial = serial;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    g_hover_cancel = hit_cancel(a->pointer_x, a->pointer_y);
    g_hover_ok = hit_ok(a->pointer_x, a->pointer_y);
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    App *a = data;
    a->pointer_x = -1;
    a->pointer_y = -1;
    g_hover_cancel = g_press_cancel = 0.0;
    g_hover_ok = g_press_ok = 0.0;
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer;
    (void)time;
    App *a = data;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    g_hover_cancel = hit_cancel(a->pointer_x, a->pointer_y);
    g_hover_ok = hit_ok(a->pointer_x, a->pointer_y);
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)time;
    App *a = data;

    if (button != BTN_LEFT)
        return;

    double x = a->pointer_x;
    double y = a->pointer_y;
    int busy = !g_auth.active || g_auth.complete;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (a->toplevel && a->seat && y < 57.0)
            xdg_toplevel_move(a->toplevel, a->seat, serial);
        g_press_cancel = (!busy && hit_cancel(x, y)) ? 1.0 : 0.0;
        g_press_ok = (!busy && hit_ok(x, y)) ? 1.0 : 0.0;
    } else {
        if (g_press_cancel && hit_cancel(x, y))
            auth_cancel();
        else if (g_press_ok && hit_ok(x, y))
            auth_submit();
        g_press_cancel = 0.0;
        g_press_ok = 0.0;
    }
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    App *a = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }

    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km;
    a->xkb_state = st;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static void password_append(const char *s)
{
    size_t used = strlen(g_auth.password);
    size_t n = strlen(s);
    if (used + n >= sizeof g_auth.password - 1)
        return;
    memcpy(g_auth.password + used, s, n + 1);
}

static void password_backspace(void)
{
    size_t len = strlen(g_auth.password);
    if (!len)
        return;
    char *start = g_auth.password;
    char *end = g_auth.password + len;
    char *prev = g_utf8_find_prev_char(start, end);
    if (prev)
        *prev = '\0';
    else
        g_auth.password[0] = '\0';
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    App *a = data;

    if (!a->xkb_state)
        return;

    xkb_state_update_key(a->xkb_state, key + 8,
        state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;
    if (!g_auth.active || g_auth.complete)
        return;

    a->dirty = 1;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);

    if (sym == XKB_KEY_Escape) {
        auth_cancel();
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        auth_submit();
        return;
    }
    if (sym == XKB_KEY_BackSpace) {
        password_backspace();
        return;
    }
    if (sym == XKB_KEY_Delete) {
        g_auth.password[0] = '\0';
        return;
    }
    if (sym == XKB_KEY_Tab)
        return;

    /* Ignore modifier-only keys and control combinations. */
    if (xkb_state_mod_index_is_active(a->xkb_state,
            xkb_keymap_mod_get_index(a->xkb_keymap, "Control"),
            XKB_STATE_MODS_EFFECTIVE) > 0)
        return;

    char buf[8];
    int n = xkb_keysym_to_utf8(sym, buf, sizeof buf);
    if (n > 0 && buf[0] != '\0' && (unsigned char)buf[0] >= 0x20)
        password_append(buf);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t dep,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)keyboard;
    (void)serial;
    App *a = data;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t caps)
{
    App *a = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        /* The cursor-shape device becomes inert with its wl_pointer. */
        if (a->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
            a->cursor_shape_device = NULL;
            a->cursor_shape = 0;
        }
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void seat_release(App *a)
{
    if (a->cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
        a->cursor_shape_device = NULL;
        a->cursor_shape = 0;
    }
    if (a->pointer) {
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }
    if (a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
    if (a->seat) {
        wl_seat_destroy(a->seat);
        a->seat = NULL;
    }
    a->seat_name = 0;
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    App *a = data;

    if (!strcmp(interface, wl_compositor_interface.name)) {
        a->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(a->shm, &shm_listener, a);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        a->wm_base = wl_registry_bind(registry, name,
                                      &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
    } else if (!strcmp(interface,
                       zxdg_decoration_manager_v1_interface.name)) {
        a->deco_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       xdg_toplevel_icon_manager_v1_interface.name)) {
        a->icon_manager = wl_registry_bind(
            registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       wp_cursor_shape_manager_v1_interface.name)) {
        a->cursor_shape_manager = wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        if (a->seat && a->seat_name == name)
            return;
        /* Keep only one active seat; the UI has a single pointer/keyboard. */
        if (a->seat)
            seat_release(a);
        a->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        if (a->seat) {
            a->seat_name = name;
            wl_seat_add_listener(a->seat, &seat_listener, a);
        }
    } else if (!strcmp(interface, wl_output_interface.name)) {
        /* Keep slots stable: live wl_output listeners point at &outputs[i]. */
        int slot = -1;
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            if (!a->outputs[i].used) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            struct wl_output *out = wl_registry_bind(registry, name,
                                                     &wl_output_interface,
                                                     version < 2 ? version : 2);
            if (out) {
                Output *o = &a->outputs[slot];
                o->output = out;
                o->registry_name = name;
                o->scale = 1;
                o->used = 1;
                o->entered = 0;
                o->app = a;
                wl_output_add_listener(out, &output_listener, o);
                if (slot + 1 > a->output_count)
                    a->output_count = slot + 1;
            }
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)registry;
    App *a = data;

    if (a->seat && a->seat_name == name) {
        seat_release(a);
        return;
    }

    for (int i = 0; i < a->output_count; i++) {
        Output *o = &a->outputs[i];
        if (!o->used || o->registry_name != name)
            continue;
        if (o->output)
            wl_output_destroy(o->output);
        memset(o, 0, sizeof *o);
        recalc_scale(a);
        a->dirty = 1;
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int wayland_start(App *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtpolkit: cannot connect to a Wayland display\n");
        return 0;
    }

    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    if (wl_display_roundtrip(a->display) < 0 ||
        wl_display_roundtrip(a->display) < 0) {
        fprintf(stderr, "jtpolkit: Wayland roundtrip failed\n");
        return 0;
    }

    if (!a->compositor || !a->shm || !a->wm_base) {
        fprintf(stderr, "jtpolkit: missing wl_compositor, wl_shm or "
                        "xdg_wm_base\n");
        return 0;
    }
    if (!a->has_argb) {
        fprintf(stderr, "jtpolkit: compositor has no ARGB8888 shm format\n");
        return 0;
    }

    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface) {
        fprintf(stderr, "jtpolkit: cannot create wl_surface\n");
        return 0;
    }
    wl_surface_add_listener(a->surface, &surface_listener, a);

    if (wl_surface_get_version(a->surface) < 3) {
        a->scale = 1;
        a->fixed_scale = 1;
    }

    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);
    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "JTLab Authentication");
    xdg_toplevel_set_app_id(a->toplevel, "jtpolkit");
    xdg_toplevel_set_min_size(a->toplevel,
                              (int)DESIGN_W, (int)DESIGN_H);
    xdg_toplevel_set_max_size(a->toplevel,
                              (int)DESIGN_W, (int)DESIGN_H);

    if (a->deco_manager) {
        a->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, a->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(
            a->decoration, &decoration_listener, a);
        zxdg_toplevel_decoration_v1_set_mode(
            a->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    if (a->icon_manager) {
        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(a->icon_manager);
        if (icon) {
            xdg_toplevel_icon_v1_set_name(icon, "jtpolkit");
            xdg_toplevel_icon_manager_v1_set_icon(a->icon_manager,
                                                  a->toplevel, icon);
            xdg_toplevel_icon_v1_destroy(icon);
        }
    }

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);

    /* Start unmapped: no buffer means no window until a request arrives. */
    wl_surface_attach(a->surface, NULL, 0, 0);
    wl_surface_commit(a->surface);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Control socket (daemon only)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    int client_fd;
    /* Filesystem identity of our bound path; fstat(fd) returns sockfs. */
    dev_t ctl_dev;
    ino_t ctl_ino;
    char buf[256];
    size_t len;
    double last_active;   /* time of most recent client input */
    int quit;
} Ctl;

static Ctl g_ctl = { .fd = -1, .client_fd = -1 };

static void ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtpolkit-ctl.sock", rt);
}

/* A non-blocking connect distinguishes a live server from a stale path. */
static int ctl_path_live(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    int err = errno;
    close(fd);
    if (rc == 0 || err == EINPROGRESS || err == EAGAIN)
        return 1;
    return err != ENOENT && err != ECONNREFUSED && err != ECONNABORTED;
}

static void ctl_ack_client(void)
{
    if (g_ctl.client_fd < 0)
        return;
    char ack = 'A';
    ssize_t n;
    do {
        n = write(g_ctl.client_fd, &ack, 1);
    } while (n < 0 && errno == EINTR);
}

static void ctl_unlink_owned(const char *path, dev_t dev, ino_t ino)
{
    if (!path || !*path || (dev == 0 && ino == 0))
        return;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) &&
        st.st_dev == dev && st.st_ino == ino)
        unlink(path);
}

static void ctl_handle_line(char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    if (!strcmp(line, "quit")) {
        g_ctl.quit = 1;
        auth_cancel();
        ipc_send(g_ipc, IPC_QUIT, "");
        ctl_ack_client();
        return;
    }

    if (strncmp(line, "accent", 6) == 0 &&
        (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) {
            accent_set(rgb);
            char hex[16];
            snprintf(hex, sizeof hex, "#%06X", rgb);
            ipc_send(g_ipc, IPC_ACCENT, hex);
        }
    }
    ctl_ack_client();
}

static void ctl_close_client(void)
{
    if (g_ctl.client_fd >= 0) {
        close(g_ctl.client_fd);
        g_ctl.client_fd = -1;
    }
    g_ctl.len = 0;
}

static void ctl_process(int flush)
{
    size_t start = 0;
    for (size_t i = 0; i < g_ctl.len; i++) {
        if (g_ctl.buf[i] == '\n') {
            g_ctl.buf[i] = '\0';
            ctl_handle_line(g_ctl.buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(g_ctl.buf, g_ctl.buf + start, g_ctl.len - start);
        g_ctl.len -= start;
    }
    if (flush && g_ctl.len > 0) {
        g_ctl.buf[g_ctl.len] = '\0';
        ctl_handle_line(g_ctl.buf);
        g_ctl.len = 0;
    }
}

static void ctl_read_client(void)
{
    char buf[128];
    for (;;) {
        ssize_t n = read(g_ctl.client_fd, buf, sizeof(buf));
        if (n > 0) {
            g_ctl.last_active = mono_sec();
            size_t room = sizeof(g_ctl.buf) - 1 - g_ctl.len;
            if ((size_t)n > room) {
                ctl_close_client();
                return;
            }
            memcpy(g_ctl.buf + g_ctl.len, buf, (size_t)n);
            g_ctl.len += (size_t)n;
            ctl_process(0);
            continue;
        }
        if (n == 0) {
            ctl_process(1);
            ctl_close_client();
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctl_process(1);
            ctl_close_client();
            return;
        }
        return;
    }
}

static void ctl_accept(void)
{
    for (;;) {
        int fd = accept4(g_ctl.fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        /* The single-instance probe connects and immediately disconnects.
         * Do not let that connection displace a real command. */
        char probe;
        ssize_t peek;
        do {
            peek = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (peek < 0 && errno == EINTR);
        if (peek == 0 || (peek < 0 && errno != EAGAIN &&
                          errno != EWOULDBLOCK)) {
            close(fd);
            continue;
        }
        if (g_ctl.client_fd >= 0) {
            close(fd);
            continue;
        }
        g_ctl.client_fd = fd;
        g_ctl.len = 0;
        g_ctl.last_active = mono_sec();
    }
}

static int ctl_start(void)
{
    char path[256];
    ctl_path(path, sizeof(path));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (!path[0] || strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "jtpolkit: control socket path too long: %s\n", path);
        return 0;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "jtpolkit: control path is not a socket: %s\n", path);
            return 0;
        }
        /* Never unlink a socket that belongs to another user (the /tmp
         * fallback is world-writable). */
        if (st.st_uid != geteuid()) {
            fprintf(stderr,
                    "jtpolkit: control socket is owned by another user: %s\n",
                    path);
            return 0;
        }
    }
    if (ctl_path_live(path)) {
        fprintf(stderr, "jtpolkit: control socket already active: %s\n", path);
        return 0;
    }
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("jtpolkit: control socket");
        return 0;
    }
    /* A Unix socket's mode is fixed at bind() time. */
    mode_t old_umask = umask(0077);
    int bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (bound < 0) {
        fprintf(stderr, "jtpolkit: cannot bind %s: %s\n", path, strerror(errno));
        close(fd);
        return 0;
    }
    if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "jtpolkit: cannot identify bound socket: %s\n", path);
        close(fd);
        unlink(path);
        return 0;
    }
    g_ctl.ctl_dev = st.st_dev;
    g_ctl.ctl_ino = st.st_ino;

    if (listen(fd, 4) < 0) {
        fprintf(stderr, "jtpolkit: cannot listen on %s: %s\n",
                path, strerror(errno));
        close(fd);
        ctl_unlink_owned(path, g_ctl.ctl_dev, g_ctl.ctl_ino);
        return 0;
    }

    g_ctl.fd = fd;
    return 1;
}

static void ctl_cleanup(void)
{
    ctl_close_client();
    if (g_ctl.fd >= 0) {
        char path[256];
        ctl_path(path, sizeof(path));
        ctl_unlink_owned(path, g_ctl.ctl_dev, g_ctl.ctl_ino);
        close(g_ctl.fd);
        g_ctl.fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Daemon <-> GUI process management                                  */
/* ------------------------------------------------------------------ */

static int g_ipc_done;   /* GUI: daemon sent IPC_DONE, exit after linger */

/* The daemon owns the polkit registration and a GDBus connection.  A forked
 * GUI child must not keep those descriptors alive: if the daemon is killed
 * while a prompt is open, a stale bus connection would keep the agent
 * registered and block a replacement.  Keep only stdio and the IPC socket. */
static void close_inherited_fds(int keep)
{
    DIR *dir = opendir("/proc/self/fd");
    if (dir) {
        int dfd = dirfd(dir);
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            int fd = atoi(ent->d_name);
            if (fd > 2 && fd != keep && fd != dfd)
                close(fd);
        }
        closedir(dir);
        return;
    }
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 4096)
        max = 4096;
    for (int fd = 3; fd < max; fd++)
        if (fd != keep)
            close(fd);
}

static void gui_spawn(void)
{
    if (g_gui_pid > 0) {
        int status;
        pid_t r = waitpid(g_gui_pid, &status, WNOHANG);
        if (r == 0 && g_ipc >= 0)
            return;                 /* live child with a live IPC link */
        if (r == 0) {
            /* Child has dropped its IPC end but has not exited yet; it is
             * shutting down.  Do not hand a new prompt to a dying process. */
            kill(g_gui_pid, SIGTERM);
            waitpid(g_gui_pid, NULL, 0);
        }
        g_gui_pid = -1;
    }
    if (g_ipc >= 0) {
        close(g_ipc);
        g_ipc = -1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return;
    }
    if (pid == 0) {
        /* Child: run the Wayland GUI.  g_auth is inherited with the
         * request details already filled in. */
        close(sv[0]);
        /* Do not keep the daemon's control endpoints alive in the child:
         * a killed daemon must leave no reachable but unserviced socket. */
        if (g_ctl.fd >= 0) {
            close(g_ctl.fd);
            g_ctl.fd = -1;
        }
        if (g_ctl.client_fd >= 0) {
            close(g_ctl.client_fd);
            g_ctl.client_fd = -1;
        }
        close_inherited_fds(sv[1]);
        g_ipc = sv[1];
        g_is_gui = 1;
        int rc = gui_main();
        _exit(rc);
    }
    close(sv[1]);
    g_ipc = sv[0];
    g_gui_pid = pid;
}

static void gui_stop(void)
{
    if (g_ipc >= 0) {
        ipc_send(g_ipc, IPC_QUIT, "");
        close(g_ipc);
        g_ipc = -1;
    }
    if (g_gui_pid > 0) {
        kill(g_gui_pid, SIGTERM);
        waitpid(g_gui_pid, NULL, 0);
        g_gui_pid = -1;
    }
}

/* ------------------------------------------------------------------ */
/*  GUI process: Wayland event loop                                    */
/* ------------------------------------------------------------------ */

static void app_cleanup(App *a)
{
    if (g_glow) {
        cairo_surface_destroy(g_glow);
        g_glow = NULL;
    }
    if (a->frame_cb) {
        wl_callback_destroy(a->frame_cb);
        a->frame_cb = NULL;
    }
    if (a->keyboard)
        wl_keyboard_destroy(a->keyboard);
    if (a->cursor_shape_device)
        wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
    if (a->pointer)
        wl_pointer_destroy(a->pointer);
    if (a->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(a->cursor_shape_manager);
    if (a->decoration)
        zxdg_toplevel_decoration_v1_destroy(a->decoration);
    if (a->toplevel)
        xdg_toplevel_destroy(a->toplevel);
    if (a->xdg_surface)
        xdg_surface_destroy(a->xdg_surface);
    if (a->surface)
        wl_surface_destroy(a->surface);
    if (a->seat)
        wl_seat_destroy(a->seat);
    if (a->deco_manager)
        zxdg_decoration_manager_v1_destroy(a->deco_manager);
    if (a->icon_manager)
        xdg_toplevel_icon_manager_v1_destroy(a->icon_manager);
    if (a->wm_base)
        xdg_wm_base_destroy(a->wm_base);
    if (a->shm)
        wl_shm_destroy(a->shm);
    if (a->compositor)
        wl_compositor_destroy(a->compositor);

    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output)
            wl_output_destroy(a->outputs[i].output);
    }
    if (a->registry)
        wl_registry_destroy(a->registry);

    if (a->xkb_state)
        xkb_state_unref(a->xkb_state);
    if (a->xkb_keymap)
        xkb_keymap_unref(a->xkb_keymap);
    if (a->xkb_ctx)
        xkb_context_unref(a->xkb_ctx);

    if (a->display) {
        for (int i = 0; i < NBUFS; i++) {
            ShmBuf *b = &a->bufs[i];
            if (b->buffer && !b->busy) {
                wl_buffer_destroy(b->buffer);
                b->buffer = NULL;
            }
        }
        wl_display_flush(a->display);
        wl_display_disconnect(a->display);
        a->display = NULL;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->data) {
            munmap(b->data, b->size);
            b->data = NULL;
        }
    }
}

static void gui_handle_ipc(IpcMsg *msg, App *a)
{
    switch (msg->type) {
    case IPC_REQUEST: {
        int echo = 0;
        const char *prompt = strchr(msg->payload, '|');
        if (prompt) {
            echo = atoi(msg->payload);
            prompt++;
        } else {
            prompt = msg->payload;
        }
        snprintf(g_auth.prompt, sizeof g_auth.prompt, "%s", prompt);
        g_auth.echo_on = !!echo;
        g_auth.awaiting = 1;
        g_auth.password[0] = '\0';
        g_auth.error[0] = '\0';
        g_auth.info[0] = '\0';
        g_auth.active = 1;
        g_auth.complete = 0;
        app_show_window();
        a->dirty = 1;
        break;
    }
    case IPC_INFO:
        snprintf(g_auth.info, sizeof g_auth.info, "%s", msg->payload);
        g_auth.error[0] = '\0';
        a->dirty = 1;
        break;
    case IPC_ERROR:
        snprintf(g_auth.error, sizeof g_auth.error, "%s", msg->payload);
        g_auth.info[0] = '\0';
        a->dirty = 1;
        break;
    case IPC_DONE: {
        int ok = atoi(msg->payload);
        const char *err = strchr(msg->payload, '|');
        g_auth.active = 0;
        g_auth.awaiting = 0;
        g_auth.complete = 1;
        g_auth.ok = !!ok;
        g_auth.hide_at = mono_sec() + (ok ? 0.7 : 1.2);
        if (!ok && err && err[1])
            snprintf(g_auth.error, sizeof g_auth.error, "%s", err + 1);
        if (g_ipc_done == 0)
            g_ipc_done = 1;
        a->dirty = 1;
        break;
    }
    case IPC_ACCENT: {
        unsigned rgb;
        if (accent_parse(msg->payload, &rgb)) {
            accent_set(rgb);
            a->dirty = 1;
        }
        break;
    }
    case IPC_QUIT:
        a->quit = 1;
        break;
    default:
        break;
    }
}

/* GUI pump: Wayland fd + IPC fd, no GLib main context. */
static int gui_pump(App *a, int timeout_ms)
{
    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;
    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        want_write = 1;
    }

    struct pollfd pfd[2];
    pfd[0].fd = wl_display_get_fd(a->display);
    pfd[0].events = POLLIN | (want_write ? POLLOUT : 0);
    pfd[0].revents = 0;
    pfd[1].fd = g_ipc;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;

    int pr = poll(pfd, 2, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    if (pfd[0].revents & POLLOUT) {
        if (wl_display_flush(a->display) < 0 && errno != EAGAIN)
            return -1;
    }
    if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    if (pfd[0].revents & POLLIN) {
        if (wl_display_dispatch(a->display) < 0)
            return -1;
    }
    if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
        IpcMsg msg;
        int r = ipc_recv(g_ipc, &msg);
        if (r <= 0)
            return -1;          /* daemon disappeared */
        gui_handle_ipc(&msg, a);
    }
    return 0;
}

static int gui_main(void)
{
    App a;
    memset(&a, 0, sizeof a);
    /* g_auth is inherited from the daemon via fork() with the action,
     * message, user and active state already set. */
    a.scale = 1;
    a.pointer_x = a.pointer_y = -1.0;
    g_app = &a;

    const char *scale_env = getenv("JTPOLKIT_SCALE");
    if (scale_env && *scale_env) {
        int s = (int)lround(atof(scale_env));
        if (s < 1) s = 1;
        if (s > 3) s = 3;
        a.scale = s;
        a.fixed_scale = 1;
    }

    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!a.xkb_ctx) {
        g_app = NULL;
        return 1;
    }

    if (!wayland_start(&a)) {
        app_cleanup(&a);
        g_app = NULL;
        return 1;
    }

    a.t0 = mono_sec();

    while (!a.quit && !g_signal_quit) {
        double now = mono_sec();

        if (g_auth.complete && g_auth.hide_at > 0.0 &&
            now >= g_auth.hide_at) {
            app_hide_window();
            if (g_ipc_done)
                break;
            memset(&g_auth, 0, sizeof g_auth);
        }

        double interval = a.visible ? 1.0 / 20.0 : 1.0;
        int due = a.dirty || (a.visible && now - a.last_draw >= interval);
        if (a.configured && due && !a.frame_cb && a.visible) {
            if (app_draw(&a, now - a.t0))
                a.dirty = 0;
            a.last_draw = now;
        }

        int timeout;
        if (!a.configured || a.frame_cb)
            timeout = 100;
        else if (a.visible) {
            double wait = interval - (mono_sec() - a.last_draw);
            if (wait < 0.0) wait = 0.0;
            timeout = (int)(wait * 1000.0) + 1;
        } else {
            timeout = 100;
        }

        if (gui_pump(&a, timeout) < 0)
            break;
    }

    app_cleanup(&a);
    if (g_ipc >= 0) {
        close(g_ipc);
        g_ipc = -1;
    }
    g_app = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Daemon process: polkit registration, no Wayland                    */
/* ------------------------------------------------------------------ */

static int daemon_poll(int timeout_ms)
{
    struct pollfd pfd[3];
    int nfds = 0;
    int ctl_idx = -1, client_idx = -1, ipc_idx = -1;

    if (g_ctl.fd >= 0) {
        ctl_idx = nfds;
        pfd[nfds].fd = g_ctl.fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (g_ctl.client_fd >= 0) {
        client_idx = nfds;
        pfd[nfds].fd = g_ctl.client_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (g_ipc >= 0) {
        ipc_idx = nfds;
        pfd[nfds].fd = g_ipc;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (nfds == 0) {
        poll(NULL, 0, timeout_ms);
        return 0;
    }

    int pr = poll(pfd, (nfds_t)nfds, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN))
        ctl_accept();
    if (client_idx >= 0 && pfd[client_idx].revents)
        ctl_read_client();
    if (ipc_idx >= 0 && (pfd[ipc_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
        IpcMsg msg;
        int r = ipc_recv(g_ipc, &msg);
        if (r <= 0) {
            /* GUI exited (e.g. window closed).  Cancel the request.
             * Leave g_gui_pid set so the main loop can reap it. */
            close(g_ipc);
            g_ipc = -1;
            if (!g_ctl.quit && g_auth.active && !g_auth.complete)
                auth_cancel();
        } else if (msg.type == IPC_CLOSE) {
            if (!g_ctl.quit && g_auth.active && !g_auth.complete)
                auth_cancel();
        } else if (msg.type == IPC_PASSWORD) {
            if (g_auth.active && !g_auth.complete && g_auth.awaiting &&
                g_auth.session) {
                g_auth.awaiting = 0;
                char *password = g_strdup(msg.payload);
                polkit_agent_session_response(g_auth.session,
                                              password ? password : "");
                if (password) {
                    memset(password, 0, strlen(password));
                    g_free(password);
                }
                memset(g_auth.password, 0, sizeof g_auth.password);
            }
        } else if (msg.type == IPC_CANCEL) {
            if (g_auth.active && !g_auth.complete)
                auth_cancel();
        }
    }
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

static int daemon_main(void)
{
    memset(&g_auth, 0, sizeof g_auth);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!ctl_start())
        return 1;

    /* Register for this process' login session; fall back to the process
     * subject on systems without a logind session. */
    GError *error = NULL;
    PolkitSubject *subject = polkit_unix_session_new_for_process_sync(
        (gint)getpid(), NULL, &error);
    if (!subject) {
        g_clear_error(&error);
        subject = polkit_unix_process_new_for_owner(
            (gint)getpid(), (guint64)0, (gint)getuid());
    }

    JtListener *listener = g_object_new(jt_listener_get_type(), NULL);
    gpointer registration = polkit_agent_listener_register(
        POLKIT_AGENT_LISTENER(listener), POLKIT_AGENT_REGISTER_FLAGS_NONE,
        subject, "/org/jtlab/AuthenticationAgent", NULL, &error);
    g_object_unref(subject);

    if (!registration) {
        fprintf(stderr, "jtpolkit: cannot register agent: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(listener);
        ctl_cleanup();
        return 1;
    }

    while (!g_ctl.quit && !g_signal_quit) {
        /* Reap exited GUI children so they don't linger as zombies. */
        if (g_gui_pid > 0) {
            int status;
            pid_t r = waitpid(g_gui_pid, &status, WNOHANG);
            if (r == g_gui_pid)
                g_gui_pid = -1;
        }

        /* A stalled single-slot client must not block later commands
         * (notably "quit").  jtpolkitctl waits at most two seconds for its
         * acknowledgement, so five seconds of silence is unambiguous. */
        if (g_ctl.client_fd >= 0 &&
            mono_sec() - g_ctl.last_active > 5.0)
            ctl_close_client();

        /* Polkit callbacks are delivered through the default GLib context. */
        if (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);
        if (daemon_poll(100) < 0)
            break;
    }

    if (g_auth.active)
        auth_complete(FALSE, "Authentication agent exited");
    if (g_auth.session) {
        g_signal_handlers_disconnect_by_func(g_auth.session,
            G_CALLBACK(session_request), NULL);
        g_signal_handlers_disconnect_by_func(g_auth.session,
            G_CALLBACK(session_completed), NULL);
        g_object_unref(g_auth.session);
        g_auth.session = NULL;
    }
    memset(g_auth.password, 0, sizeof g_auth.password);

    gui_stop();
    polkit_agent_listener_unregister(registration);
    g_object_unref(listener);
    ctl_cleanup();
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (!accent_cli(&argc, argv))
        return 2;

    if (argc >= 2 && (!strcmp(argv[1], "-h") ||
                      !strcmp(argv[1], "--help"))) {
        printf("usage: %s [ACCENT]\n\n"
               "  ACCENT              #RRGGBB or RRGGBB, for example '#378484'\n"
               "  --accent COLOR      explicit form of ACCENT\n\n"
               "the default accent is #E8A03C (amber)\n\n"
               "runs a resident polkit authentication daemon.  A GUI window\n"
               "appears only while a request is outstanding; closing it cancels\n"
               "that request but leaves the daemon running.\n\n"
               "live accent: jtpolkitctl accent '#RRGGBB'\n"
               "quit daemon: jtpolkitctl quit\n"
               "requires a Wayland compositor supporting xdg-shell\n",
               argv[0]);
        return 0;
    }

    /* The GUI child is forked from the daemon; it never re-enters here. */
    return daemon_main();
}
