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

static xcb_connection_t *c;
static xcb_window_t root, win;
static xcb_gcontext_t gc;
static xcb_drawable_t canvas;
static int font_height, font_width, font_descent;
static int bar_width;
static const unsigned pal[] = {COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9};

#define MIN(a,b) ((a > b ? b : a))

void
fill_rect (int color, int x, int y, int width, int height)
{
    xcb_change_gc (c, gc, XCB_GC_FOREGROUND, (const unsigned []){ pal[color] });
    xcb_poly_fill_rectangle (c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

/* libc doesn't give a shit to -fshort-wchar, so here's our wcslen */
size_t
wcslen_ (wchar_t *s) { 
    size_t len; 

    for (len = 0; *s; s++, len++);

    return len;
}

int
draw_string (int x, int align, int fgcol, int bgcol, int udcol, wchar_t *text)
{
    int chunk_len;
    int chars_done;
    int pos_x;
    int str_lenght;
    int str_width;
    
    pos_x = x;
    chars_done = 0;

    str_lenght = MIN(bar_width / font_width, wcslen_ (text));
    str_width = str_lenght * font_width;

    if (str_width == 0)  
        return 0;

    switch (align) {
        case 1:
            xcb_copy_area (c, canvas, canvas, gc, bar_width / 2 - pos_x / 2, 0, 
                    bar_width / 2 - (pos_x + str_width) / 2, 0, pos_x, BAR_HEIGHT);
            pos_x = bar_width / 2 - (pos_x + str_width) / 2 + pos_x;
            break;
        case 2:
            xcb_copy_area (c, canvas, canvas, gc, bar_width - pos_x, 0, 
                    bar_width - pos_x - str_width, 0, pos_x, BAR_HEIGHT);
            pos_x = bar_width - str_width; 
            break;
    }

    /* Draw the background first */
    fill_rect (bgcol, pos_x, 0, str_width, BAR_HEIGHT);

    /* Draw the underline */
    if (BAR_UNDERLINE_HEIGHT) 
        fill_rect (udcol, pos_x, BAR_HEIGHT-BAR_UNDERLINE_HEIGHT, str_width, BAR_UNDERLINE_HEIGHT);

    /* Setup the colors */
    xcb_change_gc (c, gc, XCB_GC_FOREGROUND, (const unsigned []){ pal[fgcol] });
    xcb_change_gc (c, gc, XCB_GC_BACKGROUND, (const unsigned []){ pal[bgcol] });

    do {
        chunk_len = MIN(str_lenght - chars_done, 255);
        /* String baseline coordinates */
        xcb_image_text_16 (c, chunk_len, canvas, gc, pos_x,
                BAR_HEIGHT / 2 + font_height / 2 - font_descent, (xcb_char2b_t *)text + chars_done);
        chars_done += chunk_len;
        pos_x = chars_done * font_width;
    } while (chars_done < str_lenght);

    return pos_x;
}

void
parse (char *text)
{
    wchar_t parsed_text[2048] = {0, };

    wchar_t *q = parsed_text;
    char    *p = text;

    int pos_x = 0;
    int align = 0;

    int fgcol = 1;
    int bgcol = 0;
    int udcol = 0;

    fill_rect (0, 0, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        if (*p == 0x0 || *p == 0xA || (*p == '\\' && p++ && *p != '\\' && strchr ("fbulcr", *p))) {
            pos_x += draw_string (pos_x, align, fgcol, bgcol, udcol, parsed_text);
            switch (*p++) {
                case 0x0: /* EOL */
                case 0xA: /* NL */
                    return;

                case 'f': 
                    if (*p == 'r') *p = '1'; 
                    if (isdigit (*p)) fgcol = (*p++)-'0'; 
                    break;
                case 'b': 
                    if (*p == 'r') *p = '0'; 
                    if (isdigit (*p)) bgcol = (*p++)-'0';
                    break;
                case 'u': 
                    if (*p == 'r') *p = '0'; 
                    if (isdigit (*p)) udcol = (*p++)-'0';
                    break;

                case 'l': 
                    align = 0; 
                    pos_x = 0; 
                    break;
                case 'c': 
                    align = 1; 
                    pos_x = 0; 
                    break;
                case 'r': 
                    align = 2; 
                    pos_x = 0; 
                    break;
            }
            q = parsed_text;
        } else { /* utf-8 -> ucs-2 */
            if (!(p[0] & 0x80)) {
                *q++ = p[0] << 8; 
                p   += 1;
            }
            else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
                wchar_t t = (p[0] & 0x1f) << 6 | p[1] & 0x3f;
                *q++ = (t >> 8) | (t << 8);
                p   += 2;
            }
            else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
                wchar_t t = (p[0] & 0xf) << 12 | (p[1] & 0x3f) << 6 | p[2] & 0x3f;
                *q++ = (t >> 8) | (t << 8);
                p   += 3;
            }
        }
        *q = 0;
    }
}

