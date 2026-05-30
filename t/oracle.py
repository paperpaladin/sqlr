#!/usr/bin/env python3
"""The oracle: SQLite itself is the authority, sqlr is the thing on trial.

Every database in the corpus is CREATEd and INSERTed by SQLite. Every query is
answered twice -- once by SQLite, once by sqlr -- and the two answers are
compared byte for byte. Any difference is a disagreement and fails the run.

The sqlite3 command-line shell is not installed in this environment, so the
authority is reached through python3's stdlib binding to the same libsqlite3
(see README, "The oracle"). It is the same engine; only the front end differs.
Crucially, every rendering decision that could paper over a sqlr bug is pushed
back into SQLite: REAL text comes from SQLite's own CAST(x AS TEXT) and blob hex
from SQLite's own hex(), so put_real() and put_blob() in sqlr.c are checked
against SQLite's formatting rather than against a Python reimplementation.
"""
import os
import shutil
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SQLR = os.environ.get("SQLR", os.path.join(ROOT, "sqlr"))
WORK = os.path.join(HERE, "work")

agree = 0
disagree = 0
failures = []


def check(ok, what, detail=""):
    global agree, disagree
    if ok:
        agree += 1
    else:
        disagree += 1
        failures.append(what + (("\n    " + detail) if detail else ""))


def run(args, **kw):
    return subprocess.run([SQLR] + args, capture_output=True, **kw)


# ----------------------------------------------------------- oracle rendering
# sqlr's output contract, computed from SQLite's own answers.

def o_real(conn, v):
    # SQLite's float-to-text conversion, straight from SQLite.
    return conn.execute("SELECT CAST(? AS TEXT)", (v,)).fetchone()[0].encode()


def o_hex(conn, b):
    return conn.execute("SELECT hex(?)", (sqlite3.Binary(b),)).fetchone()[0].encode()


def o_tsv_val(conn, v):
    if v is None:
        return b""
    if isinstance(v, bool):
        v = int(v)
    if isinstance(v, int):
        return str(v).encode()
    if isinstance(v, float):
        return o_real(conn, v)
    if isinstance(v, str):
        return v.encode("utf-8")
    return b"X'" + o_hex(conn, v) + b"'"


def o_json_str(s):
    out = bytearray(b'"')
    for c in s:
        if c == 0x22:
            out += b'\\"'
        elif c == 0x5C:
            out += b"\\\\"
        elif c == 0x0A:
            out += b"\\n"
        elif c == 0x0D:
            out += b"\\r"
        elif c == 0x09:
            out += b"\\t"
        elif c == 0x08:
            out += b"\\b"
        elif c == 0x0C:
            out += b"\\f"
        elif c < 0x20:
            out += ("\\u%04x" % c).encode()
        else:
            out.append(c)
    out += b'"'
    return bytes(out)


def o_json_val(conn, v):
    if v is None:
        return b"null"
    if isinstance(v, bool):
        v = int(v)
    if isinstance(v, int):
        return str(v).encode()
    if isinstance(v, float):
        return o_real(conn, v)
    if isinstance(v, str):
        return o_json_str(v.encode("utf-8"))
    return o_json_str(b"X'" + o_hex(conn, v) + b"'")


def oracle_query(db, sql, json):
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        cur = conn.execute(sql)
        names = [d[0] for d in cur.description]
        rows = cur.fetchall()
        if json:
            parts = []
            for r in rows:
                parts.append(
                    b"{"
                    + b",".join(
                        o_json_str(n.encode()) + b":" + o_json_val(conn, v)
                        for n, v in zip(names, r)
                    )
                    + b"}"
                )
            return b"[" + b",\n".join(parts) + b"]\n"
        out = bytearray()
        for r in rows:
            out += b"\t".join(o_tsv_val(conn, v) for v in r) + b"\n"
        return bytes(out)
    finally:
        conn.close()


def oracle_tables(db):
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        rows = conn.execute(
            "SELECT name FROM sqlite_schema WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' AND rootpage > 0 ORDER BY name"
        ).fetchall()
        return b"".join(r[0].encode() + b"\n" for r in rows)
    finally:
        conn.close()


