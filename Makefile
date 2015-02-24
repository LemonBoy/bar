CC	?= gcc
STRIP ?= strip
CFLAGS = -Wall -std=c99 -I/usr/include/freetype2 -Os
LDFLAGS = -lxcb -lxcb-xinerama -lxcb-randr -lX11 -lX11-xcb -lXft -lfreetype -lz -lfontconfig
CFDEBUG = -g3 -pedantic -Wall -Wunused-parameter -Wlong-long\
		  -Wsign-conversion -Wconversion -Wimplicit-function-declaration

EXEC = bar
SRCS = bar.c
OBJS = ${SRCS:.c=.o}

PREFIX?=/usr
BINDIR=${PREFIX}/bin

all: ${EXEC}

doc: README.pod
	pod2man --section=1 --center="bar Manual" --name "bar" --release="bar $(shell git describe --always)" README.pod > bar.1

.c.o:
	${CC} ${CFLAGS} -o $@ -c $<

${EXEC}: ${OBJS}
	${CC} -o ${EXEC} ${OBJS} ${LDFLAGS}

debug: ${EXEC}
debug: CC += ${CFDEBUG}

clean:
	rm -f ./*.o ./*.1
	rm -f ./${EXEC}

install: bar doc
	install -D -m 755 bar ${DESTDIR}${BINDIR}/bar
	install -D -m 644 bar.1 ${DESTDIR}${PREFIX}/share/man/man1/bar.1

uninstall:
	rm -f ${DESTDIR}${BINDIR}/bar
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/bar.1

.PHONY: all debug clean install
