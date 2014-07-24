#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <xcb/xcb.h>
#include <xcb/xinerama.h>
#include <xcb/randr.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

// Here be dragons

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define indexof(c,s) (strchr((s),(c))-(s))

typedef struct monitor_t {
    int x, width;
    xcb_window_t window;
    cairo_surface_t *surface; 
    cairo_t *cr;
    struct monitor_t *prev, *next;
} monitor_t;

typedef struct area_t {
    int begin, end, align, button;
    xcb_window_t window;
    char *cmd;
} area_t;

#define N 20

typedef struct area_stack_t {
    int pos;
    area_t slot[N];
} area_stack_t;

typedef struct color_t {
    double r, g, b, a;
} color_t;

enum {
    ATTR_OVERL = (1<<0),
    ATTR_UNDERL = (1<<1),
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

enum {
    GC_DRAW = 0,
    GC_CLEAR,
    GC_ATTR,
    GC_MAX
};

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_visualtype_t *vt;
static xcb_colormap_t colormap;
static monitor_t *monhead, *montail;
static uint32_t attrs = 0;
static bool dock = false;
static bool topbar = true;
static int bw = -1, bh = -1, bx = 0, by = 0;
static int bu = 1; /* Underline height */
static char *mfont = NULL;
static double mfont_size = 10.0;
static uint32_t fgc, bgc; 
static area_stack_t astack;

enum {
    PAL_BG,
    PAL_FG,
    PAL_ATTR,
    PAL_MAX,
};

static color_t palette[PAL_MAX];

void
cairo_set_color (cairo_t *cr, const int i)
{
    cairo_set_source_rgba(cr, palette[i].r, palette[i].g, palette[i].b, palette[i].a);
}

void
fill_rect (cairo_t *cr, const int i, int x, int y, int width, int height)
{
    cairo_set_color(cr, i);
    cairo_rectangle(cr, x, y, width, height);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
}

void
cairo_copy (cairo_t *cr, cairo_surface_t *s, int sx, int sy, int dx, int dy, int w, int h)
{
    cairo_set_source_surface(cr, s, dx - sx, dy - sy);
    cairo_rectangle (cr, dx, dy, w, h);
    cairo_fill (cr);
}

int
draw_char (monitor_t *mon, int x, int align, char *ch)
{
    cairo_font_extents_t ext;
    cairo_text_extents_t te;
    cairo_font_extents(mon->cr, &ext);
    cairo_text_extents(mon->cr, ch, &te);

    int ch_width = (int)te.x_advance + 1;
    /*printf("%f %i %i\n", te.x_advance, (int)te.x_advance, ch_width);*/

    switch (align) {
        case ALIGN_C:
            cairo_copy(mon->cr, mon->surface, mon->width / 2 - x / 2, 0, mon->width / 2 - (x + ch_width) / 2, 0, x, bh);
            x = mon->width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            cairo_copy(mon->cr, mon->surface, mon->width - x, 0, mon->width - x - ch_width, 0, x, bh);
            x = mon->width - ch_width;
            break;
    }

    /* Draw the background first */
    fill_rect(mon->cr, PAL_BG, x, by, ch_width, bh);

    /* String baseline coordinates */
    cairo_move_to(mon->cr, x, bh / 2 + ext.height / 2 - ext.descent);
    cairo_set_color(mon->cr, PAL_FG);
    cairo_show_text(mon->cr, ch);

    /* We can render both at the same time */
    if (attrs & ATTR_OVERL)
        fill_rect(mon->cr, PAL_ATTR, x, 0, ch_width, bu);
    if (attrs & ATTR_UNDERL)
        fill_rect(mon->cr, PAL_ATTR, x, bh - bu, ch_width, bu);

    return ch_width; 
}

uint32_t
parse_color (const char *str, char **end, const uint32_t def)
{
    xcb_alloc_named_color_reply_t *nc_reply;
    int str_len;
    uint32_t ret;

    if (!str)
        return def;

    /* Reset */
    if (str[0] == '-') {
        if (end)
            *end = (char *)str + 1;
        return def;
    }

    /* Hex representation */
    if (str[0] == '#') {
        errno = 0;
        uint32_t tmp = strtoul(str + 1, end, 16);
        /* Some error checking is definitely good */
        if (errno)
            tmp = def;
        return tmp;
    }

    /* Actual color name, resolve it */
    str_len = 0;
    while (isalpha(str[str_len]))
        str_len++;

    nc_reply = xcb_alloc_named_color_reply(c, xcb_alloc_named_color(c, colormap, str_len, str), NULL);

    if (!nc_reply)
        fprintf(stderr, "Could not alloc color \"%.*s\"\n", str_len, str);
    ret = (nc_reply) ? nc_reply->pixel : def;
    free(nc_reply);

    if (end)
        *end = (char *)str + str_len;

    return ret;
}

void
convert_color (const uint32_t col, color_t *out) 
{
    out->b = ((col >> 0)&0xff) / 255.0;
    out->g = ((col >> 8)&0xff) / 255.0;
    out->r = ((col >>16)&0xff) / 255.0;
    /*out->a = ((col >>24)&0xff) / 255.0;*/
    out->a = 1.0;
}

void
set_attribute (const char modifier, const char attribute)
{
    int pos = indexof(attribute, "ou");

    if (pos < 0) {
        fprintf(stderr, "Invalid attribute \"%c\" found\n", attribute);
        return;
    }

    switch (modifier) {
        case '+': attrs |= (1<<pos); break;
        case '-': attrs &=~(1<<pos); break;
        case '!': attrs ^= (1<<pos); break;
    }
}


area_t *
area_get (xcb_window_t win, const int x)
{
    for (int i = 0; i < astack.pos; i++)
        if (astack.slot[i].window == win && x > astack.slot[i].begin && x < astack.slot[i].end)
            return &astack.slot[i];
    return NULL;
}

void
area_shift (xcb_window_t win, const int align, int delta)
{
    if (align == ALIGN_L)
        return;
    if (align == ALIGN_C)
        delta /= 2;

    for (int i = 0; i < astack.pos; i++) {
        if (astack.slot[i].window == win && astack.slot[i].align == align) {
            astack.slot[i].begin -= delta;
            astack.slot[i].end -= delta;
        }
    }
}

bool
area_add (char *str, const char *optend, char **end, monitor_t *mon, const int x, const int align, const int button)
{
    char *p = str;
    area_t *a = &astack.slot[astack.pos];

    if (astack.pos == N) {
        fprintf(stderr, "astack overflow!\n");
        return false;
    }

    /* A wild close area tag appeared! */
    if (*p != ':') {
        *end = p;

        /* Basic safety checks */
        if (!a->cmd || a->align != align || a->window != mon->window)
            return false;

        const int size = x - a->begin;

        switch (align) {
            case ALIGN_L:
                a->end = x;
                break;
            case ALIGN_C:
                a->begin = mon->width / 2 - size / 2 + a->begin / 2;
                a->end = a->begin + size;
                break;
            case ALIGN_R:
                /* The newest is the rightmost one */
                a->begin = mon->width - size;
                a->end = mon->width;
                break;
        }

        astack.pos++;

        return true;
    }

    char *trail = strchr(++p, ':');

    /* Find the trailing : and make sure it's whitin the formatting block, also reject empty commands */
    if (!trail || p == trail || trail > optend) {
        *end = p;
        return false;
    }

    *trail = '\0';

    /* This is a pointer to the string buffer allocated in the main */
    a->cmd = p;
    a->align = align;
    a->begin = x;
    a->window = mon->window;
    a->button = button;

    *end = trail + 1;

    return true;
}

void
parse (char *text)
{
    monitor_t *cur_mon;
    int pos_x, align, button;
    char *p = text, *end;
    uint32_t tmp;

    pos_x = 0;
    align = ALIGN_L;
    cur_mon = monhead;

    memset(&astack, 0, sizeof(area_stack_t));

    for (monitor_t *m = monhead; m != NULL; m = m->next) {
        cairo_set_operator(m->cr, CAIRO_OPERATOR_SOURCE);
        fill_rect(m->cr, PAL_BG, 0, 0, m->width, bh);
    }

    for (;;) {
        if (*p == '\0' || *p == '\n')
            return;

        if (*p == '%' && p++ && *p == '{' && (end = strchr(p++, '}'))) {
            while (p < end) {
                while (isspace(*p))
                    p++;

                switch (*p++) {
                    case '+': set_attribute('+', *p++); break;
                    case '-': set_attribute('-', *p++); break;
                    case '!': set_attribute('!', *p++); break;

                    case 'R':
                              /*tmp = fgc;*/
                              /*fgc = bgc;*/
                              /*bgc = tmp;*/
                              break;

                    case 'l': pos_x = 0; align = ALIGN_L; break;
                    case 'c': pos_x = 0; align = ALIGN_C; break;
                    case 'r': pos_x = 0; align = ALIGN_R; break;

                    case 'A': 
                              button = XCB_BUTTON_INDEX_1;
                              /* The range is 1-5 */
                              if (isdigit(*p) && (*p > '0' && *p < '6'))
                                  button = *p++ - '0';
                              area_add(p, end, &p, cur_mon, pos_x, align, button);
                              break;

                    case 'B': convert_color(parse_color(p, &p, bgc), &palette[PAL_BG]); 
                              break;
                    case 'F': convert_color(parse_color(p, &p, fgc), &palette[PAL_FG]); 
                              break;
                    case 'U': convert_color(parse_color(p, &p, fgc), &palette[PAL_ATTR]); 
                              break;

                    case 'S':
                              if (*p == '+' && cur_mon->next)
                              { cur_mon = cur_mon->next; }
                              else if (*p == '-' && cur_mon->prev)
                              { cur_mon = cur_mon->prev; }
                              else if (*p == 'f')
                              { cur_mon = monhead; }
                              else if (*p == 'l')
                              { cur_mon = montail ? montail : monhead; }
                              else if (isdigit(*p))
                              { cur_mon = monhead;
                                for (int i = 0; i != *p-'0' && cur_mon->next; i++)
                                    cur_mon = cur_mon->next;
                              }
                              else
                              { p++; continue; }

                              p++;
                              pos_x = 0;
                              break;

                    /* In case of error keep parsing after the closing } */
                    default:
                        p = end;
                }
            }
            /* Eat the trailing } */
            p++;
        } else {
            const int utf8_size[] = {
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 0xC0-0xCF
                2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 0xD0-0xDF
                3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, // 0xE0-0xEF
                4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, // 0xF0-0xFF
            };
            int size = ((uint8_t)p[0]&0x80) ? utf8_size[(uint8_t)p[0]^0x80] : 1;
                    
            char tmp[size];
            for (int i = 0; i < size; i++)
                tmp[i] = *p++;
            tmp[size] = '\0';

            int w = draw_char(cur_mon, pos_x, align, tmp);

            pos_x += w;
            area_shift(cur_mon->window, align, w);
        }
    }
}

enum {
    NET_WM_WINDOW_TYPE,
    NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_DESKTOP,
    NET_WM_STRUT_PARTIAL,
    NET_WM_STRUT,
    NET_WM_STATE,
    NET_WM_STATE_STICKY,
    NET_WM_STATE_ABOVE,
};

void
set_ewmh_atoms (void)
{
    const char *atom_names[] = {
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_DESKTOP",
        "_NET_WM_STRUT_PARTIAL",
        "_NET_WM_STRUT",
        "_NET_WM_STATE",
        /* Leave those at the end since are batch-set */
        "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_ABOVE",
    };
    const int atoms = sizeof(atom_names)/sizeof(char *);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
    xcb_atom_t atom_list[atoms];
    xcb_intern_atom_reply_t *atom_reply;

    /* As suggested fetch all the cookies first (yum!) and then retrieve the
     * atoms to exploit the async'ness */
    for (int i = 0; i < atoms; i++)
        atom_cookie[i] = xcb_intern_atom(c, 0, strlen(atom_names[i]), atom_names[i]);

    for (int i = 0; i < atoms; i++) {
        atom_reply = xcb_intern_atom_reply(c, atom_cookie[i], NULL);
        if (!atom_reply)
            return;
        atom_list[i] = atom_reply->atom;
        free(atom_reply);
    }

    /* Prepare the strut array */
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        int strut[12] = {0};
        if (topbar) {
            strut[2] = bh;
            strut[8] = mon->x;
            strut[9] = mon->x + mon->width;
        } else {
            strut[3]  = bh;
            strut[10] = mon->x;
            strut[11] = mon->x + mon->width;
        }

        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
        xcb_change_property(c, XCB_PROP_MODE_APPEND,  mon->window, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ -1 } );
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
    }
}