void
cleanup (void)
{
    xcb_free_pixmap (c, canvas);
    xcb_destroy_window (c, win);
    xcb_free_gc (c, gc);
    xcb_disconnect (c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM) exit (0);
}

void
set_ewmh_atoms (void)
{
    xcb_intern_atom_cookie_t cookies[4];
    xcb_atom_t atoms[4];
    xcb_intern_atom_reply_t *reply;

    cookies[0] = xcb_intern_atom (c, 0, strlen ("_NET_WM_WINDOW_TYPE")     , "_NET_WM_WINDOW_TYPE");
    cookies[1] = xcb_intern_atom (c, 0, strlen ("_NET_WM_WINDOW_TYPE_DOCK"), "_NET_WM_WINDOW_TYPE_DOCK");
    cookies[2] = xcb_intern_atom (c, 0, strlen ("_NET_WM_DESKTOP")         , "_NET_WM_DESKTOP");
    cookies[3] = xcb_intern_atom (c, 0, strlen ("_NET_WM_STRUT_PARTIAL")   , "_NET_WM_STRUT_PARTIAL");

    reply = xcb_intern_atom_reply (c, cookies[0], NULL);
    atoms[0] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[1], NULL);
    atoms[1] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[2], NULL);
    atoms[2] = reply->atom; free (reply);
    reply = xcb_intern_atom_reply (c, cookies[3], NULL);
    atoms[3] = reply->atom; free (reply);

    /* Set the _NET_WM_WINDOW_TYPE_DOCK state */
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atoms[0], XCB_ATOM_ATOM, 32, 1, &atoms[1]);
    /* Show on every desktop */
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atoms[2], XCB_ATOM_CARDINAL, 32, 1, 
            (const unsigned []){ 0xffffffff } );
    /* Set the window geometry */
    xcb_change_property (c, XCB_PROP_MODE_REPLACE, win, atoms[3], XCB_ATOM_CARDINAL, 32, 12, 
            (const unsigned []) { 0, 0, BAR_HEIGHT, 0, 0, 0, 0, 0, bar_width, 0, 0} );
}

void
init (void)
{
    xcb_font_t font;
    xcb_screen_t *scr;
    xcb_query_font_reply_t *font_info;

    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error (c)) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (1);
    }
    /* Grab infos from the first screen */
    scr  = xcb_setup_roots_iterator (xcb_get_setup (c)).data;
    bar_width   = scr->width_in_pixels;
    root = scr->root;
    /* Load the font */
    font = xcb_generate_id (c);
    if (xcb_request_check (c, xcb_open_font_checked (c, font, strlen(BAR_FONT), BAR_FONT))) {
        fprintf (stderr, "Couldn't load the font\n");
        exit (1);
    }
    /* Grab infos from the font */
    font_info    = xcb_query_font_reply (c, xcb_query_font (c, font), NULL);
    font_height  = font_info->font_ascent + font_info->font_descent;
    font_width   = font_info->max_bounds.character_width;
    font_descent = font_info->font_descent;
    free (font_info);
    /* Create the main window */
    win = xcb_generate_id (c);
    xcb_create_window (c, XCB_COPY_FROM_PARENT, win, root, 0, 0, bar_width, BAR_HEIGHT, 
            0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual, 
            XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, 
            (const unsigned []){ pal[0], 1, XCB_EVENT_MASK_EXPOSURE });
    /* Set EWMH hints */
    set_ewmh_atoms ();
    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, root, bar_width, BAR_HEIGHT);
    /* Create the gc for drawing */
    gc = xcb_generate_id (c);
    xcb_create_gc (c, gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT,
            (const unsigned []){ pal[1], pal[0], font });
    /* Get rid of the font */
    xcb_close_font (c, font);
    /* Make the bar visible */
    xcb_map_window (c, win);
    xcb_flush (c);
}

int 
main (int argc, char **argv)
{
    static char input[1024] = {0, };
    struct pollfd pollin[2] = { 
        { .fd = STDIN_FILENO, .events = POLLIN }, 
        { .fd = -1          , .events = POLLIN }, 
    };

    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;

    int permanent = 0;

    char ch;
    while ((ch = getopt (argc, argv, "ph")) != -1) {
        switch (ch) {
            case 'h': 
                printf ("usage: %s [-p | -h]\n"
                        "\t-h Shows this help\n"
                        "\t-p Don't close after the data ends\n", argv[0]); 
                exit (0);
            case 'p': permanent = 1; break;
        }
    }

    atexit (cleanup);
    signal (SIGINT, sighandle);
    signal (SIGTERM, sighandle);
    init ();

    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor (c);

    fill_rect (0, 0, 0, bar_width, BAR_HEIGHT);

    for (;;) {
        int redraw = 0;

        if (poll ((struct pollfd *)&pollin, 2, -1) > 0) {
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
            xcb_copy_area (c, canvas, win, gc, 0, 0, 0, 0, bar_width, BAR_HEIGHT);
        xcb_flush (c);
    }

    return 0;
}
