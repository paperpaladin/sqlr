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

# The same harness against a sanitized build. This is the target that matters:
# sqlr parses untrusted binary input, so every truncated and corrupted file in
# the harness is run through ASan and UBSan looking for a bad read.
# shortcut: leak detection is off. The process parses one query and exits, so
# the schema and query allocations are deliberately never freed; the upgrade
# path is a freeing teardown, not an arena.
#
# halt_on_error=1, and it is not cosmetic. UBSan reports and CONTINUES by
# default, so a finding reaches stderr while the process exits 0 with the
# right rows on stdout. The truncation and corruption sweeps grep stderr for
# "runtime error" and would still catch it; the comparison cases compare
# stdout and the exit status and would not. Measured with a planted left-shift
# of a negative value in emit_row: halt_on_error=0 gives exit 0 and the
# correct answer beside the diagnostic, halt_on_error=1 gives exit 1.
test-asan: sqlr-asan
	SQLR=./sqlr-asan ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./t/run.sh

sqlr-asan: sqlr.c
	$(CC) -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -o $@ $< $(LDLIBS)

clean:
	rm -f sqlr sqlr-asan
	rm -rf t/work*

.PHONY: all test test-asan clean