monitor_t *
monitor_new (int x, int y, int width, int height)
{
    monitor_t *ret;

    ret = calloc(1, sizeof(monitor_t));
    if (!ret) {
        fprintf(stderr, "Failed to allocate new monitor\n");
        exit(EXIT_FAILURE);
    }

    ret->x = x;
    ret->width = width;
    ret->next = ret->prev = NULL;

    int win_y = (topbar ? by : height - bh - by) + y;
    ret->window = xcb_generate_id(c);

    int depth = (vt->visual_id == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
    xcb_create_window(c, depth, ret->window, scr->root,
            x, win_y, width, bh, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, vt->visual_id,
            XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP,
            (const uint32_t []){ scr->black_pixel, scr->black_pixel, dock, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS, colormap });

    ret->surface = cairo_xcb_surface_create(c, ret->window, vt, width, height);
    if (!cairo_surface_status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "awwwwww\n");
    }
    ret->cr = cairo_create(ret->surface);

    cairo_select_font_face (ret->cr, mfont? mfont: "fixed", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(ret->cr, mfont_size);

    return ret;
}

void
monitor_add (monitor_t *mon)
{
    if (!monhead) {
        monhead = mon;
    } else if (!montail) {
        montail = mon;
        monhead->next = mon;
        mon->prev = monhead;
    } else {
        mon->prev = montail;
        montail->next = mon;
        montail = montail->next;
    }
}

int
rect_sort_cb (const void *p1, const void *p2)
{
    const xcb_rectangle_t *r1 = (xcb_rectangle_t *)p1;
    const xcb_rectangle_t *r2 = (xcb_rectangle_t *)p2;

    if (r1->x < r2->x || r1->y < r2->y)
        return -1;
    if (r1->x > r2->x || r1->y > r2->y)
        return  1;

    return 0;
}

void
monitor_create_chain (xcb_rectangle_t *rects, const int num)
{
    int i;
    int width = 0, height = 0;
    int left = bx;

    /* Sort before use */
    qsort(rects, num, sizeof(xcb_rectangle_t), rect_sort_cb);

    for (i = 0; i < num; i++) {
        int h = rects[i].y + rects[i].height;
        /* Accumulated width of all monitors */
        width += rects[i].width;
        /* Get height of screen from y_offset + height of lowest monitor */
        if (h >= height)
        height = h;
    }

    if (bw < 0)
        bw = width - bx;

    if (bh < 0 || bh > height)
        bh = 18;

    /* Check the geometry */
    if (bx + bw > width || by + bh > height) {
        fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
        exit(EXIT_FAILURE);
    }

    /* Left is a positive number or zero therefore monitors with zero width are excluded */
    width = bw;
    for (i = 0; i < num; i++) {
        if (rects[i].y + rects[i].height < by)
            continue;
        if (rects[i].width > left) {
            monitor_t *mon = monitor_new(
                    rects[i].x + left,
                    rects[i].y,
                    min(width, rects[i].width - left),
                    rects[i].height);

            monitor_add(mon);

            width -= rects[i].width - left;

            /* No need to check for other monitors */
            if (width <= 0)
                break;
        }

        left -= rects[i].width;

        if (left < 0)
            left = 0;
    }
}

void
get_randr_monitors (void)
{
    xcb_randr_get_screen_resources_current_reply_t *rres_reply;
    xcb_randr_output_t *outputs;
    int i, j, num, valid = 0;

    rres_reply = xcb_randr_get_screen_resources_current_reply(c,
            xcb_randr_get_screen_resources_current(c, scr->root), NULL);

    if (!rres_reply) {
        fprintf(stderr, "Failed to get current randr screen resources\n");
        return;
    }

    num = xcb_randr_get_screen_resources_current_outputs_length(rres_reply);
    outputs = xcb_randr_get_screen_resources_current_outputs(rres_reply);


    /* There should be at least one output */
    if (num < 1) {
        free(rres_reply);
        return;
    }

    xcb_rectangle_t rects[num];

    /* Get all outputs */
    for (i = 0; i < num; i++) {
        xcb_randr_get_output_info_reply_t *oi_reply;
        xcb_randr_get_crtc_info_reply_t *ci_reply;

        oi_reply = xcb_randr_get_output_info_reply(c, xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME), NULL);

        /* Output disconnected or not attached to any CRTC ? */
        if (!oi_reply || oi_reply->crtc == XCB_NONE || oi_reply->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            rects[i].width = 0;
            continue;
        }

        ci_reply = xcb_randr_get_crtc_info_reply(c,
                xcb_randr_get_crtc_info(c, oi_reply->crtc, XCB_CURRENT_TIME), NULL);

        free(oi_reply);

        if (!ci_reply) {
            fprintf(stderr, "Failed to get RandR ctrc info\n");
            free(rres_reply);
            return;
        }

        if (ci_reply->rotation & (XCB_RANDR_ROTATION_ROTATE_90|XCB_RANDR_ROTATION_ROTATE_270))
            rects[i] = (xcb_rectangle_t){ ci_reply->x, ci_reply->y, ci_reply->height, ci_reply->width };
        else
            rects[i] = (xcb_rectangle_t){ ci_reply->x, ci_reply->y, ci_reply->width, ci_reply->height };

        free(ci_reply);

        valid++;
    }

    free(rres_reply);

    /* Check for clones and inactive outputs */
    for (i = 0; i < num; i++) {
        if (rects[i].width == 0)
            continue;

        for (j = 0; j < num; j++) {
            /* Does I countain J ? */

            if (i != j && rects[j].width) {
                if (rects[j].x >= rects[i].x && rects[j].x + rects[j].width <= rects[i].x + rects[i].width &&
                    rects[j].y >= rects[i].y && rects[j].y + rects[j].height <= rects[i].y + rects[i].height) {
                    rects[j].width = 0;
                    valid--;
                }
            }
        }
    }

    if (valid < 1) {
        fprintf(stderr, "No usable RandR output found\n");
        return;
    }

    xcb_rectangle_t r[valid];

    for (i = j = 0; i < num && j < valid; i++)
        if (rects[i].width != 0)
            r[j++] = rects[i];

    monitor_create_chain(r, valid);
}

