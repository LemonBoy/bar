b(ar) a(in't) r(ecursive)
=========================
2012 (C) The Lemon Man

A lightweight bar based on XCB (yay). Provides foreground/background color
switching along with text alignment (screw you dzen!), nothing less and 
nothing more.

Options
-------
bar accepts a couple of command line switches.

```
-h      Show the help and bail out.
-p      Make the bar permanent.
-b      Show the bar at the bottom of the screen.
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
        To reset the bg/fg color just pass 'r' as color index.

l       Aligns the text to the left.
c       Aligns the text to the center.
r       Aligns the text to the right.
```
