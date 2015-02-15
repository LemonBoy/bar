// vim:sw=4:ts=4:et:
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
#include <xcb/xcbext.h>
#include <xcb/xinerama.h>
#include <xcb/randr.h>

// Here be dragons

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define indexof(c,s) (strchr((s),(c))-(s))

typedef struct font_t {
    xcb_font_t ptr;
    int descent, height;
    uint16_t char_max;
    uint16_t char_min;
    xcb_charinfo_t *width_lut;
} font_t;

typedef struct monitor_t {
    int x, y, width;
    xcb_window_t window;
    xcb_pixmap_t pixmap;
    struct monitor_t *prev, *next;
} monitor_t;

typedef struct area_t {
    bool active;
    int begin, end, align, button;
    xcb_window_t window;
    char *cmd;
} area_t;

typedef union rgba_t {
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };
    uint32_t v;
} rgba_t;

#define N 20

typedef struct area_stack_t {
    int pos;
    area_t slot[N];
} area_stack_t;

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

#define MAX_FONT_COUNT 5

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_gcontext_t gc[GC_MAX];
static xcb_visualid_t visual;
static xcb_colormap_t colormap;
static monitor_t *monhead, *montail;
static font_t *font_list[MAX_FONT_COUNT];
static int font_count = 0;
static int font_index = -1;
static uint32_t attrs = 0;
static bool dock = false;
static bool topbar = true;
static int bw = -1, bh = -1, bx = 0, by = 0;
static int bu = 1; // Underline height
static rgba_t fgc, bgc, ugc;
static rgba_t dfgc, dbgc;
static area_stack_t astack;

void
update_gc (void)
{
    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });
    xcb_change_gc(c, gc[GC_CLEAR], XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });
    xcb_change_gc(c, gc[GC_ATTR], XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });
}

void
fill_gradient (xcb_drawable_t d, int x, int y, int width, int height, rgba_t start, rgba_t stop)
{
    float i;
    const int K = 25; // The number of steps

    for (i = 0.; i < 1.; i += (1. / K)) {
        // Perform the linear interpolation magic
        unsigned int rr = i * stop.r + (1. - i) * start.r;
        unsigned int gg = i * stop.g + (1. - i) * start.g;
        unsigned int bb = i * stop.b + (1. - i) * start.b;

        // The alpha is ignored here
        rgba_t step = {
            .r = rr,
            .g = gg,
            .b = bb,
            .a = 255,
        };

        xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ step.v });
        xcb_poly_fill_rectangle(c, d, gc[GC_DRAW], 1,
                               (const xcb_rectangle_t []){ { x, i * bh, width, bh / K + 1 } });
    }

    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });
}

