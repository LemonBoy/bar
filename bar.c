#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xinerama.h>
#include <xcb/randr.h>

// Here be dragons

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define indexof(c,s) (strchr(s,c)-s)

typedef struct font_t {
    xcb_font_t ptr;
    uint32_t descent;
    uint32_t height;
    uint16_t char_max;
    uint16_t char_min;
    xcb_charinfo_t *width_lut;
} font_t;

typedef struct monitor_t {
    uint32_t x;
    uint32_t width;
    xcb_window_t window;
    struct monitor_t *prev, *next;
} monitor_t;

enum {
    ATTR_OVERL = (1<<0),
    ATTR_UNDERL = (1<<1),
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_drawable_t canvas;
static xcb_gcontext_t gc[3];
static monitor_t *monhead, *montail;
static font_t *main_font, *alt_font;
static uint32_t attrs = 0;
static float ba = 1.0f;
static bool dock = false;
static bool topbar = true;
static int bw = -1, bh = -1;
static char *mfont, *afont;
static uint32_t fgc, bgc, ugc;
static uint32_t dfgc, dbgc;

void
update_gc (void)
{
    xcb_change_gc(c, gc[0], XCB_GC_BACKGROUND | XCB_GC_FOREGROUND, (const uint32_t []){ fgc, bgc });
    xcb_change_gc(c, gc[1], XCB_GC_FOREGROUND, (const uint32_t []){ bgc });
    xcb_change_gc(c, gc[2], XCB_GC_FOREGROUND, (const uint32_t []){ ugc });
}

void
fill_rect (xcb_gcontext_t gc, int x, int y, int width, int height)
{
    xcb_poly_fill_rectangle(c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

int
draw_char (monitor_t *mon, font_t *cur_font, int x, int align, uint16_t ch)
{
    /* In the unlikely case that the font doesn't have the glyph wanted just do nothing */
    if (ch < cur_font->char_min || ch > cur_font->char_max)
        return 0;

    int ch_width = cur_font->width_lut[ch - cur_font->char_min].character_width;

    switch (align) {
        case ALIGN_C:
            xcb_copy_area(c, canvas, canvas, gc[0], mon->width / 2 - x / 2 + mon->x, 0,
                    mon->width / 2 - (x + ch_width) / 2 + mon->x, 0, x, bh);
            x = mon->width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            xcb_copy_area(c, canvas, canvas, gc[0], mon->width - x + mon->x, 0,
                    mon->width - x - ch_width + mon->x, 0, x, bh);
            x = mon->width - ch_width;
            break;
    }

    /* Draw the background first */
    fill_rect(gc[1], x + mon->x, 0, ch_width, bh);

    /* xcb accepts string in UCS-2 BE, so swap */
    ch = (ch >> 8) | (ch << 8);

    /* String baseline coordinates */
    xcb_image_text_16(c, 1, canvas, gc[0], x + mon->x, bh / 2 + cur_font->height / 2 - cur_font->descent,
            (xcb_char2b_t *)&ch);

    /* We can render both at the same time */
    if (attrs & ATTR_OVERL)
        fill_rect(gc[2], x + mon->x, 0, ch_width, 1);
    if (attrs & ATTR_UNDERL)
        fill_rect(gc[2], x + mon->x, bh-1, ch_width, 1);

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

    /* Hex rapresentation */
    if (str[0] == '#')
        return strtoul(str + 1, end, 16);

    /* Actual color name, resolve it */
    str_len = 0;
    while (isalpha(str[str_len]))
        str_len++;

    nc_reply = xcb_alloc_named_color_reply(c, xcb_alloc_named_color(c, scr->default_colormap, str_len, str), NULL);

    if (!nc_reply)
        fprintf(stderr, "Could not alloc color \"%.*s\"\n", str_len, str);
    ret = (nc_reply) ? nc_reply->pixel : def;
    free(nc_reply);

    if (end)
        *end = (char *)str + str_len;

    return ret;
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

void
parse (char *text)
{
    font_t *cur_font;
    monitor_t *cur_mon;
    int pos_x;
    int align;
    char *p = text, *end;
    uint32_t tmp;

    pos_x = 0;
    align = ALIGN_L;
    cur_font = main_font;
    cur_mon = monhead;

    fill_rect(gc[1], 0, 0, bw, bh);

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
                              tmp = fgc;
                              fgc = bgc;
                              bgc = tmp;
                              update_gc();
                              break;

                    case 'l': pos_x = 0; align = ALIGN_L; break;
                    case 'c': pos_x = 0; align = ALIGN_C; break;
                    case 'r': pos_x = 0; align = ALIGN_R; break;

                    case 'B': bgc = parse_color(p, &p, dbgc); update_gc(); break;
                    case 'F': fgc = parse_color(p, &p, dfgc); update_gc(); break;
                    case 'U': ugc = parse_color(p, &p, dbgc); update_gc(); break;

                    case 'S':
                              if (*p == '+' && cur_mon->next)
                              { cur_mon = cur_mon->next; pos_x = 0; }
                              if (*p == '-' && cur_mon->prev)
                              { cur_mon = cur_mon->prev; pos_x = 0; }
                              if (*p == 'f')
                              { cur_mon = monhead; pos_x = 0; }
                              if (*p == 'l')
                              { cur_mon = montail ? montail : monhead; pos_x = 0; }
                              if (isdigit(*p))
                              { cur_mon = monhead;
                                for (int i = 0; i != *p-'0' && cur_mon->next; i++)
                                    cur_mon = cur_mon->next;
                              }
                              p++;
                              break;

                    /* In case of error keep parsing after the closing } */
                    default:
                        p = end;
                }
            }
            /* Eat the trailing } */
            p++;
        } else { /* utf-8 -> ucs-2 */
            uint8_t *utf = (uint8_t *)p;
            uint16_t ucs;

            if (utf[0] < 0x80) {
                ucs = utf[0];
                p  += 1;
            }
            else if ((utf[0] & 0xe0) == 0xc0) {
                ucs = (utf[0] & 0x1f) << 6 | (utf[1] & 0x3f);
                p += 2;
            }
            else if ((utf[0] & 0xf0) == 0xe0) {
                ucs = (utf[0] & 0xf) << 12 | (utf[1] & 0x3f) << 6 | (utf[2] & 0x3f);
                p += 3;
            }
            else { /* Handle ascii > 0x80 */
                ucs = utf[0];
                p += 1;
            }

            /* If the character is outside the main font charset use the alternate font */
            cur_font = (ucs < main_font->char_min || ucs > main_font->char_max) ? alt_font : main_font;

            xcb_change_gc(c, gc[0] , XCB_GC_FONT, (const uint32_t []){ cur_font->ptr });

            pos_x += draw_char(cur_mon, cur_font, pos_x, align, ucs);
        }
    }
}

