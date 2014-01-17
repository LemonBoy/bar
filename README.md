b(ar) a(in't) r(ecursive)
=========================
2012-2013 (C) The Lemon Man

A lightweight bar based on XCB (yay). Provides foreground/background color
switching along with text alignment (screw you dzen!), full utf8 support
and reduced memory footprint. It also supports transparency when using a 
compositor such as compton. Nothing less and nothing more.

Xinerama support
----------------
Thanks to @Stebalien now bar is Xinerama compliant, just compile it with
XINERAMA=1 and you're good to go!

Options
-------
bar accepts a couple of command line switches.

```
-h	    Show the help and bail out.
-b          Show the bar at the bottom of the screen.
-w <width>  Set width of the bar.
-s <height> Set height of the bar.
-o <offset> Set bar's offset from the left. 0 disables.
-f      Force docking (use this if your WM isn't EWMH compliant)
-u <line>   Choose between underline (1) or overline (0).
-t <thickness> Thickness of underline. 0 to disable.
-n <font>   Font used. In the form of '-*-*-' etc.
-k <width> Set font fallback width.
-c <opacity> Requires a compositor. 0: Invisible, 1: Opaque.
-p	    Make the bar permanent.
```

Configuration
-------------
Change the config.h file and you're good to go!
The text background and foreground are respectively the first and the second
entries in the palette (COLOR0 and COLOR1).

Colors
------
Attached there's palette.pl, an handy tool that extracts a palette from your
X colors and returns it ready to be pasted in the configuration file.

```
palette.pl <.Xresources / .Xdefaults path>
```

If you keep your colors in a separate file just feed that file and you're good
to go.

Text formatting
---------------
All the format commands are preceded by a backslash (\\). 
To draw a backslash just backslash escape it (\\\\). 

```
f<0-9>  Selects the text foreground color from the palette.
b<0-9>  Selects the text background color from the palette.
u<0-9>  Selects the underline color from the palette.
        To reset the bg/fg/underline color just pass 'r' as the color index.

l       Aligns the text to the left.
c       Aligns the text to the center.
r       Aligns the text to the right.
```


The options below are valid only if compiled with Xinerama support.


```
s<0-9>  Switches to screen 0-9
sn      Switches to next screen
sp      Switches to previous screen
sr      Switches to the rightmost screen (the latest)
sl      Switches to the leftmost screen (the first)
```
