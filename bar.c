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

#include "config.h"

// Here be dragons

#define MAX(a,b) ((a > b) ? a : b)

typedef struct fontset_item_t {
    xcb_font_t              xcb_ft;
    xcb_query_font_reply_t *info;
    xcb_charinfo_t         *table;
    int                     avg_height;
    unsigned short          char_max;
    unsigned short          char_min;
} fontset_item_t;

enum {
    FONT_MAIN,
    FONT_FALLBACK,
    FONT_MAX
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

static xcb_connection_t *c;
static xcb_window_t     win;
static xcb_drawable_t   canvas;
static xcb_gcontext_t   draw_gc;
static xcb_gcontext_t   clear_gc;
static xcb_gcontext_t   underl_gc;
static int              bar_width;
static int              bar_bottom = BAR_BOTTOM;
static fontset_item_t   fontset[FONT_MAX]; 
static fontset_item_t   *sel_font = NULL;

static const unsigned   palette[] = {COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9};

static inline void
xcb_set_bg (int i)
{
    xcb_change_gc (c, draw_gc  , XCB_GC_BACKGROUND, (const unsigned []){ palette[i] });
    xcb_change_gc (c, clear_gc , XCB_GC_FOREGROUND, (const unsigned []){ palette[i] });
}

static inline void
xcb_set_fg (int i)
{
    xcb_change_gc (c, draw_gc , XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

static inline void
xcb_set_ud (int i)
{
    xcb_change_gc (c, underl_gc, XCB_GC_FOREGROUND, (const uint32_t []){ palette[i] });
}

static inline void
xcb_fill_rect (xcb_gcontext_t gc, int x, int y, int width, int height)
{
    xcb_poly_fill_rectangle (c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

static inline void
xcb_set_fontset (int i)
{
    if (sel_font != &fontset[i]) {
        sel_font = &fontset[i];
        xcb_change_gc (c, draw_gc , XCB_GC_FONT, (const uint32_t []){ sel_font->xcb_ft });
    }
}

int
draw_char (int x, int align, wchar_t ch)
{
    int ch_width;

    ch_width = (ch > sel_font->char_min && ch < sel_font->char_max) ?
        sel_font->table[ch - sel_font->char_min].character_width    :
        0;

    /* Some fonts (such as anorexia) have the space char with the width set to 0 */
    if (ch_width == 0)
        ch_width = BAR_FONT_FALLBACK_WIDTH;

    switch (align) {
        case ALIGN_C:
            xcb_copy_area (c, canvas, canvas, draw_gc, bar_width / 2 - x / 2, 0, 
                    bar_width / 2 - (x + ch_width) / 2, 0, x, BAR_HEIGHT);
            x = bar_width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            xcb_copy_area (c, canvas, canvas, draw_gc, bar_width - x, 0, 
                    bar_width - x - ch_width, 0, x, BAR_HEIGHT);
            x = bar_width - ch_width; 
            break;
    }

    /* Draw the background first */
    xcb_fill_rect (clear_gc, x, 0, ch_width, BAR_HEIGHT);

    /* xcb accepts string in UCS-2 BE, so swap */
    ch = (ch >> 8) | (ch << 8);

    /* String baseline coordinates */
    xcb_image_text_16 (c, 1, canvas, draw_gc, x, BAR_HEIGHT / 2 + sel_font->avg_height / 2 - sel_font->info->font_descent, 
            (xcb_char2b_t *)&ch);

    /* Draw the underline */
    if (BAR_UNDERLINE_HEIGHT) 
        xcb_fill_rect (underl_gc, x, BAR_UNDERLINE*(BAR_HEIGHT-BAR_UNDERLINE_HEIGHT), ch_width, BAR_UNDERLINE_HEIGHT);

    return ch_width;
}

void
parse (char *text)
{
    char *p = text;

    int pos_x = 0;
    int align = 0;

    xcb_fill_rect (clear_gc, 0, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        if (*p == '\0')
            return;
        if (*p == '\n')
            return;

        if (*p == '\\' && p++ && *p != '\\' && strchr ("fbulcr", *p)) {
                switch (*p++) {
                    case 'f': 
                        if (!isdigit (*p)) *p = '1'; 
                        xcb_set_fg ((*p++)-'0');
                        break;
                    case 'b': 
                        if (!isdigit (*p)) *p = '0'; 
                        xcb_set_bg ((*p++)-'0');
                        break;
                    case 'u': 
                        if (!isdigit (*p)) *p = '0'; 
                        xcb_set_ud ((*p++)-'0');
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
            wchar_t t;

            if (!(p[0] & 0x80)) {
                t  = p[0]; 
                p += 1;
            }
            else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
                t  = (p[0] & 0x1f) << 6 | (p[1] & 0x3f);
                p += 2;
            }
            else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
                t  = (p[0] & 0xf) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
                p += 3;
            }
            else { /* ASCII chars > 127 go in the extended latin range */
                t  = 0xc200 + p[0];
                p += 1;
            }

            /* The character is outside the main font charset, use the fallback */
            if (t < fontset[FONT_MAIN].char_min || t > fontset[FONT_MAIN].char_max)
                xcb_set_fontset (FONT_FALLBACK);
            else
                xcb_set_fontset (FONT_MAIN);

            pos_x += draw_char (pos_x, align, t);
        }
    }
}

int
font_load (const char **font_list)
{
    xcb_query_font_cookie_t queryreq;
    xcb_query_font_reply_t *font_info;
    xcb_void_cookie_t cookie;
    xcb_font_t font;
    int max_height;

    max_height = -1;

    for (int i = 0; i < FONT_MAX; i++) {
        font = xcb_generate_id (c);

        cookie = xcb_open_font_checked (c, font, strlen (font_list[i]), font_list[i]);
        if (xcb_request_check (c, cookie)) {
            fprintf (stderr, "Could not load font %s\n", font_list[i]);
            return 1;
        }

        queryreq = xcb_query_font (c, font);
        font_info = xcb_query_font_reply (c, queryreq, NULL);

        fontset[i].xcb_ft  = font;
        fontset[i].table   = xcb_query_font_char_infos (font_info);
        fontset[i].info    = font_info;
        fontset[i].char_max= font_info->max_byte1 << 8 | font_info->max_char_or_byte2;
        fontset[i].char_min= font_info->min_byte1 << 8 | font_info->min_char_or_byte2;

        max_height = MAX(font_info->font_ascent + font_info->font_descent, max_height);
    }

    /* To have an uniform alignment */
    for (int i = 0; i < FONT_MAX; i++)
        fontset[i].avg_height = max_height;

    return 0;
}

int
set_ewmh_atoms (xcb_window_t root)
{
    xcb_intern_atom_cookie_t cookies[5];
    xcb_intern_atom_reply_t *reply;
    xcb_get_property_reply_t *reply1;
    xcb_atom_t atoms[5];
    int compliance_lvl;
    uint32_t v[12] = {0};

    cookies[0] = xcb_intern_atom (c, 0, strlen ("_NET_WM_WINDOW_TYPE")     , "_NET_WM_WINDOW_TYPE");
    cookies[1] = xcb_intern_atom (c, 0, strlen ("_NET_WM_WINDOW_TYPE_DOCK"), "_NET_WM_WINDOW_TYPE_DOCK");
    cookies[2] = xcb_intern_atom (c, 0, strlen ("_NET_WM_DESKTOP")         , "_NET_WM_DESKTOP");
    cookies[3] = xcb_intern_atom (c, 0, strlen ("_NET_WM_STRUT_PARTIAL")   , "_NET_WM_STRUT_PARTIAL");
    cookies[4] = xcb_intern_atom (c, 0, strlen ("_NET_SUPPORTED")          , "_NET_SUPPORTED");

    reply = xcb_intern_atom_reply (c, cookies[0], NULL);
    atoms[0] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[1], NULL);
    atoms[1] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[2], NULL);
    atoms[2] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[3], NULL);
    atoms[3] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[4], NULL);
    atoms[4] = reply->atom; free (reply);