void
fill_rect (xcb_drawable_t d, xcb_gcontext_t _gc, int x, int y, int width, int height)
{
    rgba_t start = (rgba_t)0x222222U;
    rgba_t end = (rgba_t)0x111111U;
    fill_gradient(d, x, y, width, height, start, end);
    // xcb_poly_fill_rectangle(c, d, _gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

// Apparently xcb cannot seem to compose the right request for this call, hence we have to do it by
// ourselves.
// The funcion is taken from 'wmdia' (http://wmdia.sourceforge.net/)
xcb_void_cookie_t xcb_poly_text_16_simple(xcb_connection_t * c,
    xcb_drawable_t drawable, xcb_gcontext_t gc, int16_t x, int16_t y,
    uint32_t len, const uint16_t *str)
{
    static const xcb_protocol_request_t xcb_req = {
	    5,                // count
	    0,                // ext
	    XCB_POLY_TEXT_16, // opcode
	    1                 // isvoid
    };
    struct iovec xcb_parts[7];
    uint8_t xcb_lendelta[2];
    xcb_void_cookie_t xcb_ret;
    xcb_poly_text_8_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.drawable = drawable;
    xcb_out.gc = gc;
    xcb_out.x = x;
    xcb_out.y = y;

    xcb_lendelta[0] = len;
    xcb_lendelta[1] = 0;

    xcb_parts[2].iov_base = (char *)&xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_parts[4].iov_base = xcb_lendelta;
    xcb_parts[4].iov_len = sizeof(xcb_lendelta);
    xcb_parts[5].iov_base = (char *)str;
    xcb_parts[5].iov_len = len * sizeof(int16_t);

    xcb_parts[6].iov_base = 0;
    xcb_parts[6].iov_len = -(xcb_parts[4].iov_len + xcb_parts[5].iov_len) & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

#define pad_to(x, to) (((x) + (to) - 1) & ~((to) - 1))

typedef struct xbm_t {
    int width;
    int height;
    uint8_t *bits;
    size_t bits_size;
} xbm_t;

xbm_t xbm_cache[20];
int xbm_cache_elem = 0;

xbm_t *
xbm_load (uint8_t *bits, int width, int height)
{
    const xcb_setup_t *setup = xcb_get_setup(c);
    uint8_t *buf;
    int width_b, line;

    xbm_t *ico = &xbm_cache[xbm_cache_elem];

    // XCB_IMAGE_ORDER_LSB_FIRST
    // bitmap_format_bit_order
    width_b = (width + 7) / 8;
    line = pad_to(width_b, setup->bitmap_format_scanline_pad >> 3);

    buf = malloc(height * line);
    if (!buf)
        return NULL;

    // Despite the naming, this is actually a stencil
    for (int i = 0; i < height * width_b; i++)
        bits[i] = ~bits[i];

    for (int i = 0; i < height; i++)
        memcpy(buf  + (i * line),
               bits + (i * width_b),
               width_b);

    ico->width = width;
    ico->height = height;
    ico->bits = buf;
    ico->bits_size = height * line;

    xbm_cache_elem++;

    return ico;
}

int
pixmap_shift (monitor_t *mon, int x, int align, int w)
{
    switch (align) {
        case ALIGN_L:
            return x;

        case ALIGN_C:
            xcb_copy_area(c, mon->pixmap, mon->pixmap, gc[GC_DRAW],
                    mon->width / 2 - x / 2, 0,
                    mon->width / 2 - (x + w) / 2, 0,
                    x, bh);
            return mon->width / 2 - (x + w) / 2 + x;

        case ALIGN_R:
            xcb_copy_area(c, mon->pixmap, mon->pixmap, gc[GC_DRAW],
                    mon->width - x, 0,
                    mon->width - x - w, 0,
                    x, bh);
            return mon->width - w;
    }

    return 0;
}

int
draw_icon (monitor_t *mon, int x, xbm_t *icon, int align)
{
    xcb_gcontext_t tmp_gc;
    xcb_pixmap_t mask;

    x = pixmap_shift(mon, x, align, icon->width);

    mask = xcb_generate_id(c);
    xcb_create_pixmap(c, 1, mask, mon->pixmap, icon->width, icon->height);

    tmp_gc = xcb_generate_id(c);
    xcb_create_gc(c, tmp_gc, mask, 0, NULL);

    xcb_put_image(
            c,
            XCB_IMAGE_FORMAT_XY_BITMAP,
            mask,
            tmp_gc,
            icon->width, icon->height,
            0, 0,
            0, 1,
            icon->bits_size, icon->bits);

    xcb_free_gc(c, tmp_gc);

    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_CLIP_MASK, (const uint32_t []){ mask });
    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_CLIP_ORIGIN_X | XCB_GC_CLIP_ORIGIN_Y,
            (const uint32_t []){ x, bh / 2 - icon->height / 2 });
    xcb_poly_fill_rectangle(c, mon->pixmap, gc[GC_DRAW], 1,
            (const xcb_rectangle_t []){ { 0, 0, bw, bh } });
    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_CLIP_MASK, (const uint32_t []){ XCB_NONE });

    xcb_free_pixmap(c, mask);

    return icon->width;
}

