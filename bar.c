#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <xcb/xcb.h>

#include "config.h"

// Here be dragons

static xcb_connection_t *c;
static int ft_height, ft_width;
static xcb_window_t root, win;
static xcb_gcontext_t gc;
static int bw, bh;
static const int pal[] = {COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9};

#define MIN(a,b) ((a > b ? b : a))

int
draw (int x, int align, char *text)
{
    int done = 0;
    int pos_x = x;
    int len = MIN(bw / ft_width, strlen (text));
    int strw = len * ft_width;

    if (!strw) return x;

    switch (align) {
        case 1:
            xcb_copy_area (c, win, win, gc, bw / 2 - pos_x / 2, 0, bw / 2 - (pos_x + strw) / 2, 0, pos_x, BAR_HEIGHT);
            pos_x = bw / 2 - (pos_x + strw) / 2 + pos_x;
            break;
        case 2:
            xcb_copy_area (c, win, win, gc, bw - pos_x, 0, bw - pos_x - strw, 0, pos_x, BAR_HEIGHT);
            pos_x = bw - strw; 
            break;
    }

    do {
        xcb_image_text_8 (c, MIN(len - done, 255), win, gc, pos_x, bh - ft_height / 2, text + done); /* Bottom-left coords */
        done += MIN(len - done, 255);
        pos_x = done * ft_width;
    } while (done < len);

    return pos_x;
}

void
parse (char *text)
{
    char parsed_text[1000] = {0, };

    char *p = text;
    char *q = parsed_text;

    int pos_x = 0;
    int align = 0;

    xcb_clear_area (c, 0, win, 0, 0, bw, BAR_HEIGHT);
    for (;;) {
        if (*p == 0x0 || *p == 0xA || (*p == '\\' && p++ && *p != '\\' && strchr ("fblcr", *p))) {
            pos_x += draw (pos_x, align, parsed_text);
            switch (*p++) {
                case 0x0: return; /* EOL */
                case 0xA: return; /* NL */
                case 'f': if (isdigit (*p)) xcb_change_gc (c, gc, XCB_GC_FOREGROUND, (const uint32_t []){ pal[*p-'0'] }); p++; break;
                case 'b': if (isdigit (*p)) xcb_change_gc (c, gc, XCB_GC_BACKGROUND, (const uint32_t []){ pal[*p-'0'] }); p++; break;
                case 'l': align = 0; pos_x = 0; break;
                case 'c': align = 1; pos_x = 0; break;
                case 'r': align = 2; pos_x = 0; break;
            }
            q = parsed_text;
        } 
        else *q++ = *p++; 
  
        *q = 0;
    }
}

void
cleanup (void)
{
    xcb_destroy_window (c, win);
    xcb_free_gc (c, gc);
    xcb_disconnect (c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
        exit (0);
}

void
init (void)
{
    xcb_font_t xf;
    xcb_screen_t *scr;
    xcb_query_font_reply_t *ft_info;

    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error (c)) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (1);
    }
    /* Grab infos from the first screen */
    scr = xcb_setup_roots_iterator (xcb_get_setup (c)).data;
    bw = scr->width_in_pixels;
    bh = BAR_HEIGHT;
    root = scr->root;
    /* Load the font */
    xf = xcb_generate_id (c);
    if (xcb_request_check (c, xcb_open_font_checked (c, xf, strlen(BAR_FONT), BAR_FONT))) {
        fprintf (stderr, "Couldn't load the font\n");
        exit (1);
    }
    /* Grab infos from the font */
    ft_info = xcb_query_font_reply (c, xcb_query_font (c, xf), NULL);
    ft_height = ft_info->font_ascent + ft_info->font_descent;
    ft_width = ft_info->max_bounds.character_width;
    /* Create the main window */
    win = xcb_generate_id (c);
    xcb_create_window (c, XCB_COPY_FROM_PARENT, win, root, 0, 0, bw, bh, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, 
            scr->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, 
            (const uint32_t []){ pal[0], 1 });
    /* Create the gc for drawing */
    gc = xcb_generate_id (c);
    xcb_create_gc (c, gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT,
            (const uint32_t []){ pal[1], pal[0], xf });
    /* Get rid of the font */
    xcb_close_font (c, xf);
    /* Make the bar visible */
    xcb_map_window (c, win);
    xcb_flush (c);
}

int 
main (void)
{
    struct pollfd stdinpoll = { .fd = 0, .events = POLLIN | POLLHUP };
    static char input[1000] = {0, };

    atexit (cleanup);
    signal (SIGINT, sighandle);
    signal (SIGTERM, sighandle);
    init ();

    for (;;) {
        if (poll (&stdinpoll, 1, -1) > 0) {
            if (stdinpoll.revents & POLLHUP) break;
            if (stdinpoll.revents & POLLIN) {
                fgets (input, sizeof(input), stdin);
                parse (input);
                xcb_flush (c);
            }
        }
    }

    return 0;
}
    