    compliance_lvl = 0;

    reply1 = xcb_get_property_reply (c, xcb_get_property (c, 0, root, atoms[4], XCB_ATOM_ATOM, 0, -1), NULL);
    if (!reply)
        return compliance_lvl;

    for (xcb_atom_t *a = xcb_get_property_value (reply1); 
         a && a != xcb_get_property_value_end (reply1).data;
         a++)
    {
        /* Set the _NET_WM_WINDOW_TYPE_DOCK state */
        if (*a == atoms[0]) {
            xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atoms[0], XCB_ATOM_ATOM, 32, 1, &atoms[1]);
            compliance_lvl++;
        }
        /* Show on every desktop */
        if (*a == atoms[2]) {
            xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atoms[2], XCB_ATOM_CARDINAL, 32, 1, 
                    (const uint32_t []){ 0xffffffff } );
            compliance_lvl++;
        }
        /* Tell the WM that this space is for the bar */
        if (*a == atoms[3]) {
            if (bar_bottom) {
                v[3]  = BAR_HEIGHT;
                v[11] = bar_width;
            }
            else {
                v[2] = BAR_HEIGHT;
                v[8] = bar_width;
            }
            xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atoms[3], XCB_ATOM_CARDINAL, 32, 12, v);
            compliance_lvl++;
        }
    }
    free (reply1);

    /* If the wm supports at least 2 NET atoms then mark as compliant */
    return (compliance_lvl > 1);
}

