#!/usr/bin/env perl
#
# palette.pl
#
# Converts your .Xdefault/.Xresources colors into a ready to paste palette
# for bar. It takes your foreground/background settings into account and if
# it cant find them it leaves COLOR0/COLOR1 undefined.
#

use strict;
use warnings;

open (F, "<".$ARGV[0]) || die "Can't open!";

while (<F>) {
    if ($_ =~ m/^\s*(?:\w+\.|\*)(color|background|foreground)(\d+)?\s*:\s*#([0-9A-Fa-f]*)/) {
        if    ($1 eq "background") { 
            printf "#define COLOR0\t0x$3\n"; 
        }
        elsif ($1 eq "foreground") { 
            printf "#define COLOR1\t0x$3\n" 
        }
        elsif ($1 eq "color" && $2 < 8) { 
            printf "#define COLOR%i\t0x$3\n", $2 + 2;
        }
    }
}
