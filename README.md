# sqlr

A read-only SQLite 3 database file parser in one C file: it reads the on-disk
format directly -- header, b-tree pages, overflow chains, records, `sqlite_schema`
-- and answers `SELECT ... FROM table [WHERE col = value]` by full table scan,
without linking libsqlite3.

```
$ sqlr app.sqlite .dbinfo
page_size	4096
page_count	3
reserved_bytes	0
text_encoding	UTF-8
schema_cookie	2
user_version	0
freelist_pages	0

$ sqlr app.sqlite .tables
events
users

$ sqlr app.sqlite .schema
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, score REAL, avatar BLOB);
CREATE TABLE events(user INTEGER, kind TEXT, payload BLOB);

$ sqlr app.sqlite 'SELECT id, name, score FROM users WHERE score > 0.5'
1	alice	1.5
7	bob	0.75

$ sqlr --json app.sqlite 'SELECT name, avatar FROM users WHERE id = 1'
[{"name":"alice","avatar":"X'00FF00'"}]
```

## make test

```
$ make clean && make && make test
rm -f sqlr sqlr-asan
rm -rf t/work
cc -std=c11 -O2 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -o sqlr sqlr.c -lm
./t/run.sh
building corpus with SQLite 3.46.1 ...
20 databases, 89 comparison cases
robustness: truncating and corrupting ...

114 agree, 0 disagree
```

About 13 seconds. `make test-asan` runs the identical harness against a build
with `-fsanitize=address,undefined` and takes about a minute:

```
$ make test-asan
...
20 databases, 89 comparison cases
robustness: truncating and corrupting ...

114 agree, 0 disagree
```

## Why this exists

The SQLite file format is a short, stable, extremely well specified thing that
sits under a few billion devices, and reading it is a good way to find out
whether you actually understand it. Writing a parser also gives you something
useful: a way to pull rows out of a database file with no SQLite available, no
lock taken, and no possibility of modifying the file -- which is exactly the
situation you are in when the file is evidence, a backup, or something you found.

The interesting part is not the b-tree. It is the handful of rules that are
invisible from the outside and silently produce wrong answers if you get them
wrong:

- A record's header length is itself a varint, and the length it encodes includes
  that varint. You cannot find the first byte of row data without decoding it.
- Varints are big-endian with seven payload bits per byte, except the ninth byte,
  which contributes all eight. There is no continuation bit to check on byte 9.
- Serial types 8 and 9 are the literal integers 0 and 1 and occupy no bytes at
  all in the record body.
- How much of an oversized payload stays on the leaf page is not "as much as
  fits". It is one of two computed values, `M` or `K`, picked so that the last
  overflow page comes out as full as possible. `local_payload()` in `sqlr.c`
  spells out the arithmetic.
- An `INTEGER PRIMARY KEY` column is stored as NULL; its value is the cell's
  rowid.
- A column with REAL affinity may hold an **integer** serial type, because
  SQLite shrinks a float whose value is an exact integer and re-floats it on
  read. Nothing in the record says the value was ever a float.

Three of those were found or confirmed by the oracle disagreeing, not by reading
the spec more carefully.

## The oracle

**SQLite is the authority and the comparison is exact.** SQLite creates every
database in the corpus and inserts every row; each query is then answered twice,
once by SQLite and once by `sqlr`, and the two byte strings must be equal.
`t/run.sh` prints `N agree, M disagree` and exits nonzero if anything disagrees.

The harness pushes every formatting decision back into SQLite rather than
reimplementing it, so the comparison stays a real test: a REAL's text comes from
SQLite's own `CAST(x AS TEXT)` and a BLOB's hex from SQLite's own `hex()`. If
`put_real()` in `sqlr.c` renders `1.0` as `1`, the oracle says so -- and it did.

**One honest deviation from the brief:** the `sqlite3` command-line shell is not
installed in this environment (`sqlite3: command not found`; no package may be
installed). SQLite is therefore reached through python3's stdlib `sqlite3`
module, which is a binding to the same libsqlite3 -- version 3.46.1. The engine
and the authority are identical; only the front end differs. The practical cost
is that output formats cannot be inherited from the shell's `.mode tabs` and
`.mode json`, so `sqlr` defines them (see PLAN.md, "Contract") and the harness
computes the expected bytes from SQLite's values.

