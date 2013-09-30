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
#if XINERAMA
#include <xcb/xinerama.h>
#endif

#include "config.h"

// Here be dragons

#define MAX(a,b) ((a > b) ? a : b)

typedef struct fontset_item_t {
    xcb_font_t      xcb_ft;
    xcb_charinfo_t *table;
    int             descent;
    int             avg_height;
    unsigned short  char_max;
    unsigned short  char_min;
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

typedef struct screen_t {
    int x;
    int width;
} screen_t;

static xcb_connection_t *c;
static xcb_window_t     win;
static xcb_drawable_t   canvas;
static xcb_gcontext_t   draw_gc;
static xcb_gcontext_t   clear_gc;
static xcb_gcontext_t   underl_gc;
static int              bar_width;
static int              bar_bottom = BAR_BOTTOM;
static int              force_docking = 0;
static fontset_item_t   fontset[FONT_MAX]; 
static fontset_item_t   *sel_font = NULL;

static screen_t         *screens;
static int              num_screens;
static const unsigned   palette[] = {COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9,BACKGROUND,FOREGROUND};

#if XINERAMA
static const char *control_characters = "fbulcsr";
#else
static const char *control_characters = "fbulcr";
#endif

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
draw_char (screen_t *screen, int x, int align, wchar_t ch)
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
            xcb_copy_area (c, canvas, canvas, draw_gc, screen->width / 2 - x / 2 + screen->x, 0,
                    screen->width / 2 - (x + ch_width) / 2 + screen->x, 0, x, BAR_HEIGHT);
            x = screen->width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            xcb_copy_area (c, canvas, canvas, draw_gc, screen->width - x + screen->x, 0,
                    screen->width - x - ch_width + screen->x, 0, x, BAR_HEIGHT);
            x = screen->width - ch_width;
            break;
    }

    /* Draw the background first */
    xcb_fill_rect (clear_gc, x + screen->x, 0, ch_width, BAR_HEIGHT);

    /* xcb accepts string in UCS-2 BE, so swap */
    ch = (ch >> 8) | (ch << 8);

    /* String baseline coordinates */
    xcb_image_text_16 (c, 1, canvas, draw_gc, x + screen->x, BAR_HEIGHT / 2 + sel_font->avg_height / 2 - sel_font->descent,
            (xcb_char2b_t *)&ch);

    /* Draw the underline */
    if (BAR_UNDERLINE_HEIGHT) 
        xcb_fill_rect (underl_gc, x + screen->x, BAR_UNDERLINE*(BAR_HEIGHT-BAR_UNDERLINE_HEIGHT), ch_width, BAR_UNDERLINE_HEIGHT);

    return ch_width;
}

void
parse (char *text)
{
    char *p = text;

    int pos_x = 0;
    int align = 0;
    screen_t *screen = &screens[0];

    xcb_fill_rect (clear_gc, 0, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        if (*p == '\0')
            return;
        if (*p == '\n')
            return;

        if (*p == '\\' && p++ && *p != '\\' && strchr (control_characters, *p)) {
                switch (*p++) {
                    case 'f': 
                        xcb_set_fg (isdigit(*p) ? (*p)-'0' : 11);
                        p++;
                        break;
                    case 'b': 
                        xcb_set_bg (isdigit(*p) ? (*p)-'0' : 10);
                        p++;
                        break;
                    case 'u': 
                        xcb_set_ud (isdigit(*p) ? (*p)-'0' : 10);
                        p++;
                        break;
#if XINERAMA
                    case 's':
                        if ((*p) == 'r') {
                            screen = &screens[num_screens - 1];
                        } else if ((*p) == 'l') {
                            screen = &screens[0];
                        } else if ((*p) == 'n') {
                            if (screen == &screens[num_screens - 1])
                                break;
                            screen++;
                        } else if ((*p) == 'p') {
                            if (screen == screens)
                                break;
                            screen--;
                        } else if (isdigit(*p)) {
                            int index = (*p)-'0';
                            if (index < num_screens) {
                                screen = &screens[index];
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                        align = ALIGN_L;
                        pos_x = 0;
                        p++;
                        break;
#endif
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

            pos_x += draw_char (screen, pos_x, align, t);
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
        fontset[i].descent = font_info->font_descent;
        fontset[i].char_max= font_info->max_byte1 << 8 | font_info->max_char_or_byte2;
        fontset[i].char_min= font_info->min_byte1 << 8 | font_info->min_char_or_byte2;

        max_height = MAX(font_info->font_ascent + font_info->font_descent, max_height);
    }

    /* To have an uniform alignment */
    for (int i = 0; i < FONT_MAX; i++)
        fontset[i].avg_height = max_height;

    return 0;
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
set_ewmh_atoms ()
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
    int strut[12] = {0};

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
    if (bar_bottom) {
        strut[3]  = BAR_HEIGHT;
        strut[11] = bar_width;
    } else {
        strut[2] = BAR_HEIGHT;
        strut[9] = bar_width;
    }

    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_WINDOW_OPACITY], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ (uint32_t)(BAR_OPACITY * 0xffffffff) } );
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
    xcb_change_property (c, XCB_PROP_MODE_APPEND,  win, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ -1 } );
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
}

