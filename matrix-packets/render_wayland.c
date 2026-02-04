#define _GNU_SOURCE
#include "render_wayland.h"
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* ── SHM buffer ──────────────────────────────────────────────── */

typedef struct {
    struct wl_buffer *wl_buf;
    void *data;
    size_t size;
    int busy;  /* 1 while compositor holds it */
} shm_buffer_t;

/* ── Wayland state ───────────────────────────────────────────── */

static struct wl_display    *display;
static struct wl_registry   *registry;
static struct wl_compositor *compositor;
static struct wl_shm        *shm;
static struct wl_output     *output;
static struct zwlr_layer_shell_v1   *layer_shell;

static struct wl_surface            *surface;
static struct zwlr_layer_surface_v1 *layer_surface;

static shm_buffer_t buffers[2];
static int pixel_width, pixel_height;  /* from configure */
static int configured = 0;
static int reconfigured = 0;
static int closed = 0;

/* Cell grid */
static int cell_w, cell_h;
static int grid_cols, grid_rows;

/* Per-column damage tracking */
typedef struct {
    int active;   /* stream present this frame */
    int min_row;  /* topmost row touched */
    int max_row;  /* bottommost row touched */
} col_damage_t;

static col_damage_t *col_dmg_prev = NULL;
static col_damage_t *col_dmg_cur  = NULL;

/* Stats bar damage tracking */
static int stats_prev_x, stats_prev_y, stats_prev_w, stats_prev_h;

/* Font */
#define FONT_FAMILY "monospace"
#define FONT_SIZE   14

/* ── helpers ─────────────────────────────────────────────────── */

static int create_shm_file(size_t size) {
    char name[] = "/matrix-packets-XXXXXX";
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void buffer_release(void *data, struct wl_buffer *buf) {
    (void)buf;
    shm_buffer_t *b = data;
    b->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int init_buffer(shm_buffer_t *buf, int w, int h) {
    size_t stride = (size_t)w * 4;
    size_t size = stride * (size_t)h;

    int fd = create_shm_file(size);
    if (fd < 0) return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    buf->wl_buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    buf->data = data;
    buf->size = size;
    buf->busy = 0;

    wl_buffer_add_listener(buf->wl_buf, &buffer_listener, buf);
    return 0;
}

static void destroy_buffer(shm_buffer_t *buf) {
    if (buf->wl_buf) {
        wl_buffer_destroy(buf->wl_buf);
        buf->wl_buf = NULL;
    }
    if (buf->data) {
        munmap(buf->data, buf->size);
        buf->data = NULL;
    }
}

static shm_buffer_t *get_free_buffer(void) {
    for (int i = 0; i < 2; i++) {
        if (!buffers[i].busy) return &buffers[i];
    }
    return NULL;  /* both busy — skip frame */
}

/* ── layer-surface listener ──────────────────────────────────── */

static void layer_surface_configure(void *data,
        struct zwlr_layer_surface_v1 *lsurf, uint32_t serial,
        uint32_t w, uint32_t h) {
    (void)data;

    zwlr_layer_surface_v1_ack_configure(lsurf, serial);

    if ((int)w != pixel_width || (int)h != pixel_height) {
        pixel_width  = (int)w;
        pixel_height = (int)h;

        /* (Re)allocate SHM buffers */
        destroy_buffer(&buffers[0]);
        destroy_buffer(&buffers[1]);
        init_buffer(&buffers[0], pixel_width, pixel_height);
        init_buffer(&buffers[1], pixel_width, pixel_height);

        /* Recompute cell grid */
        grid_cols = pixel_width  / cell_w;
        grid_rows = pixel_height / cell_h;

        /* (Re)allocate damage tracking */
        free(col_dmg_prev);
        free(col_dmg_cur);
        col_dmg_prev = calloc(grid_cols, sizeof(col_damage_t));
        col_dmg_cur  = calloc(grid_cols, sizeof(col_damage_t));

        reconfigured = 1;
    }

    configured = 1;
}

static void layer_surface_closed(void *data,
        struct zwlr_layer_surface_v1 *lsurf) {
    (void)data;
    (void)lsurf;
    closed = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* ── registry listener ───────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *reg,
        uint32_t name, const char *interface, uint32_t version) {
    (void)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!output) {
            output = wl_registry_bind(reg, name, &wl_output_interface,
                    version < 4 ? version : 4);
        }
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name,
                &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
        uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── Measure cell size with Cairo/Pango ──────────────────────── */

static void measure_cell(void) {
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(FONT_FAMILY " " G_STRINGIFY(FONT_SIZE));
    pango_layout_set_font_description(layout, desc);

    /* Measure a single character */
    pango_layout_set_text(layout, "M", 1);
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);

    cell_w = logical.width;
    cell_h = logical.height;

    /* Sanity floor */
    if (cell_w < 6)  cell_w = 8;
    if (cell_h < 10) cell_h = 16;

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);
}