int
draw_char (monitor_t *mon, font_t *cur_font, int x, int align, uint16_t ch)
{
    int ch_width = cur_font->width_lut[ch - cur_font->char_min].character_width;

    x = pixmap_shift(mon, x, align, ch_width);
    // Draw the background first
    fill_rect(mon->pixmap, gc[GC_CLEAR], x, by, ch_width, bh);

    // xcb accepts string in UCS-2 BE, so swap
    ch = (ch >> 8) | (ch << 8);

    // The coordinates here are those of the baseline
    xcb_poly_text_16_simple(c, mon->pixmap, gc[GC_DRAW],
                            x, bh / 2 + cur_font->height / 2 - cur_font->descent,
                            1, &ch);

    // We can render both at the same time
    if (attrs & ATTR_OVERL)
        fill_rect(mon->pixmap, gc[GC_ATTR], x, 0, ch_width, bu);
    if (attrs & ATTR_UNDERL)
        fill_rect(mon->pixmap, gc[GC_ATTR], x, bh - bu, ch_width, bu);

    return ch_width;
}

    static unsigned char fox_bits[] = {
        0x70, 0x88, 0xC4, 0xE2, 0x7E, 0x32, 0x09, 0x07
        0x81, 0xC3, 0xBD, 0xFF, 0x99, 0xDB, 0x7E, 0x18,
    };
    int xbm_w = 8;
    int xbm_h = 8;


rgba_t
parse_color (const char *str, char **end, const rgba_t def)
{
    xcb_alloc_named_color_reply_t *nc_reply;
    int string_len;
    rgba_t ret;
    char *ep;

    if (!str)
        return def;

    // Reset
    if (str[0] == '-') {
        if (end)
            *end = (char *)str + 1;

        return def;
    }

    // Hex representation
    if (str[0] == '#') {
        errno = 0;
        rgba_t tmp = (rgba_t)(uint32_t)strtoul(str + 1, &ep, 16);

        if (end)
            *end = ep;

        // Some error checking is definitely good
        if (errno) {
            fprintf(stderr, "Invalid color specified\n");
            return def;
        }

        string_len = ep - (str + 1);

        // If the code is in #rrggbb form then assume it's opaque
        if (string_len <= 6)
            tmp.a = 255;

        // Premultiply the alpha in
        if (tmp.a) {
            // The components are clamped automagically as the rgba_t is made of uint8_t
            return (rgba_t){
                .r = (tmp.r * tmp.a) / 255,
                .g = (tmp.g * tmp.a) / 255,
                .b = (tmp.b * tmp.a) / 255,
                .a = tmp.a,
            };
        }

        return (rgba_t)0U;
    }

    // Actual color name, resolve it
    for (string_len = 0; isalpha(str[string_len]); string_len++)
        ;

    nc_reply = xcb_alloc_named_color_reply(c, xcb_alloc_named_color(c, colormap, string_len, str), NULL);

    if (!nc_reply)
        fprintf(stderr, "Could not alloc color \"%.*s\"\n", string_len, str);

    ret = nc_reply?
        (rgba_t)nc_reply->pixel:
        def;

    free(nc_reply);

    if (end)
        *end = (char *)str + string_len;

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


area_t *
area_get (xcb_window_t win, const int btn, const int x)
{
    // Looping backwards ensures that we get the innermost area first
    for (int i = astack.pos; i >= 0; i--) {
        area_t *a = &astack.slot[i];
        if (a->window == win && a->button == btn
                && x >= a->begin && x < a->end)
            return a;
    }
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
    int i;
    char *trail;
    area_t *a;

    // A wild close area tag appeared!
    if (*str != ':') {
        *end = str;

        // Find most recent unclosed area.
        for (i = astack.pos - 1; i >= 0 && !astack.slot[i].active; i--)
            ;
        a = &astack.slot[i];

        // Basic safety checks
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
                // The newest is the rightmost one
                a->begin = mon->width - size;
                a->end = mon->width;
                break;
        }

        a->active = false;
        return true;
    }

    if (astack.pos >= N) {
        fprintf(stderr, "astack overflow!\n");
        return false;
    }
    a = &astack.slot[astack.pos++];

    // Found the closing : and check if it's just an escaped one
    for (trail = strchr(++str, ':'); trail && trail[-1] == '\\'; trail = strchr(trail + 1, ':'))
        ;

    // Find the trailing : and make sure it's within the formatting block, also reject empty commands
    if (!trail || str == trail || trail > optend) {
        *end = str;
        return false;
    }

    *trail = '\0';

    // Sanitize the user command by unescaping all the :
    for (char *needle = str; *needle; needle++) {
        int delta = trail - &needle[1];
        if (needle[0] == '\\' && needle[1] == ':') {
            memmove(&needle[0], &needle[1], delta);
            needle[delta] = 0;
        }
    }

    // This is a pointer to the string buffer allocated in the main
    a->cmd = str;
    a->active = true;
    a->align = align;
    a->begin = x;
    a->window = mon->window;
    a->button = button;

    *end = trail + 1;

    return true;
}

