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

our %vars = ();

while (<F>) {
    # Don't match comments
    if ($_ !~ m/^\s*!/) {
        # It's a define!
        if ($_ =~ m/^\s*#define\s+(\w+)\s+#([0-9A-Fa-f]{1,6})/) {
            $vars{"$1"} = hex($2);
        }
        elsif ($_ =~ m/^\s*\w*\*(background|foreground|color\d)\s*:\s*([\w\d#]+)/) {
            my ($name, $value) = (uc $1, $2); 
            # Check if it's a color
            if (substr($value, 1) eq '#') {
                $value = hex(substr($value, 1));
            } else {
                $value = $vars{"$value"};
            }
            printf "#define $name 0x%06x\n", $value;
        }
    }
}

