# SPDX-License-Identifier: GPL-2.0
# make          DOS build (Open Watcom, $WATCOM), NEWSCARD.EXE
# make host     Linux build against the simulated card only, for tests
# make test     run the tests with the host build

WATCOM ?= $(HOME)/opt/watcom
WCL = $(WATCOM)/binl64/wcl
SRC = ncmain.c nccard.c bus.c bus_sim.c ui.c scr.c pcclock.c
DOS_SRC = $(SRC) bus_ss.c
HDR = nc.h bus.h sim.h ss.h app.h scr.h pcclock.h

all: NEWSCARD.EXE

NEWSCARD.EXE: $(DOS_SRC) $(HDR)
	WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h PATH=$(WATCOM)/binl64:$$PATH \
	  $(WCL) -q -bt=dos -ms -0 -os -w4 -we -zq -fe=$@ $(DOS_SRC)
	rm -f *.o *.obj *.err

host: newscard-host

newscard-host: $(SRC) $(HDR)
	$(CC) -std=c89 -pedantic -Wall -Wextra -Werror -O2 -g -o $@ $(SRC)

test: newscard-host
	sh tests/test.sh

clean:
	rm -f NEWSCARD.EXE newscard-host *.o *.obj *.err

.PHONY: all host test clean
