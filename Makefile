CC	?= gcc
STRIP ?= strip
CFLAGS = -std=c99 -O2
<<<<<<< HEAD
LDFLAGS = -lxcb -lxcb-xinerama
=======
LDFLAGS = -lxcb -lxcb-xinerama -lxcb-randr
>>>>>>> 59fb6c5... Ported randr code to wip branch, plus some fixes.
CFDEBUG = -g3 -pedantic -Wall -Wunused-parameter -Wlong-long\
		  -Wsign-conversion -Wconversion -Wimplicit-function-declaration

EXEC = bar
SRCS = bar.c
OBJS = ${SRCS:.c=.o}

PREFIX?=/usr
BINDIR=${PREFIX}/bin

all: ${EXEC}

.c.o:
	${CC} ${CFLAGS} -o $@ -c $<

${OBJS}: config.h

config.h:
	@echo creating $@ from config.def.h
	@cp config.def.h $@

${EXEC}: ${OBJS}
	${CC} -o ${EXEC} ${OBJS} ${LDFLAGS}
	${STRIP} -s ${EXEC}

debug: ${EXEC}
debug: CC += ${CFDEBUG}

clean:
<<<<<<< HEAD
	rm -rf ./*.o
	rm -rf ./${EXEC}
=======
	rm -f ./config.h
	rm -f ./*.o
	rm -f ./${EXEC}
>>>>>>> 59fb6c5... Ported randr code to wip branch, plus some fixes.

install: bar
	test -d ${DESTDIR}${BINDIR} || mkdir -p ${DESTDIR}${BINDIR}
	install -m755 bar ${DESTDIR}${BINDIR}/bar

uninstall:
	rm -f ${DESTDIR}${BINDIR}/bar

.PHONY: all debug clean install
