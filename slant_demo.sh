#!/bin/bash
#
# demonstrate slant flags and what they look like

Len=2
geom=400x20+10+10
slant_fg='#a7a7a7'
slant_bg='#3e3e3e'
colors="%{F${slant_fg}}%{B${slant_bg}}"
invert="%{B${slant_fg}}%{F${slant_bg}}"

reset='%{B-}%{F-}'

space()
{
    printf %${Len}s
}

slant()
{
    echo -n "${colors}%{${1}${Len}}$(space)"
}


echo  " \
${colors}%{E${Len}}$(space)${reset} - \
${colors}%{e${Len}}$(space)${reset} - \
${colors}%{D${Len}}$(space)${reset} - \
${colors}%{d${Len}}$(space)${reset}     \
\
$(slant d)${invert}slants$(slant E)${reset}and$(slant d)${invert}shit$(slant E)" \
| ./lemonbar -g $geom -p -B "$slant_bg" -F "$slant_fg"
