# sqlr: plan

## Contract

`sqlr` reads a SQLite 3 database file and answers a small set of questions about
it, without linking libsqlite3 and without ever writing a byte.

    sqlr DB.sqlite .dbinfo            # header fields, one per line, as name<TAB>value
    sqlr DB.sqlite .tables            # table names, one per line, sorted
    sqlr DB.sqlite .schema            # each CREATE statement, semicolon-terminated
    sqlr [--json] DB.sqlite 'SELECT'  # rows, as TSV (default) or JSON

The query grammar is exactly:

    SELECT <* | col[,col]...> FROM <table>
           [WHERE <col> {= == != <> < <= > >=} <literal> | <col> IS [NOT] NULL]

Literals are integers, reals, `'text'` (with `''` escapes), `X'hex'` and `NULL`.
`.dbinfo` reports page size, page count, reserved bytes, text encoding, schema
cookie, user version and freelist page count, every one of which SQLite also
reports through a PRAGMA, so the header parse is checked field by field.

`rowid`, `_rowid_` and `oid` are selectable and usable in `WHERE`. Everything
else (joins, ORDER BY, LIMIT, aggregates, AND/OR, expressions) is refused with
a message on stderr and exit 1.

Exit status: 0 success, 1 error (unreadable file, unsupported query), 2 usage.

Output contract, so that comparison can be byte-exact: TSV uses a tab between
fields and a newline between rows, NULL is the empty field, a BLOB renders as
SQLite's own `X'HEX'` literal, and a REAL renders the way SQLite renders it
(`%.15g`, then a `.0` forced into the mantissa if there is no `.`, so 1.0 and
1.0e+300, not 1 and 1e+300). JSON is `[{...},\n{...}]` with NULL as
`null`.

## Hard invariant

**For every database and every query in the corpus, sqlr's stdout is
byte-identical to the answer SQLite gives for the same query, and malformed
input produces an error, never a crash, a hang, or a wrong row.**

Both halves are enforced by `make test`, which prints `N agree, M disagree` and
exits nonzero if M is not zero. The second half is checked by truncating three
valid databases at every page boundary and at a spread of other offsets,
flipping single bytes through the first pages, and feeding in hand-built hostile
files including a b-tree root that is its own child; the only acceptable
outcomes are exit 0 or exit 1 with no sanitizer report. `make test-asan` runs the
identical harness under `-fsanitize=address,undefined`.

Secondary invariant: **sqlr never writes to a database.** The file is opened
`"rb"`, and the harness checks the SHA-256 of a corpus database before and after
reading it, and that no `-journal`, `-wal` or `-shm` sidecar appeared.

## Oracle

Ranked, strongest first.

1. SQLite itself, on exactly the same query. `t/oracle.py` has SQLite create
   every database and insert every row, then answers each query twice, once
   through SQLite, once through `sqlr`, and compares the bytes. Every rendering
   decision that could hide a sqlr bug is pushed back into SQLite: REAL text
   comes from SQLite's `CAST(x AS TEXT)` and blob hex from SQLite's `hex()`, so
   `put_real()` and `put_blob()` are checked against SQLite's own
   formatting, not against a Python reimplementation of it. 89 comparison cases over
   20 databases.

   The `sqlite3` command-line shell is not installed in this environment, so
   SQLite is reached through python3's stdlib binding to the same libsqlite3
   (3.46.1). Same engine, same authority, different front end. This is the one
   honest deviation from the brief and it is noted in the README too.

2. The robustness sweep. Truncation at every page boundary of three
   databases, 500 single-byte corruptions, and hand-built degenerate files. The
   oracle here is "does not misbehave", not "matches SQLite", because
   SQLite's own behaviour on a corrupt file is not a specification.

3. ASan + UBSan over the whole harness (`make test-asan`). This is what
   found the undefined left-shift of a negative value in `be_int()`.

4. valgrind on a handful of invocations, as a spot check that the clean
   build has no invalid read the sanitizers missed.

## Non-goals

Writing, in any form. Indexes, every query is a full table scan, and an index
b-tree page is refused, not misread. WAL replay: a `-wal` file is ignored,
so an un-checkpointed database reads as its last checkpoint. `WITHOUT ROWID`
tables, which are index b-trees. UTF-16 databases. Joins, aggregates, ORDER BY,
LIMIT, expressions, more than one WHERE term. Being fast. Being a library.

## Commit sequence

- [x] 00 the 100-byte header, and `.dbinfo`
      files: sqlr.c, Makefile, LICENSE, .gitignore
      why: every later layer trusts the page size, the reserved-bytes count and
           the page count, and the page count is the bound that keeps a
           truncated file from being read past its end
- [x] 01 varints, serial types, the record format, table leaf pages,
      sqlite_schema, `.tables` and `.schema`
      why: the smallest slice that answers a real question about a real file;
           the record encoding is the part of the format nobody guesses
- [x] 02 table interior pages: b-tree descent, a visited-page bitmap, depth cap
      why: directly observable, not just plumbing: a database with 300 tables
           has a sqlite_schema that itself needs an interior page, so `.tables`
           was already wrong without this
- [x] 03 overflow pages: the local-payload threshold and chain assembly
      why: also observable through `.schema` alone, because a CREATE statement
           over ~4 KB spills its sqlite_schema record to an overflow page