void
init (void)
{
    xcb_screen_t *scr;
    xcb_window_t root;
    int y;

    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error (c)) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (1);
    }

    /* Grab infos from the first screen */
    scr  = xcb_setup_roots_iterator (xcb_get_setup (c)).data;
    root = scr->root;

    /* where to place the window */
    y = (bar_bottom) ? (scr->height_in_pixels - BAR_HEIGHT) : 0;
    bar_width = (BAR_WIDTH < 0) ? (scr->width_in_pixels - BAR_OFFSET) : BAR_WIDTH;

    /* Load the font */
    if (font_load ((const char* []){ BAR_FONT }))
        exit (1);

    /* Create the main window */
    win = xcb_generate_id (c);
    xcb_create_window (c, XCB_COPY_FROM_PARENT, win, root, BAR_OFFSET, y, bar_width,
            BAR_HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, (const uint32_t []){ palette[0], XCB_EVENT_MASK_EXPOSURE });

    /* Set EWMH hints */
    int ewmh_docking = set_ewmh_atoms (root);

    /* Quirk for wm not supporting the EWMH docking method */
    xcb_change_window_attributes (c, win, XCB_CW_OVERRIDE_REDIRECT, (const uint32_t []){ !ewmh_docking });

    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, root, bar_width, BAR_HEIGHT);

    /* Create the gc for drawing */
    draw_gc = xcb_generate_id (c);
    xcb_create_gc (c, draw_gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (const uint32_t []){ palette[1], palette[0] });

    clear_gc = xcb_generate_id (c);
    xcb_create_gc (c, clear_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[0] });

    underl_gc = xcb_generate_id (c);
    xcb_create_gc (c, underl_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[0] });

    /* Make the bar visible */
    xcb_map_window (c, win);
    /* Send a configure event. Needed to make bar work with Openbox */
    xcb_configure_window (c, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, (const uint32_t []){ BAR_OFFSET, y });

    xcb_flush (c);
}

void
cleanup (void)
{
    int i;
    for (i = 0; i < FONT_MAX; i++) {
        if (fontset[i].info)
            free (fontset[i].info);
        if (fontset[i].xcb_ft)
            xcb_close_font (c, fontset[i].xcb_ft);
    }
    if (canvas)
        xcb_free_pixmap (c, canvas);
    if (win)
        xcb_destroy_window (c, win);
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
        exit (0);
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

    int permanent = 0;

    char ch;
    while ((ch = getopt (argc, argv, "phb")) != -1) {
        switch (ch) {
            case 'h': 
                printf ("usage: %s [-p | -h] [-b]\n"
                        "\t-h Show this help\n"
                        "\t-b Put bar at the bottom of the screen\n"
                        "\t-p Don't close after the data ends\n", argv[0]); 
                exit (0);
            case 'p': permanent = 1; break;
            case 'b': bar_bottom = 1; break;
        }
    }

    atexit (cleanup);
    signal (SIGINT, sighandle);
    signal (SIGTERM, sighandle);
    init ();

    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor (c);

    xcb_fill_rect (clear_gc, 0, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        int redraw = 0;

        if (poll (pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      /* No more data... */
                if (permanent) pollin[0].fd = -1;   /* ...null the fd and continue polling :D */
                else           break;               /* ...bail out */
            }
            if (pollin[0].revents & POLLIN) { /* New input, process it */
                fgets (input, sizeof(input), stdin);
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

        if (redraw) /* Copy our temporary pixmap onto the window */
            xcb_copy_area (c, canvas, win, draw_gc, 0, 0, 0, 0, bar_width, BAR_HEIGHT);

        xcb_flush (c);
    }

    return 0;
}