The corpus covers, deliberately, the cases that break naive parsers:

| what | why it breaks parsers |
| --- | --- |
| every `.dbinfo` field against the PRAGMA that reports the same number | the 100-byte header, checked field by field |
| 300 tables on 512-byte pages | `sqlite_schema` itself outgrows one page, so `.tables` needs the interior-page walk |
| one CREATE statement over 5 KB | `sqlite_schema`'s own record spills to an overflow page, so `.schema` needs the chain reader |
| a table with 0 rows | a leaf page with no cell pointer array |
| a table with 1 row | no interior page anywhere |
| NULL in every column position, and all-NULL rows | serial type 0 consumes no body bytes |
| 0 and 1 as integers | serial types 8 and 9 consume no body bytes |
| every integer width, each with its negative twin | sign extension from 1, 2, 3, 4, 6, 8 bytes |
| `-9223372036854775808` and `9223372036854775807` | the 9-byte varint and the int64 edges |
| REAL `0.0`, `-0.0`, `1.0`, `1/3`, `1e300`, `5e-324`, `2^53` | SQLite's float-to-text rules, and integral-REAL storage |
| BLOBs containing NUL, and all 256 byte values | length-driven parsing, not NUL-terminated |
| UTF-8 text including CJK and an emoji | byte-length vs character-length |
| text and blob lengths swept one byte at a time across the overflow threshold, at page sizes 512, 1024 and 4096 | the `X`/`M`/`K` branch in the local-payload calculation |
| a 200 KB value on 512-byte pages | a ~400-page overflow chain |
| 50,000 rows on 4 KiB pages | interior pages exist at all |
| **50,000 rows on 512-byte pages: a 4-level b-tree, 3,266 leaf and 66 interior pages** | an interior page whose children are also interior pages |
| rowids made non-contiguous by DELETE, plus negative and far-apart rowids | rowid is not the row's position |
| a table with no INTEGER PRIMARY KEY | the rowid is not in the record at all |
| `ALTER TABLE ADD COLUMN`, so old rows have short records | missing trailing columns read as NULL |
| page sizes 512, 1024, 8192, 65536 | 65536 is encoded in the header as the value `1` |
| a table with an index | sqlr must ignore it, not trip over it |
| quoted, bracketed and backticked identifiers; `VARCHAR(10,2)`; a `DEFAULT 'x,y)'`; table-level CHECK and UNIQUE | the CREATE-statement column splitter |
| text vs numeric literals in `WHERE` | SQLite's type-affinity conversion rules |

And the robustness half, where the claim is "does not misbehave" rather than
"matches SQLite", because SQLite's behaviour on a corrupt file is not a spec:

- three valid databases truncated at **every page boundary and one byte either
  side**, plus a spread of other offsets -- including a 512-byte-page file that is
  mostly one overflow chain, and the 4-level b-tree;
- 500 single-byte corruptions through the first pages, where page types, cell
  counts, cell pointers and varints live;
- hand-built hostile files: empty, 16 bytes, header-only, bad magic, zero and
  non-power-of-two page sizes, a UTF-16 encoding flag, a page count of 2^31-1,
  illegal payload fractions, all-zero pages, and **a b-tree root that is its own
  child** (caught by the visited-page bitmap, which is why a cyclic file
  terminates instead of looping);
- a check that the database file's SHA-256 is unchanged after reading it and that
  no `-journal`/`-wal`/`-shm` sidecar appeared;
- every one of the above rerun under ASan and UBSan by `make test-asan`.

## Status

Works, and is checked against SQLite on every run.

**What does not work -- the honest list:**

- **No indexes.** Index b-tree pages (types 2 and 10) are *refused with an
  error*, not misread. Every query is a full table scan, so a `WHERE` on a
  50,000-row table reads all 50,000 rows.
- **No `WITHOUT ROWID` tables.** They are index b-trees. sqlr detects them from
  the CREATE statement and errors out rather than printing something wrong.
- **No WAL replay.** A `-wal` file is ignored entirely. A cleanly closed
  WAL-mode database has been checkpointed and reads correctly (the harness
  asserts this), but a database with a live, un-checkpointed `-wal` reads as its
  last checkpoint -- **the one silent wrong-answer mode left in sqlr.** Fixing it
  is item 09 in PLAN.md.