- [x] 04 CREATE-statement column parsing, the INTEGER PRIMARY KEY rowid alias,
      and `SELECT <*|cols> FROM t` as TSV
      why: a rowid alias is stored as NULL, so without this every id column in
           every database reads as empty
- [x] 05 `WHERE`: operators, literals, `IS [NOT] NULL`, and type affinity
      why: affinity is applied to the literal, not the stored value, and getting
           that backwards makes `WHERE i = '5'` silently wrong
- [x] 06 `--json` output
      why: TSV cannot represent a text value containing a tab or a newline, and
           the corpus has those
- [x] 07 the oracle harness: SQLite builds the corpus and answers every query
      files: t/oracle.py, t/run.sh, Makefile
      why: a parser of someone else's format is only as good as its comparison
           against the reference implementation; without this it is a
           plausible-looking program with unknown bugs
- [x] 08 fix: realify integers stored in REAL-affinity columns
      why: the oracle's first finding. SQLite shrinks a float whose value is an
           exact integer to an integer serial type and re-floats it on read, so
           sqlr printed `1` where sqlite3 prints `1.0`. Reverting just the
           sqlr.c half of this commit reproduces the disagreement
- [x] 09 corpus: 50,000 rows, and the same rows on 512-byte pages
      why: 50,000 rows on 4 KiB pages is only a two-level tree; on 512-byte
           pages it is four levels, 3,266 leaf and 66 interior pages, which is
           what actually exercises an interior page whose children are interior
- [x] 10 corpus: the overflow threshold swept one byte at a time
      why: the local-payload calculation has two branches and a threshold, and
           only a sweep across the boundary proves both branches
- [x] 11 corpus: rowid gaps, ALTER TABLE, page sizes, indexes, loud refusals,
      and the read-only assertion
      why: "never writes" has to be asserted, not asserted-to; and the things
           sqlr refuses have to be shown refusing
- [x] 12 fix: a `)` closing a nested type group ended the column list
      why: the oracle's second finding, and the worse of the two: `VARCHAR(10,2)`
           made the splitter stop after the first column, so `SELECT *` returned
           one column of four. A wrong answer, not an error
- [x] 13 fix: undefined left-shift of a negative value in `be_int`
      why: `(int8_t)p[0] << 8` is undefined for a negative first byte. Found by
           hand with `-fsanitize=undefined` against a truncated database; the
           next commit makes it reproducible
- [x] 14 the robustness sweep, and `make test-asan`
      why: a parser fed untrusted bytes has to be shown terminating on them,
           and the sanitizers are the only thing that can say the bounds checks
           are right, not merely present
- [x] 15 PLAN.md
- [x] 16 README.md, with an honest list of what does not work

### Milestone 1: read more of the format

- [ ] 06 index b-trees: decode page types 2 and 10, so `.indexes` and an
      index-covered scan become possible
      why: the index is half the file format and sqlr currently refuses it; this
           is the largest remaining gap between sqlr and "reads a SQLite file"
- [ ] 07 use an index for an equality `WHERE` when one exists, falling back to
      the scan, and add a harness case asserting the two paths agree with each
      other as well as with SQLite
      why: the only way to know a seek is right is to diff it against the scan
           that already matches SQLite
- [ ] 08 `WITHOUT ROWID` tables, which fall out of 06. They are index b-trees
      with the whole row as the key
      why: refusing them is honest but they are increasingly common
- [ ] 09 read `-wal` files: parse the WAL header and frame index, and prefer a
      committed frame over the page in the main file
      why: an un-checkpointed database currently reads as stale data, and that
           is the one failure mode that is silent, not loud
- [ ] 10 a real expression evaluator: tokenise once, build a tree, support
      AND/OR/NOT, parentheses, `LIKE`, `IN`, arithmetic, and `COLLATE NOCASE`
      why: `WHERE` is one term today, the limit people hit first; the
           scan loop already calls a predicate once per row, so the shape is right
- [ ] 11 `ORDER BY` and `LIMIT`, by buffering and sorting the selected rows
      why: without them the 50,000-row cases can only be compared in full
- [ ] 12 aggregates: `COUNT(*)`, `MIN`, `MAX`, `SUM`, `AVG`, and `GROUP BY`
      why: `COUNT(*)` is the first thing anyone types at a database file

### Milestone 2: prove it against the wild

- [ ] 13 run against a large real-world `.sqlite` file (a Firefox `places.sqlite`,
      a Chrome `History`) and diff every table against SQLite, not just the
      synthetic corpus
      why: a corpus written by the same person who wrote the parser shares its
           blind spots; real files have ALTER TABLE history, views, triggers,
           partial indexes and freelist pages
- [ ] 14 fuzz the page parser: a persistent-mode libFuzzer or AFL++ harness over
      `db_open` + a full scan, with the corpus as the seed set
      why: 500 single-byte flips is a sweep, not a fuzz run; coverage-guided
           mutation is how the remaining bad reads get found
- [ ] 15 check SQLite's own `PRAGMA integrity_check` findings against sqlr's
      error messages on the same corrupted files
      why: "errors cleanly" is a weak claim; "errors on exactly the files SQLite
           calls corrupt" is a strong one
- [ ] 16 extend `.dbinfo` to walk the freelist and the pointer-map pages and
      report them, so the whole file is accounted for page by page
      why: `.dbinfo` reports the freelist page *count* from the header but never
           follows the trunk pages, so a file whose freelist is corrupt looks
           fine; accounting for every page is also how you find the pages sqlr
           never visits
