# sqlr

A read-only SQLite 3 database file parser in one C file. It reads the on-disk
format directly, header, table and index b-tree pages, overflow chains,
records, `sqlite_schema`, and answers `SELECT ... FROM table [WHERE col =
value]` by full table scan, without linking libsqlite3.

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
CREATE INDEX ix_users_score ON users(score);

$ sqlr app.sqlite 'SELECT id, name, score FROM users WHERE score > 0.5'
1	alice	1.5
7	bob	0.75

$ sqlr --json app.sqlite 'SELECT name, avatar FROM users WHERE id = 1'
[{"name":"alice","avatar":"X'00FF00'"}]

$ sqlr app.sqlite .indexes
ix_users_score

$ sqlr app.sqlite .index ix_users_score
0.75	7
1.5	1
```

`.index` prints the b-tree itself, in key order: the indexed columns, then the
rowid of the row each entry points at. Nothing yet uses an index to answer a
`SELECT`; that is the next item in `PLAN.md`.

## make test

```
$ make clean && make && make test
rm -f sqlr sqlr-asan
rm -rf t/work
cc -std=c11 -O2 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -o sqlr sqlr.c -lm
./t/run.sh
building corpus with SQLite 3.46.1 ...
23 databases, 113 comparison cases
robustness: truncating and corrupting ...

171 agree, 0 disagree
```

About 25 seconds. `make test-asan` runs the identical harness against a build
with `-fsanitize=address,undefined` and takes about 85 seconds:

```
$ make test-asan
...
23 databases, 113 comparison cases
robustness: truncating and corrupting ...