font_t *
font_load (const char *str)
{
    xcb_query_font_cookie_t queryreq;
    xcb_query_font_reply_t *font_info;
    xcb_void_cookie_t cookie;
    xcb_font_t font;

    font = xcb_generate_id(c);

    cookie = xcb_open_font_checked(c, font, strlen(str), str);
    if (xcb_request_check (c, cookie)) {
        fprintf(stderr, "Could not load font %s\n", str);
        return NULL;
    }

    font_t *ret = calloc(1, sizeof(font_t));

    if (!ret)
        return NULL;

    queryreq = xcb_query_font(c, font);
    font_info = xcb_query_font_reply(c, queryreq, NULL);

    ret->ptr = font;
    ret->descent = font_info->font_descent;
    ret->height = font_info->font_ascent + font_info->font_descent;
    ret->char_max = font_info->max_byte1 << 8 | font_info->max_char_or_byte2;
    ret->char_min = font_info->min_byte1 << 8 | font_info->min_char_or_byte2;
    ret->width_lut = xcb_query_font_char_infos(font_info);

    return ret;
}

enum {
    NET_WM_WINDOW_TYPE,
    NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_DESKTOP,
    NET_WM_STRUT_PARTIAL,
    NET_WM_STRUT,
    NET_WM_STATE,
    NET_WM_WINDOW_OPACITY,
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
        "_NET_WM_WINDOW_OPACITY",
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

        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_WINDOW_OPACITY], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ (uint32_t)(ba * 0xffffffff) } );
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

    ret = malloc(sizeof(monitor_t));
    if (!ret) {
        fprintf(stderr, "Failed to allocate new monitor\n");
        exit(EXIT_FAILURE);
    }

    ret->x = x;
    ret->width = width;
    ret->next = ret->prev = NULL;

    int win_y = (topbar ? 0 : height - bh) + y;
    ret->window = xcb_generate_id(c);

    xcb_create_window(c, XCB_COPY_FROM_PARENT, ret->window, scr->root,
            x, win_y, width, bh, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
            (const uint32_t []){ bgc, XCB_EVENT_MASK_EXPOSURE });

    xcb_change_window_attributes(c, ret->window, XCB_CW_OVERRIDE_REDIRECT,
            (const uint32_t []){ dock });

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
get_randr_monitors (void)
{
    xcb_generic_error_t *err;
    xcb_randr_get_screen_resources_current_reply_t *rres_reply;
    xcb_randr_output_t *outputs;
    int num, valid = 0;
    int width = bw;

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
    for (int i = 0; i < num; i++) {
        xcb_randr_get_output_info_reply_t *oi_reply;
        xcb_randr_get_crtc_info_reply_t *ci_reply;

        oi_reply = xcb_randr_get_output_info_reply(c, xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME), NULL);

        /* Output disconnected or not attached to any CRTC ? */
        if (!oi_reply || oi_reply->crtc == XCB_NONE) {
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
    for (int i = 0; i < num; i++) {
        if (rects[i].width == 0)
            continue;

        for (int j = 0; j < num; j++) {
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

    /* Sort before use */
    qsort(rects, num, sizeof(xcb_rectangle_t), rect_sort_cb);

    for (int i = 0; i < num; i++) {
        if (rects[i].width) {
            monitor_t *mon = monitor_new(
                    rects[i].x,
                    rects[i].y,
                    min(width, rects[i].width),
                    rects[i].height);

            monitor_add(mon);

            width -= rects[i].width;

            /* No need to check for other monitors */
            if (width <= 0)
                break;
        }
    }
}

void
get_xinerama_monitors (void)
{
    xcb_xinerama_query_screens_reply_t *xqs_reply;
    xcb_xinerama_screen_info_iterator_t iter;
    int screens, width = bw;

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

    /* Sort before use */
    qsort(rects, screens, sizeof(xcb_rectangle_t), rect_sort_cb);

    /* The width is consumed across all the screens */
    for (int i = 0; i < screens; i++) {
        monitor_t *mon = monitor_new(
                rects[i].x,
                rects[i].y,
                min(width, rects[i].width),
                rects[i].height);

        monitor_add(mon);

        width -= rects[i].width;

        /* No need to check for other monitors */
        if (width <= 0)
            break;
    }

    free(xqs_reply);
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
}

void
init (void)
{
    /* If I fits I sits */
    if (bw < 0 || bw > scr->width_in_pixels)
        bw = scr->width_in_pixels;

    /* Load the fonts */
    main_font = font_load(mfont ? mfont : "fixed");
    if (!main_font)
        exit(EXIT_FAILURE);

    alt_font = font_load(afont ? afont : "fixed");
    if (!alt_font)
        exit(EXIT_FAILURE);

    /* To make the alignment uniform */
    main_font->height = alt_font->height = max(main_font->height, alt_font->height);

    /* Adjust the height */
    if (bh < 0 || bh > scr->height_in_pixels)
        bh = main_font->height + 1;

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

    if (!monhead)
        /* If no RandR outputs or Xinerama screens, fall back to using whole screen */
        monhead = monitor_new(0, 0, bw, scr->height_in_pixels);

    if (!monhead)
        exit(EXIT_FAILURE);

    /* For WM that support EWMH atoms */
    set_ewmh_atoms();

    /* Create a temporary canvas */
    canvas = xcb_generate_id(c);
    xcb_create_pixmap(c, scr->root_depth, canvas, scr->root, bw, bh);

    /* Default to the classic B/W combo */
    ugc = fgc;

    /* Create the gc for drawing */
    gc[0] = xcb_generate_id(c);
    xcb_create_gc(c, gc[0], scr->root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (const uint32_t []){ fgc, bgc });

    gc[1] = xcb_generate_id(c);
    xcb_create_gc(c, gc[1], scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ bgc });

    gc[2] = xcb_generate_id(c);
    xcb_create_gc(c, gc[2], scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ bgc });

    /* Make the bar visible */
    for (monitor_t *mon = monhead; mon; mon = mon->next)
        xcb_map_window(c, mon->window);

    xcb_flush(c);
}

void
cleanup (void)
{
    if (main_font) {
        xcb_close_font (c, main_font->ptr);
        free(main_font);
    }

    if (alt_font) {
        xcb_close_font (c, alt_font->ptr);
        free(alt_font);
    }

    while (monhead) {
        monitor_t *next = monhead->next;
        free(monhead);
        monhead = next;
    }

    if (canvas)
        xcb_free_pixmap(c, canvas);
    if (gc[0])
        xcb_free_gc(c, gc[0]);
    if (gc[1])
        xcb_free_gc(c, gc[1]);
    if (gc[2])
        xcb_free_gc(c, gc[2]);
    if (c)
        xcb_disconnect(c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
        exit(EXIT_SUCCESS);
}

/* Parse an urxvt-like geometry string {width}x{height}, both the fields are
 * optional. A width of -1 means that the bar spawns the whole screen.  */
void
parse_geometry_string (char *str)
{
    char *p, *q;
    int tmp;

    if (!str)
        return;

    p = str;

    tmp = strtoul(p, &q, 10);
    if (p != q)
        bw = tmp;

    /* P now might point to a NULL char, strtoul takes care of that */
    p = q + 1;

    tmp = strtoul(p, &q, 10);
    if (p != q)
        bh = tmp;
}

void
parse_font_list (char *str)
{
    char *tok;

    if (!str)
        return;

    tok = strtok(str, ",");
    if (tok)
        mfont = tok;
    tok = strtok(NULL, ",");
    if (tok)
        afont = tok;

    return;
}

int
main (int argc, char **argv)
{
    char input[2048] = {0, };
    struct pollfd pollin[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = -1          , .events = POLLIN },
    };
    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;
    bool permanent = false;

    /* Install the parachute! */
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    /* Connect to the Xserver and initialize scr */
    xconn();

    /* B/W combo */
    dbgc = bgc = scr->black_pixel;
    dfgc = fgc = scr->white_pixel;

    char ch;
    while ((ch = getopt(argc, argv, "hg:bdf:a:pB:F:")) != -1) {
        switch (ch) {
            case 'h':
                printf ("usage: %s [-h | -g | -b | -d | -f | -a | -p | -B | -F]\n"
                        "\t-h Show this help\n"
                        "\t-g Set the bar geometry {width}x{height})\n"
                        "\t-b Put bar at the bottom of the screen\n"
                        "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-f Bar font list, comma separated\n"
                        "\t-a Set the bar alpha ranging from 0.0 to 1.0 (requires a compositor)\n"
                        "\t-p Don't close after the data ends\n"
                        "\t-B Set background color in #RRGGBB\n"
                        "\t-F Set foreground color in #RRGGBB\n", argv[0]);
                exit (EXIT_SUCCESS);
            case 'a': ba = strtof(optarg, NULL); break;
            case 'g': parse_geometry_string(optarg); break;
            case 'p': permanent = true; break;
            case 'b': topbar = false; break;
            case 'd': dock = true; break;
            case 'f': parse_font_list(optarg); break;
            case 'B': dbgc = bgc = parse_color(optarg, NULL, scr->black_pixel); break;
            case 'F': dfgc = fgc = parse_color(optarg, NULL, scr->white_pixel); break;
        }
    }

    /* Sanitize the arguments */
    if (ba > 1.0f)
        ba = 1.0f;
    if (ba < 0.0f)
        ba = 0.0f;

    /* Do the heavy lifting */
    init();
    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor(c);
    /* Clear the bar */
    fill_rect(gc[1], 0, 0, bw, bh);

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
                            if (expose_ev->count == 0) redraw = true;
                        break;
                    }

                    free(ev);
                }
            }
        }

        if (redraw) { /* Copy our temporary pixmap onto the window */
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
                xcb_copy_area(c, canvas, mon->window, gc[0], mon->x, 0, 0, 0, mon->width, bh);
            }
        }

        xcb_flush(c);
    }

    return 0;
}