bool
font_has_glyph (font_t *font, const uint16_t c)
{
    return (c >= font->char_min &&
            c <= font->char_max &&
            font->width_lut &&
            font->width_lut[c - font->char_min].character_width);
}

// returns NULL if character cannot be printed
font_t *
select_drawable_font (const uint16_t c)
{
    // If the user has specified a font to use, try that first.
    if (font_index != -1 && font_has_glyph(font_list[font_index - 1], c))
        return font_list[font_index - 1];

    // If the end is reached without finding an apropriate font, return NULL.
    // If the font can draw the character, return it.
    for (int i = 0; i < font_count; i++) {
        if (font_has_glyph(font_list[i], c))
            return font_list[i];
    }
    return NULL;
}

void
parse (char *text)
{
    font_t *cur_font;
    monitor_t *cur_mon;
    int pos_x, align, button;
    char *p = text, *end;
    rgba_t tmp;

    pos_x = 0;
    align = ALIGN_L;
    cur_mon = monhead;

    memset(&astack, 0, sizeof(area_stack_t));

    for (monitor_t *m = monhead; m != NULL; m = m->next)
        fill_rect(m->pixmap, gc[GC_CLEAR], 0, 0, m->width, bh);

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

                    case 'A':
                              button = XCB_BUTTON_INDEX_1;
                              // The range is 1-5
                              if (isdigit(*p) && (*p > '0' && *p < '6'))
                                  button = *p++ - '0';
                              area_add(p, end, &p, cur_mon, pos_x, align, button);
                              break;

                    case 'B': bgc = parse_color(p, &p, dbgc); update_gc(); break;
                    case 'F': fgc = parse_color(p, &p, dfgc); update_gc(); break;
                    case 'U': ugc = parse_color(p, &p, dbgc); update_gc(); break;

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

                    case 'T':
                              font_index = (int)strtoul(p, NULL, 10);
                              // User-specified 'font_index' ∊ (0,font_count]
                              // Otherwise just fallback to the automatic font selection
                              if (!font_index || font_index > font_count)
                                  font_index = -1;
                              p = end;
                              break;

                    case 'i':
                              ;
                              int icon_index = (int)strtoul(p, NULL, 10);
                              if (icon_index < xbm_cache_elem) {
                                  int w = draw_icon(cur_mon, pos_x, &xbm_cache[0], align);
                                  pos_x += w;
                                  area_shift(cur_mon->window, align, w);
                              }
                              p = end;
                              break;

                    // In case of error keep parsing after the closing }
                    default:
                        p = end;
                }
            }
            // Eat the trailing }
            p++;
        } else { // utf-8 -> ucs-2
            uint8_t *utf = (uint8_t *)p;
            uint16_t ucs;

            // ASCII
            if (utf[0] < 0x80) {
                ucs = utf[0];
                p  += 1;
            }
            // Two byte utf8 sequence
            else if ((utf[0] & 0xe0) == 0xc0) {
                ucs = (utf[0] & 0x1f) << 6 | (utf[1] & 0x3f);
                p += 2;
            }
            // Three byte utf8 sequence
            else if ((utf[0] & 0xf0) == 0xe0) {
                ucs = (utf[0] & 0xf) << 12 | (utf[1] & 0x3f) << 6 | (utf[2] & 0x3f);
                p += 3;
            }
            // Four byte utf8 sequence
            else if ((utf[0] & 0xf8) == 0xf0) {
                ucs = 0xfffd;
                p += 4;
            }
            // Five byte utf8 sequence
            else if ((utf[0] & 0xfc) == 0xf8) {
                ucs = 0xfffd;
                p += 5;
            }
            // Siz byte utf8 sequence
            else if ((utf[0] & 0xfe) == 0xfc) {
                ucs = 0xfffd;
                p += 6;
            }
            // Not a valid utf-8 sequence
            else {
                ucs = utf[0];
                p += 1;
            }

            cur_font = select_drawable_font(ucs);
            if (!cur_font)
                continue;

            xcb_change_gc(c, gc[GC_DRAW] , XCB_GC_FONT, (const uint32_t []){ cur_font->ptr });

            int w = draw_char(cur_mon, cur_font, pos_x, align, ucs);
            pos_x += w;

            area_shift(cur_mon->window, align, w);
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
        fprintf(stderr, "Could not load font \"%s\"\n", str);
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

    // Copy over the width lut as it's part of font_info
    int lut_size = sizeof(xcb_charinfo_t) * xcb_query_font_char_infos_length(font_info);
    if (lut_size) {
        ret->width_lut = malloc(lut_size);
        memcpy(ret->width_lut, xcb_query_font_char_infos(font_info), lut_size);
    }

    free(font_info);

    return ret;
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
        // Leave those at the end since are batch-set
        "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_ABOVE",
    };
    const int atoms = sizeof(atom_names)/sizeof(char *);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
    xcb_atom_t atom_list[atoms];
    xcb_intern_atom_reply_t *atom_reply;

    // As suggested fetch all the cookies first (yum!) and then retrieve the
    // atoms to exploit the async'ness
    for (int i = 0; i < atoms; i++)
        atom_cookie[i] = xcb_intern_atom(c, 0, strlen(atom_names[i]), atom_names[i]);

    for (int i = 0; i < atoms; i++) {
        atom_reply = xcb_intern_atom_reply(c, atom_cookie[i], NULL);
        if (!atom_reply)
            return;
        atom_list[i] = atom_reply->atom;
        free(atom_reply);
    }

    // Prepare the strut array
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
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 3, "bar");
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
    ret->y = (topbar ? by : height - bh - by) + y;
    ret->width = width;
    ret->next = ret->prev = NULL;
    ret->window = xcb_generate_id(c);

    int depth = (visual == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
    xcb_create_window(c, depth, ret->window, scr->root,
            ret->x, ret->y, width, bh, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
            XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP,
            (const uint32_t []){ bgc.v, bgc.v, dock, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS, colormap });

    ret->pixmap = xcb_generate_id(c);
    xcb_create_pixmap(c, depth, ret->pixmap, ret->window, width, bh);

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

    // Sort before use
    qsort(rects, num, sizeof(xcb_rectangle_t), rect_sort_cb);

    for (i = 0; i < num; i++) {
        int h = rects[i].y + rects[i].height;
        // Accumulated width of all monitors
        width += rects[i].width;
        // Get height of screen from y_offset + height of lowest monitor
        if (h >= height)
        height = h;
    }

    if (bw < 0)
        bw = width - bx;

    // Use the first font height as all the font heights have been set to the biggest of the set
    if (bh < 0 || bh > height)
        bh = font_list[0]->height + bu + 2;

    // Check the geometry
    if (bx + bw > width || by + bh > height) {
        fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
        exit(EXIT_FAILURE);
    }

    // Left is a positive number or zero therefore monitors with zero width are excluded
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

            // No need to check for other monitors
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


    // There should be at least one output
    if (num < 1) {
        free(rres_reply);
        return;
    }

    xcb_rectangle_t rects[num];

    // Get all outputs
    for (i = 0; i < num; i++) {
        xcb_randr_get_output_info_reply_t *oi_reply;
        xcb_randr_get_crtc_info_reply_t *ci_reply;

        oi_reply = xcb_randr_get_output_info_reply(c, xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME), NULL);

        // Output disconnected or not attached to any CRTC ?
        if (!oi_reply || oi_reply->crtc == XCB_NONE || oi_reply->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oi_reply);
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

        // There's no need to handle rotated screens here (see #69)
        rects[i] = (xcb_rectangle_t){ ci_reply->x, ci_reply->y, ci_reply->width, ci_reply->height };

        free(ci_reply);

        valid++;
    }

    free(rres_reply);

    // Check for clones and inactive outputs
    for (i = 0; i < num; i++) {
        if (rects[i].width == 0)
            continue;

        for (j = 0; j < num; j++) {
            // Does I countain J ?

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

    // Fetch all the screens first
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

xcb_visualid_t
get_visual (void)
{
    xcb_depth_iterator_t iter;

    iter = xcb_screen_allowed_depths_iterator(scr);

    // Try to find a RGBA visual
    while (iter.rem) {
        xcb_visualtype_t *vis = xcb_depth_visuals(iter.data);

        if (iter.data->depth == 32)
            return vis->visual_id;

        xcb_depth_next(&iter);
    }

    // Fallback to the default one
    return scr->root_visual;
}

// Parse an X-styled geometry string, we don't support signed offsets tho.
bool
parse_geometry_string (char *str, int *tmp)
{
    char *p = str;
    int i = 0, j;

    if (!str || !str[0])
        return false;

    // The leading = is optional
    if (*p == '=')
        p++;

    while (*p) {
        // A geometry string has only 4 fields
        if (i >= 4) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        // Move on if we encounter a 'x' or '+'
        if (*p == 'x') {
            if (i > 0) // The 'x' must precede '+'
                break;
            i++; p++; continue;
        }
        if (*p == '+') {
            if (i < 1) // Stray '+', skip the first two fields
                i = 2;
            else
                i++;
            p++; continue;
        }
        // A digit must follow
        if (!isdigit(*p)) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        // Try to parse the number
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
    char *tok, *end;

    if (!str)
        return;

    tok = strtok(str, ",");

    while (tok) {
        if (font_count > MAX_FONT_COUNT - 1) {
            fprintf(stderr, "Too many fonts; maximum %i\n", MAX_FONT_COUNT);
            return;
        }

        // Strip the leading and trailing whitespaces
        while (isspace(*tok) || iscntrl(*tok))
            tok++;

        end = tok + strlen(tok) - 1;

        while ((end > tok && isspace(*end)) || iscntrl(*end))
            end--;

        *(end + 1) = '\0';

        if (tok[0]) {
            // Load the selected font
            font_t *font = font_load(tok);
            if (font)
                font_list[font_count++] = font;
        }
        else
            fprintf(stderr, "Empty font name, skipping...\n");

        tok = strtok(NULL, ",");
    }
}

void
xconn (void)
{
    // Connect to X
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "Couldn't connect to X\n");
        exit(EXIT_FAILURE);
    }

    // Grab infos from the first screen
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    // Try to get a RGBA visual and build the colormap for that
    visual = get_visual();

    colormap = xcb_generate_id(c);
    xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, scr->root, visual);
}