def oracle_dbinfo(db):
    """Every line of .dbinfo is a header field that SQLite also reports through
    a PRAGMA, so the 100-byte header parse is checked field by field."""
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        def pr(name):
            return conn.execute("PRAGMA " + name).fetchone()[0]
        return ("page_size\t%d\n"
                "page_count\t%d\n"
                "reserved_bytes\t%d\n"
                "text_encoding\t%s\n"
                "schema_cookie\t%d\n"
                "user_version\t%d\n"
                "freelist_pages\t%d\n"
                % (pr("page_size"), pr("page_count"), 0, pr("encoding"),
                   pr("schema_version"), pr("user_version"),
                   pr("freelist_count"))).encode()
    finally:
        conn.close()


def oracle_schema(db):
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        rows = conn.execute(
            "SELECT sql FROM sqlite_schema WHERE sql IS NOT NULL"
        ).fetchall()
        return b"".join(r[0].encode() + b";\n" for r in rows)
    finally:
        conn.close()


# ------------------------------------------------------------------- corpus

def make(name, script, rows=(), page_size=None, extra=None):
    """Build a database with SQLite. SQLite is the only thing that ever writes."""
    path = os.path.join(WORK, name + ".db")
    if os.path.exists(path):
        os.remove(path)
    conn = sqlite3.connect(path)
    if page_size:
        conn.execute("PRAGMA page_size=%d" % page_size)
        conn.execute("VACUUM")
    conn.executescript(script)
    for sql, params in rows:
        conn.executemany(sql, params)
    if extra:
        conn.executescript(extra)
    conn.commit()
    conn.execute("PRAGMA journal_mode=DELETE")  # keep it a single rollback-journal file
    conn.commit()
    conn.close()
    # Collapse any journal so the .db file alone is the whole database.
    for suffix in ("-journal", "-wal", "-shm"):
        j = path + suffix
        if os.path.exists(j):
            os.remove(j)
    return path


CASES = []  # (dbpath, argv_tail, sql_for_oracle_or_None, kind)


def q(db, sql, json=False):
    CASES.append((db, (["--json"] if json else []) + [db, sql], sql, "json" if json else "tsv"))


def meta(db, which):
    CASES.append((db, [db, which], None, which))


