CC	?= gcc
STRIP ?= strip
CFLAGS = -std=c99 -O2
LDFLAGS = -lxcb -lxcb-xinerama
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
	rm -rf ./*.o
	rm -rf ./${EXEC}

install: bar
	test -d ${DESTDIR}${BINDIR} || mkdir -p ${DESTDIR}${BINDIR}
	install -m755 bar ${DESTDIR}${BINDIR}/bar

uninstall:
	rm -f ${DESTDIR}${BINDIR}/bar

.PHONY: all debug clean install
