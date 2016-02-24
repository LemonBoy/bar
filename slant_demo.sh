#!/bin/bash
#
# demonstrate slant flags and what they look like

Len=2
geom=200x20+10+10
bg='%{B#ff000000}'
sbg='%{B#ff00ff00}'

space()
{
	printf %${Len}s
}

echo  " \
${sbg}%{E${Len}}$(space)${bg} - \
${sbg}%{e${Len}}$(space)${bg} - \
${sbg}%{D${Len}}$(space)${bg} - \
${sbg}%{d${Len}}$(space)${bg} " \
| ./lemonbar -g $geom -p


