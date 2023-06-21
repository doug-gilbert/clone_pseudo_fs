SHELL = /bin/sh

PREFIX=/usr/local
INSTDIR=$(DESTDIR)/$(PREFIX)/bin
MANDIR=$(DESTDIR)/$(PREFIX)/man

EXECS = renameat2
	
EXTRAS =


MAN_PGS =
MAN_PREF = man8

LARGE_FILE_FLAGS = -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

# For C++/clang testing
## CC = gcc
## CXX = g++
## CC = clang
## CXX = clang++

LD = $(CXX)
CXXLD = $(CXX)

CPPFLAGS = -iquote ../include -iquote .. -D_REENTRANT $(LARGE_FILE_FLAGS) -DHAVE_CONFIG_H 
CXXFLAGS = -std=c++11 -pthread -ggdb -O2 -W -Wall -iquote ../include -D_REENTRANT $(LARGE_FILE_FLAGS)
## CXXFLAGS = -std=c++14 -pthread -ggdb -O2 -W -Wall -iquote ../include -D_REENTRANT $(LARGE_FILE_FLAGS)
## CXXFLAGS = -std=c++17 -pthread -ggdb -O2 -W -Wall -iquote ../include -D_REENTRANT $(LARGE_FILE_FLAGS)
## CXXFLAGS = -std=c++20 -pthread -ggdb -O2 -W -Wall -iquote ../include -D_REENTRANT $(LARGE_FILE_FLAGS)
## CXXFLAGS = -std=c++2a -pthread -ggdb -O2 -W -Wall -iquote ../include -D_REENTRANT $(LARGE_FILE_FLAGS)
# CPPFLAGS = -iquote ../include -iquote .. -D_REENTRANT $(LARGE_FILE_FLAGS) -DHAVE_CONFIG_H -DDEBUG
CFLAGS = -g -O2 -W -Wall
# CFLAGS = -ggdb -O2 -W -Wall -DDEBUG
# CFLAGS = -g -O2 -Wall -DSG_KERNEL_INCLUDES
# CFLAGS = -g -O2 -Wall -pedantic
# CFLAGS = -Wall -W -pedantic -std=c11 --analyze
# CFLAGS = -Wall -W -pedantic -std=c++14 -fPIC
# CFLAGS = -Wall -W -pedantic -std=c++20

LDFLAGS =

all: $(EXECS)

extras: $(EXTRAS)



depend dep:
	for i in *.c; do $(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) -M $$i; \
	done > .depend

clean:
	/bin/rm -f *.o $(EXECS) $(EXTRAS) core .depend

renameat2: renameat2.o
	$(LD) -o $@ $(LDFLAGS) $^


install: $(EXECS)
	install -d $(INSTDIR)
	for name in $^; \
	 do install -s -o root -g root -m 755 $$name $(INSTDIR); \
	done
	install -d $(MANDIR)/$(MAN_PREF)
	for mp in $(MAN_PGS); \
	 do install -o root -g root -m 644 $$mp $(MANDIR)/$(MAN_PREF); \
	 gzip -9f $(MANDIR)/$(MAN_PREF)/$$mp; \
	done

uninstall:
	dists="$(EXECS)"; \
	for name in $$dists; do \
	 rm -f $(INSTDIR)/$$name; \
	done
	for mp in $(MAN_PGS); do \
	 rm -f $(MANDIR)/$(MAN_PREF)/$$mp.gz; \
	done

# Linux uses GNU make and FreeBSD uses Berkely make. The following lines
# only work in Linux. Possible solutions in FreeBSD:
#    a) use 'gmake'; b) comment out the next 3 lines, starting with 'ifeq'
#        c) build with 'make -f Makefile.freebsd'
# In Linux one can install bmake (but that won't help here).
ifeq (.depend,$(wildcard .depend))
include .depend
endif