void
get_xinerama_monitors (void)
{
    xcb_xinerama_query_screens_reply_t *xqs_reply;
    xcb_xinerama_screen_info_iterator_t iter;
    int screens;

    xqs_reply = xcb_xinerama_query_screens_reply(c,
            xcb_xinerama_query_screens_unchecked(c), NULL);

    iter = xcb_xinerama_query_screens_screen_info_iterator(xqs_reply);
    screens = iter.rem;

    xcb_rectangle_t rects[screens];

    /* Fetch all the screens first */
    for (int i = 0; iter.rem; i++) {
        rects[i].x = iter.data->x_org;
        rects[i].y = iter.data->y_org;
        rects[i].width = iter.data->width;
        rects[i].height = iter.data->height;
        xcb_xinerama_screen_info_next(&iter);
    }

    free(xqs_reply);

    monitor_create_chain(rects, screens);
}

xcb_visualtype_t *
get_visual_type (void)
{
    xcb_depth_iterator_t iter;

    iter = xcb_screen_allowed_depths_iterator(scr);

    /* Try to find a RGBA visual */
    while (iter.rem) {
        xcb_visualtype_t *vis = xcb_depth_visuals(iter.data);

        if (iter.data->depth == 32)
            return vis;

        xcb_depth_next(&iter);
    }
    iter = xcb_screen_allowed_depths_iterator(scr);

    /* Try to find a RGBA visual */
    while (iter.rem) {
        xcb_visualtype_t *vis = xcb_depth_visuals(iter.data);

        if (iter.data->depth == 24)
            return vis;

        xcb_depth_next(&iter);
    }

    /* Fallback to the default one */
    return NULL;
}