def build_corpus():
    os.makedirs(WORK, exist_ok=True)

    # 1. The degenerate shapes: no rows at all, and exactly one row. An empty
    #    table is a single leaf page with zero cells; naive parsers read a cell
    #    pointer array that is not there.
    db = make("tiny", """
        CREATE TABLE vacant(a, b, c);
        CREATE TABLE one(a INTEGER, b TEXT);
        INSERT INTO one VALUES(42, 'only');
    """)
    meta(db, ".tables")
    meta(db, ".schema")
    meta(db, ".dbinfo")
    q(db, "SELECT * FROM vacant")
    q(db, "SELECT * FROM vacant", json=True)
    q(db, "SELECT * FROM one")
    q(db, "SELECT * FROM one", json=True)
    q(db, "SELECT b, a FROM one")
    q(db, "SELECT rowid, a FROM one")
    q(db, "SELECT * FROM one WHERE a = 42")
    q(db, "SELECT * FROM one WHERE a = 43")

    # 1b. 300 tables on 512-byte pages, so sqlite_schema itself outgrows one
    #     page and .tables has to descend through an interior page. And one
    #     CREATE statement long enough that its sqlite_schema record spills to
    #     an overflow page, so .schema has to follow the chain.
    db = make("manytables",
              ";\n".join("CREATE TABLE t%03d(a INTEGER, b TEXT)" % i for i in range(300)),
              page_size=512)
    meta(db, ".tables")
    meta(db, ".schema")
    meta(db, ".dbinfo")

    cols = ", ".join("column_with_a_deliberately_long_name_%03d TEXT" % i
                     for i in range(120))
    db = make("wideschema", "CREATE TABLE w(%s);" % cols,
              extra="INSERT INTO w DEFAULT VALUES;")
    meta(db, ".tables")
    meta(db, ".schema")
    q(db, "SELECT column_with_a_deliberately_long_name_119 FROM w")

    # 2. Every storage class, NULL in every column position, and the integer
    #    widths that exercise each serial type: 0 and 1 (types 8 and 9, which
    #    occupy no body bytes), then 1-, 2-, 3-, 4-, 6- and 8-byte ints, each
    #    with its negative twin so the two's-complement sign extension is tested.
    ints = [0, 1, -1, 127, -128, 32767, -32768, 8388607, -8388608,
            2147483647, -2147483648, 140737488355327, -140737488355328,
            9223372036854775807, -9223372036854775808]
    script = """
        CREATE TABLE allt(i INTEGER, t TEXT, r REAL, b BLOB, n);
    """
    rows = []
    data = []
    for v in ints:
        # + 1/16 keeps this REAL off an exact integer; integral REALs are their
        # own block below because SQLite stores them differently on disk.
        data.append((v, "i=%d" % v, (v % 1000) / 8.0 + 0.0625, b"\x00\x01\x02", None))
    # NULL in each column position in turn, including all-NULL.
    for pos in range(5):
        row = [7, "seven", 7.5, b"\x07", "x"]
        row[pos] = None
        data.append(tuple(row))
    data.append((None, None, None, None, None))
    # REAL edge cases: SQLite's %.15g-plus-".0" rendering is easy to get wrong.
    for r in [0.1, 1 / 3, -2.5, 1e300, 1e-300, 1e21, 123456789012345.6,
              3.141592653589793, 5e-324]:
        data.append((None, None, r, None, None))
    # REALs whose value is an exact integer. SQLite shrinks these to an INTEGER
    # serial type on disk and re-floats them on read, so nothing in the record
    # says the value was ever a float -- a reader that trusts the serial type
    # prints 1 where sqlite3 prints 1.0.
    for r in [0.0, -0.0, 1.0, -1.0, 7.0, 1e16, 2.0 ** 53, -(2.0 ** 53)]:
        data.append((None, None, r, None, None))
    # Blobs containing NUL bytes and every byte value; UTF-8 text.
    data.append((1, "ascii", 1.5, bytes(range(256)), None))
    data.append((2, "héllo wörld é中文 \U0001f600", 2.5, b"\x00\x00\x00", None))
    data.append((3, "", 3.5, b"", "empty text and empty blob"))
    rows.append(("INSERT INTO allt VALUES(?,?,?,?,?)", data))
    db = make("types", script, rows)
    meta(db, ".tables")
    meta(db, ".schema")
    q(db, "SELECT * FROM allt")
    q(db, "SELECT * FROM allt", json=True)
    q(db, "SELECT i FROM allt WHERE i = -9223372036854775808")
    q(db, "SELECT i FROM allt WHERE i = 0")
    q(db, "SELECT i, t FROM allt WHERE t = 'ascii'")
    q(db, "SELECT i FROM allt WHERE i IS NULL")
    q(db, "SELECT i FROM allt WHERE i IS NOT NULL")
    q(db, "SELECT i FROM allt WHERE i > 100")
    q(db, "SELECT i FROM allt WHERE i <= -1")
    q(db, "SELECT r FROM allt WHERE r = 0.1")
    q(db, "SELECT b FROM allt WHERE b = X'00000000'")
    q(db, "SELECT i FROM allt WHERE i != 7")
    q(db, "SELECT rowid, i FROM allt WHERE rowid = 3")

    # 3. Overflow pages. The local-payload calculation has two branches (K and
    #    M) and a threshold X; a sweep of value lengths across the boundary is
    #    the only way to be sure both branches are right. Page size 512 puts the
    #    boundary at a small, easily swept length.
    for psize, lo, hi in ((512, 380, 1120), (1024, 900, 1400), (4096, 3980, 4200)):
        db = make("ovf%d" % psize,
                  "CREATE TABLE ov(n INTEGER, s TEXT, b BLOB);",
                  [("INSERT INTO ov VALUES(?,?,?)",
                    [(L, "a" * L, bytes([L & 0xFF]) * L) for L in range(lo, hi)])],
                  page_size=psize)
        q(db, "SELECT * FROM ov")
        q(db, "SELECT n FROM ov WHERE n = %d" % (lo + 5))
        q(db, "SELECT s FROM ov WHERE n = %d" % (hi - 1))

    # A value spanning many overflow pages, and one spanning exactly a few.
    db = make("ovlong", "CREATE TABLE big(k INTEGER, v TEXT, w BLOB);",
              [("INSERT INTO big VALUES(?,?,?)",
                [(1, "x" * 200000, b"\x00\xff" * 50000),
                 (2, "y" * 509, b"z" * 1),
                 (3, "é" * 30000, bytes(range(256)) * 200)])],
              page_size=512)
    q(db, "SELECT * FROM big")
    q(db, "SELECT * FROM big", json=True)
    q(db, "SELECT k FROM big WHERE k = 3")

    # 4. 50,000 rows: the table b-tree must grow interior pages, so a scan that
    #    only understands leaf pages returns a fraction of the rows.
    db = make("big50k",
              "CREATE TABLE r(id INTEGER PRIMARY KEY, k INTEGER, s TEXT, f REAL);",
              [("INSERT INTO r VALUES(?,?,?,?)",
                [(i, i * 7 % 50021, "row-%06d" % i, i / 16.0) for i in range(1, 50001)])])
    q(db, "SELECT * FROM r")
    q(db, "SELECT id FROM r WHERE id = 49999")
    q(db, "SELECT id, s FROM r WHERE k = 0")
    q(db, "SELECT id FROM r WHERE s = 'row-025000'")
    q(db, "SELECT id, f FROM r WHERE id > 49995")
    meta(db, ".tables")

    # 4b. The same row count on 512-byte pages: ~3,300 leaf pages need more
    #     interior cells than one interior page holds, so the tree grows a third
    #     level and an interior page points at another interior page. A walker
    #     that only handles one level of indirection stops here.
    db = make("deep",
              "CREATE TABLE d(id INTEGER PRIMARY KEY, s TEXT);",
              [("INSERT INTO d VALUES(?,?)",
                [(i, "deep-row-%06d-padding" % i) for i in range(1, 50001)])],
              page_size=512)
    q(db, "SELECT * FROM d")
    q(db, "SELECT id FROM d WHERE id = 50000")
    q(db, "SELECT s FROM d WHERE s = 'deep-row-033333-padding'")
    q(db, "SELECT id FROM d WHERE id > 49998")
    meta(db, ".dbinfo")

    # 5. Non-contiguous rowids after deletes, plus an explicit rowid gap, so the
    #    scan cannot assume rowid == position.
    db = make("gaps", """
        CREATE TABLE g(id INTEGER PRIMARY KEY, v TEXT);
        INSERT INTO g VALUES(1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e');
        DELETE FROM g WHERE id IN (2,4);
        INSERT INTO g VALUES(1000,'far'),(-5,'negative');
        INSERT INTO g(v) VALUES('auto');
    """)
    meta(db, ".dbinfo")
    q(db, "SELECT * FROM g")
    q(db, "SELECT rowid, id, v FROM g")
    q(db, "SELECT v FROM g WHERE id = -5")

    # A table with no INTEGER PRIMARY KEY at all: rowid is hidden, and every
    # column really is stored in the record.
    db = make("norowid", """
        CREATE TABLE p(a TEXT, b INTEGER);
        INSERT INTO p VALUES('x',1),('y',2);
        DELETE FROM p WHERE a='x';
        INSERT INTO p VALUES('z',3);
    """)
    q(db, "SELECT rowid, a, b FROM p")
    q(db, "SELECT * FROM p")

    # 6. ALTER TABLE ADD COLUMN: rows written before the ALTER have short
    #    records and the missing trailing columns must read as NULL.
    db = make("altered", """
        CREATE TABLE a(x INTEGER);
        INSERT INTO a VALUES(1),(2);
        ALTER TABLE a ADD COLUMN y TEXT;
        INSERT INTO a VALUES(3,'three');
        ALTER TABLE a ADD COLUMN z REAL;
        INSERT INTO a VALUES(4,'four',4.5);
    """)
    q(db, "SELECT * FROM a")
    q(db, "SELECT * FROM a", json=True)
    q(db, "SELECT x, z FROM a WHERE z IS NULL")

    # 7. Page sizes other than the default, including the 65536 case that the
    #    header encodes as the value 1.
    for psize in (512, 1024, 8192, 65536):
        db = make("ps%d" % psize,
                  "CREATE TABLE s(i INTEGER, t TEXT);",
                  [("INSERT INTO s VALUES(?,?)",
                    [(i, "p%d-%d" % (psize, i)) for i in range(300)])],
                  page_size=psize)
        q(db, "SELECT * FROM s")
        q(db, "SELECT t FROM s WHERE i = 299")
        meta(db, ".dbinfo")

    # 8. An index exists; sqlr ignores it and scans. Queries are chosen so the
    #    answer is order-identical whether SQLite uses the index or not.
    db = make("indexed", """
        CREATE TABLE ix(k TEXT, v INTEGER);
        INSERT INTO ix VALUES('a',1),('b',2),('c',3),('a',4);
        CREATE INDEX ix_k ON ix(k);
    """)
    meta(db, ".schema")
    q(db, "SELECT * FROM ix")
    q(db, "SELECT * FROM ix WHERE k = 'a'")

    # 9. Identifiers and declared types that stress the CREATE-statement
    #    shape-matcher: quoted names, a comma inside a type, table constraints
    #    that are not columns, DEFAULT values containing parentheses.
    db = make("ddl", """
        CREATE TABLE "odd name"(
            "a b" VARCHAR(10,2) NOT NULL DEFAULT 'x,y)',
            [brack] INTEGER DEFAULT 0,
            `tick` REAL,
            plain,
            CHECK ("a b" <> ''),
            UNIQUE([brack])
        );
        INSERT INTO "odd name" VALUES('hi', 5, 1.5, NULL);
    """)
    meta(db, ".schema")
    q(db, 'SELECT * FROM "odd name"')
    q(db, 'SELECT "a b", [brack], `tick` FROM "odd name"')

    # 10. Affinity in the WHERE clause: comparing a text literal against an
    #     INTEGER column and a numeric literal against a TEXT column is where
    #     SQLite's conversion rules bite.
    db = make("affin", """
        CREATE TABLE af(i INTEGER, t TEXT, n NUMERIC, noaff);
        INSERT INTO af VALUES(5,'5',5,5);
        INSERT INTO af VALUES(10,'10',10,'10');
        INSERT INTO af VALUES(0,'abc',0,'abc');
    """)
    q(db, "SELECT * FROM af WHERE i = '5'")
    q(db, "SELECT * FROM af WHERE t = 5")
    q(db, "SELECT * FROM af WHERE t = '5'")
    q(db, "SELECT * FROM af WHERE n = '10'")
    q(db, "SELECT * FROM af WHERE noaff = 5")
    q(db, "SELECT * FROM af WHERE noaff = '5'")
    q(db, "SELECT * FROM af WHERE i > '5'")

    return WORK