171 agree, 0 disagree
```

## Why this exists

The SQLite file format is a short, stable, extremely well specified thing that
sits under a few billion devices, and reading it is a good way to find out
whether you actually understand it. Writing a parser also gives you something
useful: a way to pull rows out of a database file with no SQLite available, no
lock taken, and no possibility of modifying the file, exactly the
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
- That threshold is not the same on an index page as on a table leaf. A table
  leaf keeps up to `usable - 35` bytes; an index page, interior or leaf, keeps
  up to `((usable - 12) * 64 / 255) - 23`, which on a 512-byte page is 102
  bytes against 477. The same 300-byte string therefore sits whole on the
  table page and spills from the index page pointing at it.
- An index interior cell carries a key as well as a child pointer, and that
  key sorts after everything in the child. A table interior cell carries only
  a rowid, so for tables the traversal order is just the order of the
  descents; for indexes the cell's own entry has to be emitted between the
  child it points at and the next one.
- An `INTEGER PRIMARY KEY` column is stored as NULL. Its value is the cell's
  rowid.
- A column with REAL affinity may hold an integer serial type, because
  SQLite shrinks a float whose value is an exact integer and re-floats it on
  read. Nothing in the record says the value was ever a float.

Three of those were found or confirmed by the oracle disagreeing, not by reading
the spec more carefully. The two index rules were not: they came from the spec,
and the oracle's part was to show they had been read correctly.

## The oracle

SQLite is the authority and the comparison is exact. SQLite creates every
database in the corpus and inserts every row. Each query is then answered twice,
once by SQLite and once by `sqlr`, and the two byte strings must be equal.
`t/run.sh` prints `N agree, M disagree` and exits nonzero if anything disagrees.

An index is compared the same way, and the hard part is the expected order.
It is not an `ORDER BY` written out by hand: `INDEXED BY` makes SQLite
traverse that index and emit its entries in the order they are stored, which
is the same walk `sqlr` does over the same pages, and `EXPLAIN QUERY PLAN` is
checked first to confirm SQLite really answered that way rather than by
sorting the table. The corpus holds a `COLLATE NOCASE` index and a `DESC`
index for exactly that reason: both store their keys in an order a
hand-written `ORDER BY` is likely to get wrong, and under this oracle neither
needed to be reasoned about at all.

The harness pushes every formatting decision back into SQLite,
not reimplementing it, so the comparison stays a real test. A REAL's text comes from
SQLite's own `CAST(x AS TEXT)` and a BLOB's hex from SQLite's own `hex()`. If
`put_real()` in `sqlr.c` renders `1.0` as `1`, the oracle says so, and it did.

One honest deviation from the brief: the `sqlite3` command-line shell is not
installed in this environment (`sqlite3: command not found`. No package may be
installed). SQLite is therefore reached through python3's stdlib `sqlite3`
module, a binding to the same libsqlite3, version 3.46.1. The engine
and the authority are identical; only the front end differs. In practice the cost is that output formats cannot be inherited from the shell's `.mode tabs` and
`.mode json`, so `sqlr` defines them (see PLAN.md, "Contract") and the harness
computes the expected bytes from SQLite's values.

The corpus covers, deliberately, the cases that break naive parsers:

| what | why it breaks parsers |
| --- | --- |
| six of the seven `.dbinfo` fields against the PRAGMA that reports the same number, and `reserved_bytes` against byte 20 of the file, since no PRAGMA reads it back | the 100-byte header, checked field by field |
| 32 reserved bytes per page, on 512-byte pages, with values long enough to overflow | reserved bytes shrink the usable page, so every overflow threshold and the end of the cell content area move with them |
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
| **50,000 rows on 512-byte pages: a 4-level b-tree, 3,266 leaf and 66 interior pages** (asserted against SQLite's `dbstat`, not just described) | an interior page whose children are also interior pages |
| rowids made non-contiguous by DELETE, plus negative and far-apart rowids | rowid is not the row's position |
| a table with no INTEGER PRIMARY KEY | the rowid is not in the record at all |
| `ALTER TABLE ADD COLUMN`, so old rows have short records | missing trailing columns read as NULL |
| page sizes 512, 1024, 8192, 65536 | 65536 is encoded in the header as the value `1` |
| a table with a 4-entry index | the whole index b-tree is one leaf page, so there is no interior page at all |
| **eight indexes over 2,000 rows, every one asserted through `dbstat` to have an INTERIOR page** | index interior cells carry a key as well as a child pointer, and a corpus of single-page indexes never reaches that format |
| indexes on one column, on two columns, UNIQUE, `COLLATE NOCASE`, `DESC`, on a REAL column, on a BLOB column, and partial (`WHERE salary > 100`) | the stored order is not the table's order, and for NOCASE and DESC it is not byte order either; a REAL column stores an integral value as an integer in the index exactly as it does in the table |
| NULLs in an indexed column | NULL sorts before everything in an index |
| 300-byte keys on 512-byte pages, with the index root's own cells overflowing too | the index local-payload threshold is a different formula from the table one, and an interior cell's key overflows like any other |
| `sqlite_autoindex_u_1`, from a UNIQUE constraint | its `sqlite_schema` row has no SQL at all, so its key columns cannot be read from a statement |
| a table with an index | a plain `SELECT` must still scan and ignore it |
| quoted, bracketed and backticked identifiers; `VARCHAR(10,2)`; a `DEFAULT 'x,y)'`; table-level CHECK and UNIQUE | the CREATE-statement column splitter |
| text vs numeric literals in `WHERE` | SQLite's type-affinity conversion rules |

And the robustness half, where the claim is "does not misbehave",
not "matches SQLite", because SQLite's behaviour on a corrupt file is not a spec:

- four valid databases truncated at up to 1,400 offsets each, chosen as
  every page boundary and one byte either side plus every offset inside the
  100-byte header, including a 512-byte-page file that is mostly one overflow
  chain, and the 4-level b-tree. The small fixture gets every candidate offset;
  the two large ones generate more candidates than the budget (deep.db offers
  10,274), so they get an even sample of them **spread across the whole file**.
  That spread is the point: the budget used to be spent on the lowest offsets,
  which reached 13% of deep.db and 49% of the overflow-chain file, so neither
  sweep ever cut the interior pages or the chain tail it existed for. The
  fourth is the index fixture, cut while `.index` walks index interior pages,
  index leaves and the overflow chains hanging off both;
- 500 single-byte corruptions through the first pages of a table-only
  database, where page types, cell counts, cell pointers and varints live,
  and 300 more anywhere in the index fixture, answered with `.index`;
- hand-built hostile files: empty, 16 bytes, header-only, bad magic, zero and
  non-power-of-two page sizes, a UTF-16 encoding flag, a page count of 2^31-1,
  illegal payload fractions, all-zero pages, and **a b-tree root that is its own
  child** (caught by the visited-page bitmap, so a cyclic file
  terminates instead of looping);
- three more built by corrupting the index fixture at a known offset: an index
  root whose page type byte claims to be a table leaf, a table root whose page
  claims to be an index leaf, and an index interior page made its own
  right-most child. The first two must report the mismatch rather than decode
  a child pointer as a payload length. The third is what pays for the two
  separate visited-page bitmaps: with one shared bitmap it reports "deeper
  than 40 levels" after emitting the same pages forty times, because reading
  an overflowing key wiped the record of every page already visited;
- a check that the database file's SHA-256 is unchanged after reading it and that
  no `-journal`/`-wal`/`-shm` sidecar appeared;
- every one of the above rerun under ASan and UBSan by `make test-asan`.

## Status

Works, and is checked against SQLite on every run.

What does not work: the honest list:

- No index is ever used to answer a query. Index b-trees are read and can be
  dumped with `.index`, but every `SELECT` is still a full table scan, so a
  `WHERE` on a 50,000-row table reads all 50,000 rows even when an index
  would answer it in three page reads. That is item 07 in PLAN.md.
- No `WITHOUT ROWID` tables, and no indexes on them. Such an index ends each
  entry with the table's primary key columns instead of a rowid, so dumping
  one would print something that looks right and is not. Both the table and
  its indexes are refused, including the auto-index a UNIQUE constraint
  creates, which has no CREATE statement of its own and so has to be refused
  through its table.
- `.index` has no affinity information for an index SQLite wrote for itself
  (`sqlite_autoindex_*`, whose `sqlite_schema` row carries no SQL) or for an
  index over an expression rather than a column. The only visible effect is
  on a REAL column: an integral value stored as an integer serial type would
  print as `0` where SQLite prints `0.0`. No auto-index in the corpus covers
  a REAL column, so this is a known gap and not a tested one.
- `.index` ignores `--json` and always prints TSV, and it prints the entry's
  columns without naming them, because an index entry's column names are not
  in the record and an expression index has none.
- `.indexes` lists the auto-indexes alongside the named ones. The sqlite3
  shell is not installed here, so there is no shell behaviour to match; the
  oracle is the same question put to `sqlite_schema`.
- No WAL replay. A `-wal` file is ignored entirely. A cleanly closed
  WAL-mode database has been checkpointed and reads correctly (the harness
  asserts this), but a database with a live, un-checkpointed `-wal` reads as its
  last checkpoint, the one silent wrong-answer mode left in sqlr. Fixing it
  is item 09 in PLAN.md.
- No writes. Not a limitation. It is the point.
- No joins, no ORDER BY, no LIMIT, no aggregates, no AND/OR, no expressions,
  no `LIKE`, no `IN`. One optional `WHERE` term comparing one column to one
  literal. Anything else is refused with a message, never silently ignored.
- No views, triggers or virtual tables. `.schema` prints their SQL because it
  prints every `sqlite_schema` row's SQL, but you cannot query them.
- No UTF-16 databases. Rejected at open time.
- No `COLLATE` in a `WHERE`. Text comparison is `memcmp`, i.e. BINARY. A
  column declared `COLLATE NOCASE` compares case-sensitively, and that is a
  wrong answer. A `COLLATE NOCASE` *index* dumps correctly, because the order
  is the file's and not something sqlr computes.
- No column DEFAULT values. A column added by `ALTER TABLE ADD COLUMN ... 
  DEFAULT 5` reads as NULL in rows written before the ALTER, where SQLite reads
  5. The corpus adds columns without defaults.
- `INTEGER PRIMARY KEY DESC` is not treated as a rowid alias, which matches
  SQLite, but the detection is textual and would be confused by an unusual
  spelling.
- The whole file is read into memory. A 10 GB database needs 10 GB of RAM.
- The CREATE-statement parser is a shape-matcher, not an SQL parser. It
  handles everything SQLite itself emits, and the corpus pushes on it, but exotic
  DDL (generated columns, a DEFAULT containing an unbalanced quote) will confuse
  it.
- TSV output is not escaped. A text value containing a tab or a newline
  produces ambiguous TSV, exactly as `sqlite3 -tabs` does. Use `--json` for
  values like that; JSON output escapes control characters properly.
- A BLOB and a text value spelling `X'41'` render identically. Blobs use
  SQLite's `X'HEX'` literal form in both output modes.

Every deliberate shortcut is marked in the source with a `shortcut:` comment
naming its ceiling and its upgrade path; `grep -n 'shortcut:' sqlr.c` is the
short version of this list.

### One surviving mutation

Twelve defects were planted in the index code and the suite had to go red for
each. Eleven did. The survivor is marking an overflow page into the b-tree
walk's visited bitmap instead of the overflow chain's own: it still clears
the right bitmap, so the cycle checks both still fire, and the only extra
effect is marking pages the walk has not visited. On a valid database an
overflow page is never also a b-tree page, so nothing changes; on a corrupt
one it could report a cycle that is a page-number collision rather than a
real loop, which is still an error on a file that is still corrupt. Nothing
in the corpus distinguishes the two, and a fixture built to distinguish them
would be asserting which of two error messages a corrupt file produces.

## Design

One file, about 1,450 lines, no dependency beyond libc and libm. `.dbinfo` is
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

walk()           recursive descent over b-tree pages, table (types 5 and 13)
                 and index (types 2 and 10) alike, told which kind it is
                 walking so that a rootpage pointing at the other kind is an
                 error rather than a misread. Interior cells are visited in
                 order and then the right-most pointer from the page header,
                 which is what keeps a scan in key order. An index interior
                 cell also carries a key, emitted after its left child and
                 before the next cell. Page 1 is special-cased: its b-tree
                 header is at offset 100 but its cell pointers are still
                 offsets from the page start. A visited-page bitmap makes a
                 cyclic file terminate; a depth cap of 40 bounds the recursion.

local_payload()  the overflow threshold, which is one formula for a table leaf
                 and a different one for an index page. See the comment; it is
                 the one piece of arithmetic in the format that cannot be
                 inferred.
payload_read()   local bytes plus the overflow chain, concatenated into an
                 exactly sized buffer -- exactly sized because a buffer larger
                 than its data hides an over-read from ASan. The chain has its
                 own visited-page bitmap, separate from the walk's, because it
                 is followed in the middle of one. A payload claiming to be
                 larger than the whole database is rejected before anything is
                 allocated.

rec_parse()      header-length varint, then one serial-type varint per column,
                 then bodies. Returns an array of tagged values pointing into the
                 payload.

schema_load()    walks page 1 (always the root of sqlite_schema) and reads
                 (type, name, tbl_name, rootpage, sql) from each row.
parse_create()   shape-matches the column list out of the CREATE statement to get
                 column names, declared types, each column's affinity, and which
                 column (if any) is the INTEGER PRIMARY KEY rowid alias.

index_affinities()
                 shape-matches the column list out of a CREATE INDEX statement
                 and maps each name onto its table column, which is the only
                 way to know that an index entry's integer belongs in a REAL
                 column. It is also where an index on a WITHOUT ROWID table is
                 refused, since that is a property of the table, not of the
                 index.
index_row()      a callback invoked once per index entry: the indexed columns,
                 then the rowid the entry points at.

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