void
xconn (void)
{
    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "Couldn't connect to X\n");
        exit(EXIT_FAILURE);
    }

    /* Grab infos from the first screen */
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    /* Try to get a RGBA visual and build the colormap for that */
    vt = get_visual_type();

    colormap = xcb_generate_id(c);
    xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, scr->root, vt->visual_id);
}

void
init (void)
{
    /* To make the alignment uniform */
    /*main_font->height = alt_font->height = max(main_font->height, alt_font->height);*/

    /* Generate a list of screens */
    const xcb_query_extension_reply_t *qe_reply;

    /* Initialiaze monitor list head and tail */
    monhead = montail = NULL;

    /* Check if RandR is present */
    qe_reply = xcb_get_extension_data(c, &xcb_randr_id);

    if (qe_reply && qe_reply->present) {
        get_randr_monitors();
    } else {
        qe_reply = xcb_get_extension_data(c, &xcb_xinerama_id);

        /* Check if Xinerama extension is present and active */
        if (qe_reply && qe_reply->present) {
            xcb_xinerama_is_active_reply_t *xia_reply;
            xia_reply = xcb_xinerama_is_active_reply(c, xcb_xinerama_is_active(c), NULL);

            if (xia_reply && xia_reply->state)
                get_xinerama_monitors();

            free(xia_reply);
        }
    }

    if (!monhead) {
        /* If I fits I sits */
        if (bw < 0)
            bw = scr->width_in_pixels - bx;

        /* Adjust the height */
        if (bh < 0 || bh > scr->height_in_pixels)
            bh = 18;

        /* Check the geometry */
        if (bx + bw > scr->width_in_pixels || by + bh > scr->height_in_pixels) {
            fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
            exit(EXIT_FAILURE);
        }

        /* If no RandR outputs or Xinerama screens, fall back to using whole screen */
        monhead = monitor_new(0, 0, bw, scr->height_in_pixels);
    }

    if (!monhead)
        exit(EXIT_FAILURE);

    /* For WM that support EWMH atoms */
    set_ewmh_atoms();

    /* Make the bar visible and clear the pixmap */
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        fill_rect(mon->cr, PAL_BG, 0, 0, mon->width, bh);
        xcb_map_window(c, mon->window);
    }

    xcb_flush(c);
}