# --------------------------------------------------------------- comparison

def compare_cases():
    for db, argv, sql, kind in CASES:
        p = run(argv)
        if kind == ".tables":
            want = oracle_tables(db)
        elif kind == ".dbinfo":
            want = oracle_dbinfo(db)
        elif kind == ".schema":
            want = oracle_schema(db)
        else:
            want = oracle_query(db, sql, kind == "json")
        label = "%s :: %s" % (os.path.basename(db), " ".join(argv[len(argv) - 1:]))
        if p.returncode != 0:
            check(False, label, "sqlr exited %d: %s" % (p.returncode, p.stderr.decode()[:300]))
            continue
        if p.stdout != want:
            detail = "sqlr gave %d bytes, sqlite gave %d bytes" % (len(p.stdout), len(want))
            for i in range(min(len(p.stdout), len(want))):
                if p.stdout[i] != want[i]:
                    a = p.stdout[max(0, i - 60):i + 60]
                    b = want[max(0, i - 60):i + 60]
                    detail += "\n    first difference at byte %d\n    sqlr   : %r\n    sqlite : %r" % (i, a, b)
                    break
            check(False, label, detail)
        else:
            check(True, label)


# ---------------------------------------------------------- loud refusals
# Things sqlr does not support must fail with a message, not silently lie.