/* ── Color mapping ───────────────────────────────────────────── */

typedef struct { double r, g, b; } rgb_t;

static rgb_t color_for_pair(int pair) {
    switch (pair) {
        case COLOR_INBOUND:  return (rgb_t){0.0, 0.8, 0.0};
        case COLOR_OUTBOUND: return (rgb_t){0.0, 0.8, 0.8};
        case COLOR_HEX:      return (rgb_t){0.9, 0.0, 0.0};
        case COLOR_SRC_IP:   return (rgb_t){0.0, 0.8, 0.8};
        case COLOR_DST_IP:   return (rgb_t){0.0, 0.8, 0.0};
        case COLOR_PORT:     return (rgb_t){0.9, 0.9, 0.0};
        case COLOR_PROTO:    return (rgb_t){0.8, 0.0, 0.8};
        case COLOR_ARROW:    return (rgb_t){0.9, 0.9, 0.9};
        case COLOR_HEAD:     return (rgb_t){1.0, 1.0, 1.0};
        case COLOR_FADING:   return (rgb_t){0.0, 0.8, 0.0};
        default:             return (rgb_t){0.0, 0.8, 0.0};
    }
}

/* ── Public API ──────────────────────────────────────────────── */

int wayland_init(void) {
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return -1;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "Missing required Wayland globals\n");
        if (!layer_shell)
            fprintf(stderr, "  zwlr_layer_shell_v1 not available — is this a wlroots-based compositor?\n");
        return -1;
    }

    /* Measure font cell */
    measure_cell();

    /* Create surface */
    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "matrix-packets");

    /* Anchor to all edges, size 0x0 → compositor provides dimensions */
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_add_listener(layer_surface,
            &layer_surface_listener, NULL);

    /* Initial commit (no buffer) triggers configure */
    wl_surface_commit(surface);

    /* Block until configure arrives */
    while (!configured && wl_display_dispatch(display) != -1)
        ;

    if (!configured) {
        fprintf(stderr, "Wayland: never received configure\n");
        return -1;
    }

    return 0;
}

