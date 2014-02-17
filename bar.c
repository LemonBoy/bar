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

#include "config.h"

// Here be dragons

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

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

static struct config_t {
    int  place_bottom;
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
static xcb_screen_t     *scr;

static xcb_drawable_t   canvas;
static xcb_gcontext_t   draw_gc, clear_gc, underl_gc;

static monitor_t *monhead, *montail;
static font_t *main_font, *alt_font;

static const uint32_t palette[] = {
    COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9,BACKGROUND,FOREGROUND
};

void
set_bg (int i)
{
    xcb_change_gc (c, draw_gc  , XCB_GC_BACKGROUND, (const unsigned []){ palette[i] });
    xcb_change_gc (c, clear_gc , XCB_GC_FOREGROUND, (const unsigned []){ palette[i] });
}

void
set_fg (int i)
{
    xcb_change_gc (c, draw_gc , XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

void
set_ud (int i)
{
    xcb_change_gc (c, underl_gc, XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

void
fill_rect (xcb_gcontext_t gc, int x, int y, int width, int height)
{
    xcb_poly_fill_rectangle (c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

int
draw_char (monitor_t *mon, font_t *cur_font, int x, int align, int underline, uint16_t ch)
{
    /* In the unlikely case that the font doesn't have the glyph wanted just do nothing */
    if (ch < cur_font->char_min || ch > cur_font->char_max)
        return 0;

    int ch_width = cur_font->width_lut[ch - cur_font->char_min].character_width;

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
    fill_rect (clear_gc, x + mon->x, 0, ch_width, cfg.height);

    /* xcb accepts string in UCS-2 BE, so swap */
    ch = (ch >> 8) | (ch << 8);

    /* String baseline coordinates */
    xcb_image_text_16 (c, 1, canvas, draw_gc, x + mon->x, cfg.height / 2 + cur_font->height / 2 - cur_font->descent,
            (xcb_char2b_t *)&ch);

    /* Draw the underline if -1, an overline if 1 */
    if (BAR_UNDERLINE_HEIGHT && underline) 
        fill_rect (underl_gc, x + mon->x, (underline < 0)*(cfg.height-BAR_UNDERLINE_HEIGHT), ch_width, BAR_UNDERLINE_HEIGHT);

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
    cur_mon = monhead;

    fill_rect (clear_gc, 0, 0, cfg.width, cfg.height);

    for (;;) {
        if (*p == '\0' || *p == '\n')
            return;

        if (*p == '^' && p++ && *p != '^' && strchr ("_-fbulcrs", *p)) {
                switch (*p++) {
                    case 'f': 
                        set_fg (isdigit(*p) ? *p-'0' : 11);
                        p++;
                        break;
                    case 'b': 
                        set_bg (isdigit(*p) ? *p-'0' : 10);
                        p++;
                        break;
                    case 'u': 
                        set_ud (isdigit(*p) ? *p-'0' : 10);
                        p++;
                        break;

                    case '_':
                        underline_flag = (underline_flag == -1) ? 0 : -1;
                        break;
                    case '-':
                        underline_flag = (underline_flag ==  1) ? 0 :  1;
                        break;
                    case 's':
                        /* montail only gets set with multiple monitors */
                        if (montail) {
                            if (*p == 'r') {
                                cur_mon = montail;
                            } else if (*p == 'l') {
                                cur_mon = monhead;
                            } else if (*p == 'n') {
                                if (cur_mon->next)
                                    cur_mon = cur_mon->next;
                            } else if (*p == 'p') {
                                if (cur_mon->prev)
                                    cur_mon = cur_mon->prev;
                            } else if (isdigit(*p)) {
                                cur_mon = monhead;
                                /* Start at 0 */
                                for (int i = 0; i != *p-'0' && cur_mon->next; i++)
                                    cur_mon = cur_mon->next;
                            }
                        } else {
                            p++;
                            break;
                        }

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
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
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

    int win_y = (cfg.place_bottom ? (height - cfg.height) : 0) + y;
    ret->window = xcb_generate_id(c);

    xcb_create_window(c, XCB_COPY_FROM_PARENT, ret->window, scr->root,
            x, win_y, width, cfg.height, 0, 
            XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, 
            (const uint32_t []){ palette[10], XCB_EVENT_MASK_EXPOSURE });

    xcb_change_window_attributes (c, ret->window, XCB_CW_OVERRIDE_REDIRECT,
            (const uint32_t []){ cfg.force_docking });

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
    return ((xcb_rectangle_t *)p1)->x - ((xcb_rectangle_t *)p2)->x;
}

void
get_randr_outputs(void)
{
    int i, j, num, cnt = 0;
    xcb_generic_error_t *err;
    xcb_randr_get_screen_resources_current_cookie_t rres_query;
    xcb_randr_get_screen_resources_current_reply_t *rres_reply;
    xcb_randr_output_t *outputs;
    xcb_timestamp_t config_timestamp;

    rres_query = xcb_randr_get_screen_resources_current(c, scr->root);
    rres_reply = xcb_randr_get_screen_resources_current_reply(c, rres_query, &err);
    if (rres_reply == NULL || err != NULL) {
        fprintf(stderr, "Failed to get current randr screen resources\n");
        free(rres_reply);
        return;
    }

    num = xcb_randr_get_screen_resources_current_outputs_length(rres_reply);
    outputs = xcb_randr_get_screen_resources_current_outputs(rres_reply);
    config_timestamp = rres_reply->config_timestamp;
    free(rres_reply);
    if (num < 1) {
        fprintf(stderr, "Failed to get current randr outputs\n");
        return;
    }

    xcb_rectangle_t rects[num];

    /* get all outputs */
    for (i = 0; i < num; i++) {
        xcb_randr_get_output_info_cookie_t output_query;
        xcb_randr_get_output_info_reply_t *output_reply;
        xcb_randr_get_crtc_info_cookie_t crtc_query;
        xcb_randr_get_crtc_info_reply_t *crtc_reply;

        output_query = xcb_randr_get_output_info(c, outputs[i], config_timestamp);
        output_reply = xcb_randr_get_output_info_reply(c, output_query, &err);
        if (err != NULL || output_reply == NULL || output_reply->crtc == XCB_NONE) {
            rects[i].width = 0;
            continue;
        }
        crtc_query = xcb_randr_get_crtc_info(c, output_reply->crtc, config_timestamp);
        crtc_reply = xcb_randr_get_crtc_info_reply(c, crtc_query, &err);
        if (err != NULL | crtc_reply == NULL) {
            fprintf(stderr, "Failed to get randr ctrc info\n");
            rects[i].width = 0;
            free(output_reply);
            continue;
        }
        rects[i].x = crtc_reply->x;
        rects[i].y = crtc_reply->y;
        rects[i].width = crtc_reply->width;
        rects[i].height = crtc_reply->height;
        free(crtc_reply);
        free(output_reply);
        cnt++;
    }

    if (cnt < 1) {
        fprintf(stderr, "No usable randr outputs\n");
        return;
    }

    /* check for clones and inactive outputs */
    for (i = 0; i < num; i++) {
        if (rects[i].width == 0)
            continue;
        for (j = 0; j < num; j++) {
            if (i == j || rects[j].width == 0 || rects[i].x != rects[j].x || rects[i].y != rects[j].y)
                continue;
            /* clone found, only keep one */
            rects[i].width = (rects[i].width < rects[j].width) ? rects[i].width : rects[j].width;
            rects[i].height = (rects[i].height < rects[j].height) ? rects[i].height : rects[j].height;
            rects[j].width = 0;
            cnt--;
        }
    }

    if (cnt < 1) {
        fprintf(stderr, "No usable randr outputs\n");
        return;
    }

    int width = cfg.width;

    for (i = j = 0; i < num && j < cnt; i++) {
        if (rects[i].width) {
            monitor_t *mon = monitor_new (
                    rects[i].x,
                    rects[i].y,
                    MIN(width, rects[i].width),
                    rects[i].height);

            monitor_add (mon);

            width -= rects[i].width;

            /* No need to check for other monitors */
            if (width <= 0)
                break;
            j++;
        }
    }
}

void
get_xinerama_screens (void)
{
    xcb_xinerama_query_screens_reply_t *xqs_reply;
    xcb_xinerama_screen_info_iterator_t iter;
    int width = cfg.width;
    int i, screens;

    xqs_reply = xcb_xinerama_query_screens_reply (c,
            xcb_xinerama_query_screens_unchecked (c), NULL);

    iter = xcb_xinerama_query_screens_screen_info_iterator (xqs_reply);

    xcb_rectangle_t rects[iter.rem];

    /* Fetch all the screens first */
    for (i = 0; iter.rem; i++) {
        rects[i].x = iter.data->x_org;
        rects[i].y = iter.data->y_org;
        rects[i].width = iter.data->width;
        rects[i].height = iter.data->height;
        xcb_xinerama_screen_info_next (&iter);
    }

    screens = i;

    /* Sort by X */
    qsort(rects, i, sizeof(xcb_rectangle_t), rect_sort_cb);

    /* The width is consumed across all the screens */
    for (i = 0; i < screens; i++) {
        monitor_t *mon = monitor_new (
                rects[i].x,
                rects[i].y,
                MIN(width, rects[i].width),
                rects[i].height);

        monitor_add (mon);

        width -= rects[i].width;

        /* No need to check for other monitors */
        if (width <= 0)
            break;
    }

    free (xqs_reply);
}

void
init (void)
{
    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error (c)) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (EXIT_FAILURE);
    }

    /* Grab infos from the first screen */
    scr  = xcb_setup_roots_iterator (xcb_get_setup (c)).data;

    /* If I fits I sits */
    if (cfg.width < 0 || cfg.width > scr->width_in_pixels)
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
    const xcb_query_extension_reply_t *qe_reply;
    int width = cfg.width;

    /* Initialiaze monitor list head and tail */
    monhead = montail = NULL;

    /* Check if RandR is present */
    qe_reply = xcb_get_extension_data (c, &xcb_randr_id);

    if (qe_reply && qe_reply->present) {
        get_randr_outputs ();
    } else {
        qe_reply = xcb_get_extension_data (c, &xcb_xinerama_id);

        /* Check if Xinerama extension is present and active */
        if (qe_reply && qe_reply->present) {
            xcb_xinerama_is_active_reply_t *xia_reply;
            xia_reply = xcb_xinerama_is_active_reply (c, xcb_xinerama_is_active (c), NULL);

            if (xia_reply && xia_reply->state)
                get_xinerama_screens ();

            free (xia_reply);
        }
    }

    if (!monhead)
        /* If no RandR outputs or Xinerama screens, fall back to using whole screen */
        monhead = monitor_new (0, 0, width, scr->height_in_pixels);

    if (!monhead) 
        exit(EXIT_FAILURE);

    /* For WM that support EWMH atoms */
    set_ewmh_atoms();

    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, scr->root, cfg.width, cfg.height);

    /* Create the gc for drawing */
    draw_gc = xcb_generate_id (c);
    xcb_create_gc (c, draw_gc, scr->root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (const uint32_t []){ palette[11], palette[10] });

    clear_gc = xcb_generate_id (c);
    xcb_create_gc (c, clear_gc, scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    underl_gc = xcb_generate_id (c);
    xcb_create_gc (c, underl_gc, scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    /* Make the bar visible */
    for (monitor_t *mon = monhead; mon; mon = mon->next)
        xcb_map_window(c, mon->window);

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

    while (monhead) {
        monitor_t *next = monhead->next;
        free(monhead);
        monhead = next;
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

    fill_rect (clear_gc, 0, 0, cfg.width, cfg.height);

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
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
//                if (mon->width)
                    xcb_copy_area (c, canvas, mon->window, draw_gc, mon->x, 0, 0, 0, mon->width, cfg.height);
            }
        }

        xcb_flush (c);
    }

    return 0;
}