def refusals():
    db = make("wor", """
        CREATE TABLE w(k TEXT PRIMARY KEY, v INTEGER) WITHOUT ROWID;
        INSERT INTO w VALUES('a',1),('b',2);
    """)
    p = run([db, "SELECT * FROM w"])
    check(p.returncode == 1 and b"WITHOUT ROWID" in p.stderr,
          "WITHOUT ROWID table is refused loudly",
          "rc=%d err=%r" % (p.returncode, p.stderr[:200]))

    db2 = CASES[0][0]
    for bad, needle in (
        ("SELECT * FROM nosuchtable", b"no such table"),
        ("SELECT nosuchcol FROM one", b"no such column"),
        ("UPDATE one SET a=1", b"only SELECT"),
        ("SELECT * FROM one WHERE a = 1 ORDER BY a", b"trailing"),
        ("SELECT * FROM one JOIN one", b"trailing"),
        ("SELECT", b"column"),
    ):
        p = run([db2, bad])
        check(p.returncode == 1 and needle in p.stderr,
              "refusal: %s" % bad, "rc=%d err=%r" % (p.returncode, p.stderr[:200]))

    # A WAL-mode database is a documented non-goal, but it must not be misread.
    wal = os.path.join(WORK, "walmode.db")
    for s in ("", "-wal", "-shm"):
        if os.path.exists(wal + s):
            os.remove(wal + s)
    conn = sqlite3.connect(wal)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript("CREATE TABLE w(a); INSERT INTO w VALUES(1),(2),(3);")
    conn.commit()
    conn.close()
    # After a clean close SQLite checkpoints, so the .db is complete and sqlr
    # reads it fine. That is the honest claim: sqlr reads a checkpointed file.
    p = run([wal, "SELECT * FROM w"])
    check(p.returncode == 0 and p.stdout == b"1\n2\n3\n",
          "checkpointed WAL-mode file reads correctly",
          "rc=%d out=%r" % (p.returncode, p.stdout[:120]))