void
cleanup (void)
{
    while (monhead) {
        monitor_t *next = monhead->next;
        cairo_destroy(monhead->cr);
        cairo_surface_destroy(monhead->surface);
        xcb_destroy_window(c, monhead->window);
        free(monhead);
        monhead = next;
    }

    xcb_free_colormap(c, colormap);

    if (c)
        xcb_disconnect(c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
        exit(EXIT_SUCCESS);
}

/* Parse an X-styled geometry string, we don't support signed offsets tho. */
bool
parse_geometry_string (char *str, int *tmp)
{
    char *p = str;
    int i = 0, j;

    if (!str || !str[0])
        return false;

    /* The leading = is optional */
    if (*p == '=')
        p++;

    while (*p) {
        /* A geometry string has only 4 fields */
        if (i >= 4) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        /* Move on if we encounter a 'x' or '+' */
        if (*p == 'x') {
            if (i > 0) /* The 'x' must precede '+' */
                break;
            i++; p++; continue;
        }
        if (*p == '+') {
            if (i < 1) /* Stray '+', skip the first two fields */
                i = 2;
            else
                i++;
            p++; continue;
        }
        /* A digit must follow */
        if (!isdigit(*p)) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        /* Try to parse the number */
        errno = 0;
        j = strtoul(p, &p, 10);
        if (errno) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        tmp[i] = j;
    }

    return true;
}

void
parse_font_list (char *str)
{
    char *tok;

    if (!str)
        return;

    tok = strtok(str, ",");
    if (tok) {
        mfont = tok; // font face

        // if a colon is found
        tok = strtok(mfont, ":");
        if(tok) {
            // treat the string before the colon as the font face
            mfont = tok; // font face

            // if characters exist after the colon
            tok = strtok(NULL, ":");
            if(tok) {
                double candidate_size;
                candidate_size = atof(tok);
                // and the size is non-zero
                if(candidate_size != 0) {
                    // treat the rest of the string as the font size
                    mfont_size = candidate_size; // font size
                }
            }
        }
	}

    return;
}

int
main (int argc, char **argv)
{
    struct pollfd pollin[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = -1          , .events = POLLIN },
    };
    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;
    xcb_button_press_event_t *press_ev;
    char input[4096] = {0, };
    bool permanent = false;
    int geom_v[4] = { -1, -1, 0, 0 };

    /* Install the parachute! */
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    /* Connect to the Xserver and initialize scr */
    xconn();

    bgc = scr->black_pixel;
    fgc = scr->white_pixel;

    char ch;
    while ((ch = getopt(argc, argv, "hg:bdf:a:pu:B:F:")) != -1) {
        switch (ch) {
            case 'h':
                printf ("usage: %s [-h | -g | -b | -d | -f | -a | -p | -u | -B | -F]\n"
                        "\t-h Show this help\n"
                        "\t-g Set the bar geometry {width}x{height}+{xoffset}+{yoffset}\n"
                        "\t-b Put bar at the bottom of the screen\n"
                        "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-f Bar font list, comma separated\n"
                        "\t-p Don't close after the data ends\n"
                        "\t-u Set the underline/overline height in pixels\n"
                        "\t-B Set background color in #AARRGGBB\n"
                        "\t-F Set foreground color in #AARRGGBB\n", argv[0]);
                exit (EXIT_SUCCESS);
            case 'g': (void)parse_geometry_string(optarg, geom_v); break;
            case 'p': permanent = true; break;
            case 'b': topbar = false; break;
            case 'd': dock = true; break;
            case 'f': parse_font_list(optarg); break;
            case 'u': bu = strtoul(optarg, NULL, 10); break;
            case 'B': bgc = parse_color(optarg, NULL, scr->black_pixel); break;
            case 'F': fgc = parse_color(optarg, NULL, scr->white_pixel); break;
        }
    }

    convert_color(bgc, &palette[PAL_BG]);
    convert_color(fgc, &palette[PAL_FG]);
    convert_color(fgc, &palette[PAL_ATTR]);

    /* Copy the geometry values in place */
    bw = geom_v[0];
    bh = geom_v[1];
    bx = geom_v[2];
    by = geom_v[3];

    /* Do the heavy lifting */
    init();
    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor(c);

    for (;;) {
        bool redraw = false;

        if (poll(pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      /* No more data... */
                if (permanent) pollin[0].fd = -1;   /* ...null the fd and continue polling :D */
                else break;                         /* ...bail out */
            }
            if (pollin[0].revents & POLLIN) { /* New input, process it */
                if (fgets(input, sizeof(input), stdin) == NULL)
                    break; /* EOF received */

                parse(input);
                redraw = true;
            }
            if (pollin[1].revents & POLLIN) { /* Xserver broadcasted an event */
                while ((ev = xcb_poll_for_event(c))) {
                    expose_ev = (xcb_expose_event_t *)ev;

                    switch (ev->response_type & 0x7F) {
                        case XCB_EXPOSE:
                            if (expose_ev->count == 0) 
                                redraw = true;
                            break;
                        case XCB_BUTTON_PRESS:
                            press_ev = (xcb_button_press_event_t *)ev;
                            {
                                area_t *area = area_get(press_ev->event, press_ev->event_x);
                                /* Respond to the click */
                                if (area && area->button == press_ev->detail) {
                                    write(STDOUT_FILENO, area->cmd, strlen(area->cmd)); 
                                    write(STDOUT_FILENO, "\n", 1); 
                                }
                            }
                            break;
                    }

                    free(ev);
                }
            }
        }

        if (redraw) { /* Copy our temporary pixmap onto the window */
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
                cairo_surface_flush(mon->surface);
            }
        }

        xcb_flush(c);
    }

    return EXIT_SUCCESS;
}