void
init (void)
{
    // This has to be declared as an array because otherwise the compiler would turn it into a const
    // string, making strtok choke very hard on this
    char fallback_font[] = "fixed";

    // Try to load a default font
    if (!font_count)
        parse_font_list(fallback_font);

    // We tried and failed hard, there's something wrong
    if (!font_count)
        exit(EXIT_FAILURE);

    // To make the alignment uniform, find maximum height
    int maxh = font_list[0]->height;
    for (int i = 1; i < font_count; i++)
        maxh = max(maxh, font_list[i]->height);

    // Set maximum height to all fonts
    for (int i = 0; i < font_count; i++)
        font_list[i]->height = maxh;

    // Generate a list of screens
    const xcb_query_extension_reply_t *qe_reply;

    // Initialiaze monitor list head and tail
    monhead = montail = NULL;

    // Check if RandR is present
    qe_reply = xcb_get_extension_data(c, &xcb_randr_id);

    if (qe_reply && qe_reply->present) {
        get_randr_monitors();
    } else {
        qe_reply = xcb_get_extension_data(c, &xcb_xinerama_id);

        // Check if Xinerama extension is present and active
        if (qe_reply && qe_reply->present) {
            xcb_xinerama_is_active_reply_t *xia_reply;
            xia_reply = xcb_xinerama_is_active_reply(c, xcb_xinerama_is_active(c), NULL);

            if (xia_reply && xia_reply->state)
                get_xinerama_monitors();

            free(xia_reply);
        }
    }

    if (!monhead) {
        // If I fits I sits
        if (bw < 0)
            bw = scr->width_in_pixels - bx;

        // Adjust the height
        if (bh < 0 || bh > scr->height_in_pixels)
            bh = maxh + bu + 2;

        // Check the geometry
        if (bx + bw > scr->width_in_pixels || by + bh > scr->height_in_pixels) {
            fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
            exit(EXIT_FAILURE);
        }

        // If no RandR outputs or Xinerama screens, fall back to using whole screen
        monhead = monitor_new(0, 0, bw, scr->height_in_pixels);
    }

    if (!monhead)
        exit(EXIT_FAILURE);

    // For WM that support EWMH atoms
    set_ewmh_atoms();

    // Create the gc for drawing
    gc[GC_DRAW] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_DRAW], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });

    gc[GC_CLEAR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_CLEAR], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });

    gc[GC_ATTR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_ATTR], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });

    // Make the bar visible and clear the pixmap
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        fill_rect(mon->pixmap, gc[GC_CLEAR], 0, 0, mon->width, bh);
        xcb_map_window(c, mon->window);

        // Make sure that the window really gets in the place it's supposed to be
        // Some WM such as Openbox need this
        xcb_configure_window(c, mon->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, (const uint32_t []){ mon->x, mon->y });
    }

    xcb_flush(c);
}