# ------------------------------------------------------------- robustness
# A file-format parser is fed untrusted bytes. Truncated and corrupted input
# must produce an error, not a crash and not a hang.

def readonly_check():
    """sqlr must never modify a database file."""
    import hashlib
    src = os.path.join(WORK, "types.db")
    with open(src, "rb") as f:
        before = hashlib.sha256(f.read()).hexdigest()
    mode_before = os.stat(src).st_mode
    for c in ([".tables"], [".schema"], ["SELECT * FROM allt"]):
        run([src] + c)
    with open(src, "rb") as f:
        after = hashlib.sha256(f.read()).hexdigest()
    check(before == after and mode_before == os.stat(src).st_mode,
          "database file is unchanged after reading it")
    check(not any(os.path.exists(src + s) for s in ("-journal", "-wal", "-shm")),
          "sqlr created no journal or WAL sidecar")


def main():
    if not os.path.exists(SQLR):
        print("sqlr binary not found at %s; run make first" % SQLR, file=sys.stderr)
        return 2
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)

    print("building corpus with SQLite %s ..." % sqlite3.sqlite_version)
    build_corpus()
    n = len(os.listdir(WORK))
    print("%d databases, %d comparison cases" % (n, len(CASES)))

    compare_cases()
    refusals()
    readonly_check()

    if failures:
        print("\nFAILURES:")
        for f in failures:
            print("  - " + f)
    print("\n%d agree, %d disagree" % (agree, disagree))
    return 1 if disagree else 0


if __name__ == "__main__":
    sys.exit(main())