int render_frame_wayland(unsigned long frame_count) {
    if (closed) return -1;

    shm_buffer_t *buf = get_free_buffer();
    if (!buf) return 0;  /* skip frame */

    /* Reset current-frame damage tracking */
    memset(col_dmg_cur, 0, grid_cols * sizeof(col_damage_t));

    int stride = pixel_width * 4;
    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, pixel_width, pixel_height, stride);
    cairo_t *cr = cairo_create(cs);

    /* Clear to transparent black */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    /* Switch to normal compositing for drawing */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Set up Pango for character drawing */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(
        FONT_FAMILY " " G_STRINGIFY(FONT_SIZE));
    pango_layout_set_font_description(layout, desc);

    char ch_buf[2] = {0, 0};

    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &streams[i];
        if (s->state == STREAM_EMPTY) continue;
        if (s->column < 0 || s->column >= grid_cols) continue;

        int is_fading = (s->state == STREAM_FADING);
        int col = s->column;

        for (int c = 0; c < s->chars_shown; c++) {
            int row = (int)s->row - (s->chars_shown - 1 - c);
            if (row < 0 || row >= grid_rows) continue;

            /* Track per-column damage */
            if (!col_dmg_cur[col].active) {
                col_dmg_cur[col].active  = 1;
                col_dmg_cur[col].min_row = row;
                col_dmg_cur[col].max_row = row;
            } else {
                if (row < col_dmg_cur[col].min_row) col_dmg_cur[col].min_row = row;
                if (row > col_dmg_cur[col].max_row) col_dmg_cur[col].max_row = row;
            }

            int text_idx = s->text_len - s->chars_shown + c;
            if (text_idx < 0 || text_idx >= s->text_len) continue;

            double px_x = col * cell_w;
            double px_y = row * cell_h;

            if (is_fading) {
                if (c == s->chars_shown - 1) {
                    /* Blinking head block */
                    if (frame_count % BLINK_CYCLE < BLINK_ON) {
                        rgb_t clr = color_for_pair(s->colors[0]);
                        /* Bright head */
                        double r = clr.r * 1.3; if (r > 1.0) r = 1.0;
                        double g = clr.g * 1.3; if (g > 1.0) g = 1.0;
                        double b = clr.b * 1.3; if (b > 1.0) b = 1.0;
                        cairo_set_source_rgba(cr, r, g, b, 1.0);
                        cairo_rectangle(cr, px_x, px_y, cell_w, cell_h);
                        cairo_fill(cr);
                    }
                    continue;
                } else {
                    /* Fading trail — dim */
                    rgb_t clr = color_for_pair(s->colors[text_idx]);
                    cairo_set_source_rgba(cr, clr.r, clr.g, clr.b, 1.0);
                }
            } else if (c == s->chars_shown - 1) {
                /* Active head — blinking bright solid block */
                if (frame_count % BLINK_CYCLE < BLINK_ON) {
                    rgb_t clr = color_for_pair(s->colors[0]);
                    double r = clr.r * 1.3; if (r > 1.0) r = 1.0;
                    double g = clr.g * 1.3; if (g > 1.0) g = 1.0;
                    double b = clr.b * 1.3; if (b > 1.0) b = 1.0;
                    cairo_set_source_rgba(cr, r, g, b, 1.0);
                    cairo_rectangle(cr, px_x, px_y, cell_w, cell_h);
                    cairo_fill(cr);
                }
                continue;
            } else {
                /* Trail character */
                rgb_t clr = color_for_pair(s->colors[text_idx]);
                cairo_set_source_rgba(cr, clr.r, clr.g, clr.b, 1.0);
            }

            /* Draw the character */
            ch_buf[0] = s->text[text_idx];
            cairo_move_to(cr, px_x, px_y);
            pango_layout_set_text(layout, ch_buf, 1);
            pango_cairo_show_layout(cr, layout);
        }
    }

    /* Draw stats bar in bottom-right */
    char stats[64];
    if (bytes_per_sec < 1024) {
        snprintf(stats, sizeof(stats), "%lu B/s | %lu pkts",
                 bytes_per_sec, packets_captured);
    } else if (bytes_per_sec < 1024 * 1024) {
        snprintf(stats, sizeof(stats), "%.1f KB/s | %lu pkts",
                 bytes_per_sec / 1024.0, packets_captured);
    } else {
        snprintf(stats, sizeof(stats), "%.1f MB/s | %lu pkts",
                 bytes_per_sec / (1024.0 * 1024.0), packets_captured);
    }

    pango_layout_set_text(layout, stats, -1);
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);

    double stats_x = pixel_width - logical.width - cell_w;
    double stats_y = pixel_height - cell_h;
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.7);
    cairo_move_to(cr, stats_x, stats_y);
    pango_cairo_show_layout(cr, layout);

    /* Current stats bar region */
    int cur_stats_x = (int)stats_x;
    int cur_stats_y = (int)stats_y;
    int cur_stats_w = logical.width + cell_w;
    int cur_stats_h = cell_h;

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    /* Attach buffer */
    wl_surface_attach(surface, buf->wl_buf, 0, 0);

    /* Damage only changed columns (union of prev and cur active regions) */
    for (int c = 0; c < grid_cols; c++) {
        if (!col_dmg_cur[c].active && !col_dmg_prev[c].active)
            continue;

        int r_min, r_max;
        if (col_dmg_cur[c].active && col_dmg_prev[c].active) {
            r_min = col_dmg_cur[c].min_row < col_dmg_prev[c].min_row
                  ? col_dmg_cur[c].min_row : col_dmg_prev[c].min_row;
            r_max = col_dmg_cur[c].max_row > col_dmg_prev[c].max_row
                  ? col_dmg_cur[c].max_row : col_dmg_prev[c].max_row;
        } else if (col_dmg_cur[c].active) {
            r_min = col_dmg_cur[c].min_row;
            r_max = col_dmg_cur[c].max_row;
        } else {
            r_min = col_dmg_prev[c].min_row;
            r_max = col_dmg_prev[c].max_row;
        }

        int px_x = c * cell_w;
        int px_y = r_min * cell_h;
        int px_h = (r_max - r_min + 1) * cell_h;
        wl_surface_damage_buffer(surface, px_x, px_y, cell_w, px_h);
    }

    /* Damage stats bar (union of prev and cur position) */
    wl_surface_damage_buffer(surface, cur_stats_x, cur_stats_y,
                             cur_stats_w, cur_stats_h);
    if (stats_prev_w > 0) {
        wl_surface_damage_buffer(surface, stats_prev_x, stats_prev_y,
                                 stats_prev_w, stats_prev_h);
    }

    wl_surface_commit(surface);
    buf->busy = 1;

    /* Save damage state for next frame */
    col_damage_t *tmp = col_dmg_prev;
    col_dmg_prev = col_dmg_cur;
    col_dmg_cur  = tmp;

    stats_prev_x = cur_stats_x;
    stats_prev_y = cur_stats_y;
    stats_prev_w = cur_stats_w;
    stats_prev_h = cur_stats_h;

    return wl_display_flush(display) < 0 ? -1 : 0;
}

