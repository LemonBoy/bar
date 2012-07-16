b(ar) a(in't) r(ecursive)
=========================
2012 (C) The Lemon Man

A lightweight bar based on XCB (yay). Provides foreground/background color
switching along with text alignment (screw you dzen!), nothing less and 
nothing more.

Configuration
-------------
Change the config.h file and you're good to go!
The text background and foreground are respectively the first and the second
entries in the palette (COLOR0 and COLOR1).

Text formatting
---------------
All the format commands are preceded by a backslash (\\). 
To draw a backslash just backslash escape it (\\\\). 

```
f<0-9>  Selects the text foreground color from the palette.
b<0-9>  Selects the text background color from the palette.

l       Aligns the text to the left.
c       Aligns the text to the center.
r       Aligns the text to the right.
```
