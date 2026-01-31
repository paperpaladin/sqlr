# sqlr -- read-only SQLite 3 file parser. gcc + C11 + libm, nothing else.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
LDLIBS  := -lm

all: sqlr

sqlr: sqlr.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# The oracle. SQLite builds the databases and answers the queries; sqlr answers
# the same queries; the bytes must match exactly.
test: sqlr
	./t/run.sh

clean:
	rm -f sqlr
	rm -rf t/work

.PHONY: all test clean
