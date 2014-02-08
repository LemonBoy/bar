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

#include "config.h"

// Here be dragons

#define MAX(a,b) ((a > b) ? a : b)
#define MIN(a,b) ((a < b) ? a : b)

typedef struct font_t {
    xcb_font_t      ptr;
    uint32_t        descent;
    uint32_t        height;
    uint16_t        char_max;
    uint16_t        char_min;
    xcb_charinfo_t *width_lut;
} font_t;

typedef struct monitor_t {
    uint32_t     x;
    uint32_t     width;
    xcb_window_t window;
    struct monitor_t *prev, *next;
} monitor_t;

struct config_t {
    int place_bottom;
    int force_docking;
    int permanent;
    int width;
    int height;
    float alpha;
    char main_font[64];
    char alt_font[64];
} cfg = {
    0, 0, 0, -1, 18, 1.0f, "fixed", "fixed",
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

static xcb_connection_t *c;

static xcb_drawable_t   canvas;
static xcb_gcontext_t   draw_gc;
static xcb_gcontext_t   clear_gc;
static xcb_gcontext_t   underl_gc;

static monitor_t *mons;
static font_t *main_font, *alt_font;

static const uint32_t palette[] = {
    COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9,BACKGROUND,FOREGROUND
};

static const char *control_characters = "_-fbulcrs";

void
xcb_set_bg (int i)
{
    xcb_change_gc (c, draw_gc  , XCB_GC_BACKGROUND, (const unsigned []){ palette[i] });
    xcb_change_gc (c, clear_gc , XCB_GC_FOREGROUND, (const unsigned []){ palette[i] });
}

void
xcb_set_fg (int i)
{
    xcb_change_gc (c, draw_gc , XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

void
xcb_set_ud (int i)
{
    xcb_change_gc (c, underl_gc, XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

void
xcb_fill_rect (xcb_gcontext_t gc, int x, int y, int width, int height)
{
    xcb_poly_fill_rectangle (c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

int
draw_char (monitor_t *mon, font_t *cur_font, int x, int align, int underline, uint16_t ch)
{
    int ch_width;

    ch_width = (ch > cur_font->char_min && ch < cur_font->char_max) ?
        cur_font->width_lut[ch - cur_font->char_min].character_width :
        0;

    /* Some fonts (such as anorexia) have the space char with the width set to 0 */
    if (ch_width == 0)
        ch_width = BAR_FONT_FALLBACK_WIDTH;

    switch (align) {
        case ALIGN_C:
            xcb_copy_area (c, canvas, canvas, draw_gc, mon->width / 2 - x / 2 + mon->x, 0,
                    mon->width / 2 - (x + ch_width) / 2 + mon->x, 0, x, cfg.height);
            x = mon->width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            xcb_copy_area (c, canvas, canvas, draw_gc, mon->width - x + mon->x, 0,
                    mon->width - x - ch_width + mon->x, 0, x, cfg.height);
            x = mon->width - ch_width;
            break;
    }

    /* Draw the background first */
    xcb_fill_rect (clear_gc, x + mon->x, 0, ch_width, cfg.height);

    /* xcb accepts string in UCS-2 BE, so swap */
    ch = (ch >> 8) | (ch << 8);

    /* String baseline coordinates */
    xcb_image_text_16 (c, 1, canvas, draw_gc, x + mon->x, cfg.height / 2 + cur_font->height / 2 - cur_font->descent,
            (xcb_char2b_t *)&ch);

    /* Draw the underline if -1, an overline if 1 */
    if (BAR_UNDERLINE_HEIGHT && underline) 
        xcb_fill_rect (underl_gc, x + mon->x, (underline < 0)*(cfg.height-BAR_UNDERLINE_HEIGHT), ch_width, BAR_UNDERLINE_HEIGHT);

    return ch_width;
}

void
parse (char *text)
{
    char *p = text;

    int pos_x = 0;
    int align = 0;
    int underline_flag = 0;

    font_t *cur_font;
    monitor_t *cur_mon;

    cur_font = main_font;
    cur_mon = mons;

    xcb_fill_rect (clear_gc, 0, 0, cfg.width, cfg.height);

    for (;;) {
        if (*p == '\0' || *p == '\n')
            return;

        if (*p == '^' && p++ && *p != '^' && strchr (control_characters, *p)) {
                switch (*p++) {
                    case 'f': 
                        xcb_set_fg (isdigit(*p) ? *p-'0' : 11);
                        p++;
                        break;
                    case 'b': 
                        xcb_set_bg (isdigit(*p) ? *p-'0' : 10);
                        p++;
                        break;
                    case 'u': 
                        xcb_set_ud (isdigit(*p) ? *p-'0' : 10);
                        p++;
                        break;

                    case '_':
                        underline_flag = (underline_flag == -1) ? 0 : -1;
                        break;
                    case '-':
                        underline_flag = (underline_flag ==  1) ? 0 :  1;
                        break;
                    case 's':
                        if (*p == 'r') 
                            cur_mon = mons;
                        else if (*p == 'l') 
                            { while (cur_mon->next) cur_mon = cur_mon->next; }
                        else if (*p == 'n') 
                            { if (cur_mon->next) cur_mon = cur_mon->next; }
                        else if (*p == 'p') 
                            { if (cur_mon->prev) cur_mon = cur_mon->prev; }
                        else if (isdigit(*p)) {
                            cur_mon = mons;
                            /* Start at 1 */
                            for (int i = 1; i <= *p-'0' && cur_mon->next; i++)
                                cur_mon = cur_mon->next;
                        }
                        else
                            break;

                        /* Consume the argument */
                        p++;
                        
                        align = ALIGN_L;
                        pos_x = 0;
                        break;
                    case 'l': 
                        align = ALIGN_L; 
                        pos_x = 0; 
                        break;
                    case 'c': 
                        align = ALIGN_C; 
                        pos_x = 0; 
                        break;
                    case 'r': 
                        align = ALIGN_R; 
                        pos_x = 0; 
                        break;
                }
        } else { /* utf-8 -> ucs-2 */
            if (!cur_mon->window)
                continue;

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

            xcb_change_gc (c, draw_gc , XCB_GC_FONT, (const uint32_t []){ cur_font->ptr });

            pos_x += draw_char (cur_mon, cur_font, pos_x, align, underline_flag, ucs);
        }
    }
}

font_t *
font_load (char *font_string)
{
    xcb_query_font_cookie_t queryreq;
    xcb_query_font_reply_t *font_info;
    xcb_void_cookie_t cookie;
    xcb_font_t font;

    font = xcb_generate_id (c);

    cookie = xcb_open_font_checked (c, font, strlen (font_string), font_string);
    if (xcb_request_check (c, cookie)) {
        fprintf (stderr, "Could not load font %s\n", font_string);
        return NULL;
    }

    font_t *ret = calloc(1, sizeof(font_t));

    if (!ret)
        return NULL;

    queryreq = xcb_query_font (c, font);
    font_info = xcb_query_font_reply (c, queryreq, NULL);

    ret->ptr = font;
    ret->width_lut = xcb_query_font_char_infos (font_info);
    ret->descent = font_info->font_descent;
    ret->char_max = font_info->max_byte1 << 8 | font_info->max_char_or_byte2;
    ret->char_min = font_info->min_byte1 << 8 | font_info->min_char_or_byte2;
    ret->height = font_info->font_ascent + font_info->font_descent;

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
    for (monitor_t *mon = mons; mon; mon = mon->next) {
        int strut[12] = {0};
        if (cfg.place_bottom) {
            strut[3]  = cfg.height;
            strut[10] = mon->x;
            strut[11] = mon->x + mon->width;
        } else {
            strut[2] = cfg.height;
            strut[8] = mon->x;
            strut[9] = mon->x + mon->width;
        }

        xcb_change_property (c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_WINDOW_OPACITY], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ (uint32_t)(cfg.alpha * 0xffffffff) } );
        xcb_change_property (c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
        xcb_change_property (c, XCB_PROP_MODE_APPEND,  mon->window, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
        xcb_change_property (c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ -1 } );
        xcb_change_property (c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
        xcb_change_property (c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
    }
}

monitor_t *
monitor_new (int x, int y, int width, int height, xcb_window_t root, xcb_visualid_t visual)
{
    monitor_t *ret;

    ret = calloc(1, sizeof(monitor_t));
    if (!ret)
        return NULL;

    ret->x = x;
    ret->width = width;

    if (width) {
        int win_y = (cfg.place_bottom ? (height - cfg.height) : 0) + y;
        ret->window = xcb_generate_id(c);

        xcb_create_window(c, XCB_COPY_FROM_PARENT, ret->window, root, x, win_y, width, cfg.height, 0, 
                XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, 
                (const uint32_t []){ palette[10], XCB_EVENT_MASK_EXPOSURE });

        xcb_change_window_attributes (c, ret->window, XCB_CW_OVERRIDE_REDIRECT, (const uint32_t []){ cfg.force_docking });

        xcb_map_window(c, ret->window);
    }

    return ret;
}

void
init (void)
{
    xcb_screen_t *scr;
    xcb_window_t root;

    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error (c)) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (EXIT_FAILURE);
    }

    /* Grab infos from the first screen */
    scr  = xcb_setup_roots_iterator (xcb_get_setup (c)).data;
    root = scr->root;

    /* If I fits I sits */
    if (cfg.width < 0)
        cfg.width = scr->width_in_pixels;

    /* Load the fonts */
    main_font = font_load((char *)cfg.main_font);
    if (!main_font)
        exit(EXIT_FAILURE);

    alt_font = font_load((char *)cfg.alt_font);
    if (!alt_font)
        exit(EXIT_FAILURE);

    /* To make the alignment uniform */
    main_font->height = alt_font->height = MAX(main_font->height, alt_font->height);

    /* Generate a list of screens */
    xcb_xinerama_is_active_reply_t *xia_reply;

    xia_reply = xcb_xinerama_is_active_reply (c, xcb_xinerama_is_active (c), NULL);

    if (xia_reply && xia_reply->state) {
        xcb_xinerama_query_screens_reply_t *xqs_reply;
        xcb_xinerama_screen_info_iterator_t iter;

        xqs_reply = xcb_xinerama_query_screens_reply (c, xcb_xinerama_query_screens_unchecked (c), NULL);

        iter = xcb_xinerama_query_screens_screen_info_iterator (xqs_reply);

        /* The width is consumed across all the screens */
        int width_to_consume = (cfg.width == scr->width_in_pixels) ? 0 : cfg.width;

        while (iter.rem) {
            monitor_t *mon = monitor_new (
                    iter.data->x_org + ((iter.data->width > width_to_consume) ? width_to_consume : 0),
                    iter.data->y_org + cfg.place_bottom ? (iter.data->height - cfg.height) : 0,
                    iter.data->width - MIN(iter.data->width, width_to_consume),
                    iter.data->height,
                    root, scr->root_visual);

            mon->prev = mons;

            if (!mons)
                mons = mon;
            else
                mons->next = mon;

            if (width_to_consume)
                width_to_consume -= MIN(iter.data->width, width_to_consume);

            xcb_xinerama_screen_info_next (&iter);
        }
    }
    else {
        mons = monitor_new (
                0, 
                cfg.place_bottom ? (scr->height_in_pixels - cfg.height) : 0,
                cfg.width,
                scr->height_in_pixels,
                root, scr->root_visual);
    }

    if (!mons) 
        exit(EXIT_FAILURE);

    /* For WM that support EWMH atoms */
    set_ewmh_atoms();

    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, root, cfg.width, cfg.height);

    /* Create the gc for drawing */
    draw_gc = xcb_generate_id (c);
    xcb_create_gc (c, draw_gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (const uint32_t []){ palette[11], palette[10] });

    clear_gc = xcb_generate_id (c);
    xcb_create_gc (c, clear_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    underl_gc = xcb_generate_id (c);
    xcb_create_gc (c, underl_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    xcb_flush (c);
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

    while (mons) {
        monitor_t *next = mons->next;
        free(mons);
        mons = next;
    }

    if (canvas)
        xcb_free_pixmap (c, canvas);
    if (draw_gc)
        xcb_free_gc (c, draw_gc);
    if (clear_gc)
        xcb_free_gc (c, clear_gc);
    if (underl_gc)
        xcb_free_gc (c, underl_gc);
    if (c)
        xcb_disconnect (c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM) 
        exit (EXIT_SUCCESS);
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
        cfg.width = tmp;

    /* P now might point to a NULL char, strtoul takes care of that */
    p = q + 1;

    tmp = strtoul(p, &q, 10);
    if (p != q)
        cfg.height = tmp;
}

void
parse_font_list (char *str)
{
    char *tok;

    if (!str)
        return;

    tok = strtok(str, ",");
    if (tok) 
        strncpy(cfg.main_font, tok, sizeof(cfg.main_font));
    tok = strtok(NULL, ",");
    if (tok) 
        strncpy(cfg.alt_font, tok, sizeof(cfg.alt_font));

    return;
}

int 
main (int argc, char **argv)
{
    char input[1024] = {0, };
    struct pollfd pollin[2] = { 
        { .fd = STDIN_FILENO, .events = POLLIN }, 
        { .fd = -1          , .events = POLLIN }, 
    };

    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;

    char ch;
    while ((ch = getopt (argc, argv, "hg:bdf:a:o:p")) != -1) {
        switch (ch) {
            case 'h': 
                printf ("usage: %s [-h | -g | -b | -d | -f | -a | -o | -p ]\n"
                        "\t-h Show this help\n"
                        "\t-g Set the bar geometry {width}x{height})\n"
                        "\t-b Put bar at the bottom of the screen\n"
                        "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-f Bar font list, comma separated\n"
                        "\t-a Set the bar alpha ranging from 0.0 to 1.0 (requires a compositor)\n"
                        "\t-p Don't close after the data ends\n", argv[0]); 
                exit (EXIT_SUCCESS);
            case 'a': cfg.alpha = strtof(optarg, NULL); break;
            case 'g': parse_geometry_string(optarg); break;
            case 'p': cfg.permanent = 1; break;
            case 'b': cfg.place_bottom = 1; break;
            case 'd': cfg.force_docking = 1; break;
            case 'f': parse_font_list(optarg); break;
        }
    }

    /* Sanitize the arguments */
    if (cfg.alpha > 1.0f)
        cfg.alpha = 1.0f;
    if (cfg.alpha < 0.0f)
        cfg.alpha = 0.0f;

    atexit (cleanup);
    signal (SIGINT, sighandle);
    signal (SIGTERM, sighandle);
    init ();

    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor (c);

    xcb_fill_rect (clear_gc, 0, 0, cfg.width, cfg.height);

    for (;;) {
        int redraw = 0;

        if (poll (pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {          /* No more data... */
                if (cfg.permanent) pollin[0].fd = -1;   /* ...null the fd and continue polling :D */
                else break;                             /* ...bail out */
            }
            if (pollin[0].revents & POLLIN) { /* New input, process it */
                if (fgets (input, sizeof(input), stdin) == NULL)
                    break; /* EOF received */

                parse (input);
                redraw = 1;
            }
            if (pollin[1].revents & POLLIN) { /* Xserver broadcasted an event */
                while ((ev = xcb_poll_for_event (c))) {
                    expose_ev = (xcb_expose_event_t *)ev;

                    switch (ev->response_type & 0x7F) {
                        case XCB_EXPOSE: 
                            if (expose_ev->count == 0) redraw = 1; 
                        break;
                    }

                    free (ev);
                }
            }
        }

        if (redraw) { /* Copy our temporary pixmap onto the window */
            for (monitor_t *mon = mons; mon; mon = mon->next) {
                if (mon->window)
                    xcb_copy_area (c, canvas, mon->window, draw_gc, mon->x, 0, 0, 0, mon->width, cfg.height);
            }
        }

        xcb_flush (c);
    }

    return 0;
}