int wayland_dispatch(void) {
    if (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
        return 0;
    }

    wl_display_flush(display);

    struct pollfd pfd = {
        .fd = wl_display_get_fd(display),
        .events = POLLIN,
    };

    /* Non-blocking check */
    if (poll(&pfd, 1, 0) > 0) {
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);
    } else {
        wl_display_cancel_read(display);
    }

    return 0;
}

int wayland_get_fd(void) {
    return wl_display_get_fd(display);
}

int wayland_get_width_cells(void) {
    return grid_cols;
}

int wayland_get_height_cells(void) {
    return grid_rows;
}

int wayland_check_reconfigure(void) {
    if (reconfigured) {
        reconfigured = 0;
        return 1;
    }
    return 0;
}

void wayland_cleanup(void) {
    free(col_dmg_prev);
    free(col_dmg_cur);
    col_dmg_prev = NULL;
    col_dmg_cur  = NULL;

    destroy_buffer(&buffers[0]);
    destroy_buffer(&buffers[1]);

    if (layer_surface) {
        zwlr_layer_surface_v1_destroy(layer_surface);
        layer_surface = NULL;
    }
    if (surface) {
        wl_surface_destroy(surface);
        surface = NULL;
    }
    if (layer_shell) {
        /* layer_shell v3+ has destroy, but we bound <=4 so it's safe */
        zwlr_layer_shell_v1_destroy(layer_shell);
        layer_shell = NULL;
    }
    if (output) {
        wl_output_release(output);
        output = NULL;
    }
    if (shm) {
        wl_shm_destroy(shm);
        shm = NULL;
    }
    if (compositor) {
        wl_compositor_destroy(compositor);
        compositor = NULL;
    }
    if (registry) {
        wl_registry_destroy(registry);
        registry = NULL;
    }
    if (display) {
        wl_display_disconnect(display);
        display = NULL;
    }
}