void
cleanup (void)
{
    for (int i = 0; i < font_count; i++) {
        xcb_close_font(c, font_list[i]->ptr);
        free(font_list[i]->width_lut);
        free(font_list[i]);
    }

    while (monhead) {
        monitor_t *next = monhead->next;
        xcb_destroy_window(c, monhead->window);
        xcb_free_pixmap(c, monhead->pixmap);
        free(monhead);
        monhead = next;
    }

    xcb_free_colormap(c, colormap);

    if (gc[GC_DRAW])
        xcb_free_gc(c, gc[GC_DRAW]);
    if (gc[GC_CLEAR])
        xcb_free_gc(c, gc[GC_CLEAR]);
    if (gc[GC_ATTR])
        xcb_free_gc(c, gc[GC_ATTR]);
    if (c)
        xcb_disconnect(c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
        exit(EXIT_SUCCESS);
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
    int ch;

    // Install the parachute!
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    // Connect to the Xserver and initialize scr
    xconn();

    // B/W combo
    dbgc = bgc = parse_color("black", NULL, (rgba_t)scr->black_pixel);
    dfgc = fgc = parse_color("white", NULL, (rgba_t)scr->white_pixel);

    ugc = fgc;

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
            case 'B': dbgc = bgc = parse_color(optarg, NULL, (rgba_t)scr->black_pixel); break;
            case 'F': dfgc = fgc = parse_color(optarg, NULL, (rgba_t)scr->white_pixel); break;
        }
    }

    // Copy the geometry values in place
    bw = geom_v[0];
    bh = geom_v[1];
    bx = geom_v[2];
    by = geom_v[3];

    // Do the heavy lifting
    init();
    // Get the fd to Xserver
    pollin[1].fd = xcb_get_file_descriptor(c);

    xbm_load (fox_bits, xbm_w, xbm_h);

    for (;;) {
        bool redraw = false;

        // If connection is in error state, then it has been shut down.
        if (xcb_connection_has_error(c))
            break;

        if (poll(pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      // No more data...
                if (permanent) pollin[0].fd = -1;   // ...null the fd and continue polling :D
                else break;                         // ...bail out
            }
            if (pollin[0].revents & POLLIN) { // New input, process it
                if (fgets(input, sizeof(input), stdin) == NULL)
                    break; // EOF received

                parse(input);
                redraw = true;
            }
            if (pollin[1].revents & POLLIN) { // Xserver broadcasted an event
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
                                area_t *area = area_get(press_ev->event, press_ev->detail, press_ev->event_x);
                                // Respond to the click
                                if (area) {
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

        if (redraw) { // Copy our temporary pixmap onto the window
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
                xcb_copy_area(c, mon->pixmap, mon->window, gc[GC_DRAW], 0, 0, 0, 0, mon->width, bh);
            }
        }

        xcb_flush(c);
    }

    return EXIT_SUCCESS;
}