void
init (void)
{
    xcb_screen_t *scr;
    xcb_window_t root;
    int y;
    int right_bar_offset;

#if XINERAMA
    xcb_generic_error_t* err = NULL;
    xcb_xinerama_query_screens_reply_t *xinerama_reply;
    xcb_xinerama_query_screens_cookie_t xinerama_query;
    xcb_xinerama_screen_info_iterator_t xinerama_iter;
    screen_t *cur_screen;
#endif

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

    /* Generate a list of screens */
#if XINERAMA
    xinerama_query = xcb_xinerama_query_screens_unchecked (c);
    xinerama_reply = xcb_xinerama_query_screens_reply (c, xinerama_query, &err);
    if (err != NULL || xinerama_reply == NULL)
        exit (1);

    xinerama_iter = xcb_xinerama_query_screens_screen_info_iterator (xinerama_reply);

    num_screens = xinerama_iter.rem;
    screens = calloc (num_screens, sizeof(screen_t));

    if (screens == NULL)
        exit (1);

    /* Add BAR_OFFSET to the last screen */
    right_bar_offset = scr->width_in_pixels - bar_width - BAR_OFFSET;
    for (cur_screen = &screens[num_screens-1]; cur_screen >= screens; xcb_xinerama_screen_info_next (&xinerama_iter), cur_screen--) {
        cur_screen->width = xinerama_iter.data->width;
        if (right_bar_offset > 0) {
            if (right_bar_offset >= cur_screen->width) {
                /* Remove the screen */
                num_screens--;
                right_bar_offset -= cur_screen->width;
            } else {
                cur_screen->width -= right_bar_offset;
                right_bar_offset = 0;
            }
        }

        cur_screen->x = xinerama_iter.data->x_org - BAR_OFFSET;
        if (cur_screen->x < 0) {
            /* First screen */
            cur_screen->x = 0;
            break;
        }
    }
    free(xinerama_reply);

    /* Remove BAR_OFFSET from the first screen */
    cur_screen->width -= BAR_OFFSET;

    /* Shift */
    if (cur_screen > screens) {
        memmove (screens, cur_screen, sizeof(screen_t) * num_screens);
    }

    /* Reallocate */
    screens = realloc (screens, num_screens);
    if (screens == NULL)
        exit (1);
#else
    num_screens = 1;
    screens = calloc(1, sizeof(screen_t));
    screens[0].x = 0;
    screens[0].width = bar_width;
#endif

    /* Create the main window */
    win = xcb_generate_id (c);
    xcb_create_window (c, XCB_COPY_FROM_PARENT, win, root, BAR_OFFSET, y, bar_width,
            BAR_HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, (const uint32_t []){ palette[10], XCB_EVENT_MASK_EXPOSURE });

    /* For WM that support EWMH atoms */
    set_ewmh_atoms();

    xcb_change_window_attributes (c, win, XCB_CW_OVERRIDE_REDIRECT, (const uint32_t []){ force_docking });

    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, root, bar_width, BAR_HEIGHT);

    /* Create the gc for drawing */
    draw_gc = xcb_generate_id (c);
    xcb_create_gc (c, draw_gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (const uint32_t []){ palette[11], palette[10] });

    clear_gc = xcb_generate_id (c);
    xcb_create_gc (c, clear_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    underl_gc = xcb_generate_id (c);
    xcb_create_gc (c, underl_gc, root, XCB_GC_FOREGROUND, (const uint32_t []){ palette[10] });

    /* Make the bar visible */
    xcb_map_window (c, win);

    xcb_flush (c);
}

void
cleanup (void)
{
    int i;
    for (i = 0; i < FONT_MAX; i++) {
        if (fontset[i].xcb_ft)
            xcb_close_font (c, fontset[i].xcb_ft);
    }
    if (screens)
        free (screens);
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
    while ((ch = getopt (argc, argv, "phbf")) != -1) {
        switch (ch) {
            case 'h': 
                printf ("usage: %s [-p | -h] [-b]\n"
                        "\t-h Show this help\n"
                        "\t-b Put bar at the bottom of the screen\n"
                        "\t-f Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-p Don't close after the data ends\n", argv[0]); 
                exit (0);
            case 'p': permanent = 1; break;
            case 'b': bar_bottom = 1; break;
            case 'f': force_docking = 1; break;
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
