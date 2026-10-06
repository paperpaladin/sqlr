# sqlr -- read-only SQLite 3 file parser. gcc + C11 + libm, nothing else.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
LDLIBS  := -lm

all: sqlr

sqlr: sqlr.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f sqlr

.PHONY: all clean