- **No writes.** Not a limitation; it is the point.
- **No joins, no ORDER BY, no LIMIT, no aggregates, no AND/OR, no expressions,
  no `LIKE`, no `IN`.** One optional `WHERE` term comparing one column to one
  literal. Anything else is refused with a message, never silently ignored.
- **No views, triggers or virtual tables.** `.schema` prints their SQL because it
  prints every `sqlite_schema` row's SQL, but you cannot query them.
- **No UTF-16 databases.** Rejected at open time.
- **No `COLLATE`.** Text comparison is `memcmp`, i.e. BINARY. A column declared
  `COLLATE NOCASE` will compare case-sensitively, which is a wrong answer; the
  corpus does not use one.
- **No column DEFAULT values.** A column added by `ALTER TABLE ADD COLUMN ... 
  DEFAULT 5` reads as NULL in rows written before the ALTER, where SQLite reads
  5. The corpus adds columns without defaults.
- **`INTEGER PRIMARY KEY DESC` is not treated as a rowid alias**, which matches
  SQLite, but the detection is textual and would be confused by an unusual
  spelling.
- **The whole file is read into memory.** A 10 GB database needs 10 GB of RAM.
- **The CREATE-statement parser is a shape-matcher, not an SQL parser.** It
  handles everything SQLite itself emits, and the corpus pushes on it, but exotic
  DDL (generated columns, a DEFAULT containing an unbalanced quote) will confuse
  it.
- **TSV output is not escaped.** A text value containing a tab or a newline
  produces ambiguous TSV, exactly as `sqlite3 -tabs` does. Use `--json` for
  values like that; JSON output escapes control characters properly.
- **A BLOB and a text value spelling `X'41'` render identically.** Blobs use
  SQLite's `X'HEX'` literal form in both output modes.

Every deliberate shortcut is marked in the source with a `shortcut:` comment
naming its ceiling and its upgrade path; `grep -n 'shortcut:' sqlr.c` is the
short version of this list.

## Design

One file, about 1,250 lines, no dependency beyond libc and libm. `.dbinfo` is
answered from the header alone, before any b-tree is touched, so it still works
on a file whose pages are corrupt.

```
db_open()        validates the 100-byte header: magic, page size (with the
                 "1 means 65536" special case), reserved bytes, the three fixed
                 payload fractions, text encoding. The page count is taken from
                 the header only when it does not exceed what the file actually
                 contains, which is what turns a truncated file into "page N out
                 of range" instead of a read past the buffer.

varint()         the 1-9 byte big-endian integer, bounds-checked against the end
be_int()         n-byte two's complement, accumulated unsigned and sign-extended
                 at the end (shifting a negative value left is undefined)

walk()           recursive descent over table b-tree pages. Interior cells
                 (type 5) are visited in order and then the right-most pointer
                 from the page header, which is what keeps a scan in rowid order.
                 Page 1 is special-cased: its b-tree header is at offset 100 but
                 its cell pointers are still offsets from the page start.
                 A visited-page bitmap makes a cyclic file terminate; a depth cap
                 of 40 bounds the recursion.

local_payload()  the overflow threshold. See the comment; it is the one piece of
                 arithmetic in the format that cannot be inferred.
payload_read()   local bytes plus the overflow chain, concatenated into a reused
                 buffer. A payload claiming to be larger than the whole database
                 is rejected before anything is allocated.

rec_parse()      header-length varint, then one serial-type varint per column,
                 then bodies. Returns an array of tagged values pointing into the
                 payload.

schema_load()    walks page 1 (always the root of sqlite_schema) and reads
                 (type, name, tbl_name, rootpage, sql) from each row.
parse_create()   shape-matches the column list out of the CREATE statement to get
                 column names, declared types, each column's affinity, and which
                 column (if any) is the INTEGER PRIMARY KEY rowid alias.

query_parse()    the tiny grammar, resolved against the table's columns.
emit_row()       a callback invoked once per row by walk(): realify REAL-affinity
                 integers, substitute the rowid for the INTEGER PRIMARY KEY,
                 pad short records with NULL, apply the WHERE predicate, print.
```

Everything flows through one callback, so there is no row buffer and memory use
is independent of the result size. Errors are a string in a static buffer plus a
`-1` return, propagated to `main`, printed once on stderr.

## License

GPL-3. See `LICENSE`.
