#!/usr/bin/env python3
"""The oracle: SQLite itself is the authority, sqlr is the thing on trial.

Every database in the corpus is CREATEd and INSERTed by SQLite. Every query is
answered twice -- once by SQLite, once by sqlr -- and the two answers are
compared byte for byte. Any difference is a disagreement and fails the run.

The sqlite3 command-line shell is not installed in this environment, so the
authority is reached through python3's stdlib binding to the same libsqlite3
(see README, "The oracle"). It is the same engine; only the front end differs.
Every rendering decision that could paper over a sqlr bug is pushed
back into SQLite: REAL text comes from SQLite's own CAST(x AS TEXT) and blob hex
from SQLite's own hex(), so put_real() and put_blob() in sqlr.c are checked
against SQLite's formatting rather than against a Python reimplementation.
"""
import ctypes
import ctypes.util
import os
import random
import re
import shutil
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SQLR = os.environ.get("SQLR", os.path.join(ROOT, "sqlr"))
# The pid is in the path because two runs of this suite at once used to
# share t/work, and the first thing each does is delete it -- so the slower
# run had its databases removed out from under it mid-comparison. Hit for
# real: two instances of .portfolio/extract.py overlapped, and the repos with
# fixed scratch paths reported failures they do not have.
WORK = os.path.join(HERE, "work.%d" % os.getpid())

# Checks are counted by what the authority is, not all into one number. The
# run used to print "171 agree, 0 disagree", and 28 of those 171 were not
# agreements with anything: they were sqlr refusing a query, erroring cleanly
# on a corrupt file, or leaving a file unmodified. Adding another "correctly
# declines this" assertion made the agreement figure go up, which is the
# wrong direction for a number whose name says SQLite agreed.
#
#   sqlite   SQLite answered the same question and the bytes matched: a query,
#            a PRAGMA, dbstat, EXPLAIN QUERY PLAN.
#   self     no outside authority. sqlr's two access paths against each other,
#            sqlr's own choice of path, and a fixture's bytes read against the
#            format specification.
#   refusal  sqlr must decline loudly: exit 1 and say why.
#   robust   sqlr must not misbehave on truncated, corrupt or hostile input,
#            and must not write. The claim is not "matches SQLite", because
#            SQLite's behaviour on a corrupt file is not a specification.
KINDS = ("sqlite", "self", "refusal", "robust")
KIND_WHAT = {
    "sqlite": "agree with SQLite",
    "self": "checks with no outside authority (the two access paths, fixture bytes)",
    "refusal": "refusals, each loud and with a reason",
    "robust": "hostile inputs that error cleanly, and the read-only checks",
}
passed = dict.fromkeys(KINDS, 0)
failed = dict.fromkeys(KINDS, 0)
failures = []
notes = []


def check(ok, what, detail="", kind="sqlite"):
    assert kind in KINDS, kind
    if ok:
        passed[kind] += 1
    else:
        failed[kind] += 1
        failures.append("[%s] " % kind + what + (("\n    " + detail) if detail else ""))


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
    """Six of the seven .dbinfo lines are header fields SQLite also reports
    through a PRAGMA, so the 100-byte header parse is checked field by field.

    reserved_bytes is the seventh and has no PRAGMA -- SQLite will set it
    through sqlite3_file_control but never reads it back out, so the authority
    here is the file format specification instead: it is the single byte at
    offset 20. That is a second implementation of the same spec sentence
    rather than a second opinion, which is weaker than a PRAGMA, so say which
    one it is. It used to be a hardcoded 0, which every fixture also happened
    to be; deleting sqlr's read of byte 20 altogether left the suite at
    "114 agree, 0 disagree".
    """
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        def pr(name):
            return conn.execute("PRAGMA " + name).fetchone()[0]
        with open(db, "rb") as fh:
            reserved = fh.read(21)[20]
        return ("page_size\t%d\n"
                "page_count\t%d\n"
                "reserved_bytes\t%d\n"
                "text_encoding\t%s\n"
                "schema_cookie\t%d\n"
                "user_version\t%d\n"
                "freelist_pages\t%d\n"
                % (pr("page_size"), pr("page_count"), reserved, pr("encoding"),
                   pr("schema_version"), pr("user_version"),
                   pr("freelist_count"))).encode()
    finally:
        conn.close()


def oracle_indexes(db):
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        rows = conn.execute(
            "SELECT name FROM sqlite_schema WHERE type='index' AND rootpage > 0"
            " ORDER BY name"
        ).fetchall()
        return b"".join(r[0].encode() + b"\n" for r in rows)
    finally:
        conn.close()


def oracle_index(db, name, sql):
    """What the index b-tree holds, according to SQLite walking it.

    The expected bytes are not an ORDER BY I reasoned out: `INDEXED BY` makes
    SQLite traverse that index and emit its entries in the order they are
    stored, which is the same walk sqlr does over the same pages. That is why
    the plan is checked first -- if SQLite answered by sorting the table
    instead, the comparison would be against my idea of the index order rather
    than against the index, and a DESC or COLLATE NOCASE index is exactly
    where that idea goes wrong.
    """
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        plan = " | ".join(r[3] for r in conn.execute("EXPLAIN QUERY PLAN " + sql))
        check(re.search(r"\bINDEX %s\b" % re.escape(name), plan) is not None,
              "%s: SQLite answers the %s oracle query by walking that index"
              % (os.path.basename(db), name), plan)
        out = bytearray()
        for r in conn.execute(sql):
            out += b"\t".join(o_tsv_val(conn, v) for v in r) + b"\n"
        return bytes(out)
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

def seed_reserve_bytes(path, n):
    """Create `path` as a database whose pages end with `n` reserved bytes.

    Reserved bytes shrink the usable page below the page size, which moves
    every overflow threshold and the end of the cell content area. There is no
    PRAGMA that sets them and none that reads them back: the only interface is
    sqlite3_file_control(SQLITE_FCNTL_RESERVE_BYTES), which Python's module
    does not expose. So drive libsqlite3 through ctypes for this one step --
    still SQLite doing the writing, which is the rule the corpus is built on.
    The value lives in the header from here on, so the ordinary Python path
    can carry on appending to the file afterwards.
    """
    lib = ctypes.CDLL(ctypes.util.find_library("sqlite3") or "libsqlite3.so.0")
    # The rest of the corpus is written by Python's sqlite3 module, and this
    # one file is started by a libsqlite3 loaded separately. On this machine
    # they are the same 3.46.1, but a Python linked against a different
    # SQLite would have two versions writing one fixture -- which breaks the
    # "SQLite is the only thing that writes" rule in a way that is hard to
    # see, since both halves are still SQLite.
    lib.sqlite3_libversion.restype = ctypes.c_char_p
    ctypes_ver = lib.sqlite3_libversion().decode()
    if ctypes_ver != sqlite3.sqlite_version:
        raise RuntimeError(
            "libsqlite3 via ctypes is %s but Python's module is %s; one "
            "fixture would be written by two different SQLites"
            % (ctypes_ver, sqlite3.sqlite_version))
    db = ctypes.c_void_p()
    if lib.sqlite3_open(path.encode(), ctypes.byref(db)) != 0:
        raise RuntimeError("sqlite3_open failed for %s" % path)
    try:
        want = ctypes.c_int(n)
        # SQLITE_FCNTL_RESERVE_BYTES == 38
        if lib.sqlite3_file_control(db, None, 38, ctypes.byref(want)) != 0:
            raise RuntimeError("SQLITE_FCNTL_RESERVE_BYTES refused")
        # A page has to be written for the header to reach the file, and the
        # VACUUM is what rewrites it with the new usable size.
        lib.sqlite3_exec(db, b"CREATE TABLE seed(x); VACUUM;", None, None, None)
    finally:
        lib.sqlite3_close(db)
    with open(path, "rb") as fh:
        got = fh.read(21)[20]
    if got != n:
        raise RuntimeError("asked for %d reserved bytes, header says %d" % (n, got))


def make(name, script, rows=(), page_size=None, extra=None, reserve_bytes=None):
    """Build a database with SQLite. SQLite is the only thing that ever writes."""
    path = os.path.join(WORK, name + ".db")
    if os.path.exists(path):
        os.remove(path)
    if reserve_bytes:
        seed_reserve_bytes(path, reserve_bytes)
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


def check_btree_shape(db, table, levels, internal, leaf):
    """Assert the shape of a fixture's b-tree, using SQLite's own dbstat view.

    A fixture whose whole purpose is its shape has to have that shape
    asserted, not described. README.md and PLAN.md both name this tree as "a
    4-level b-tree, 3,266 leaf and 66 interior pages" and give that as the
    reason the fixture exists -- an interior page whose children are also
    interior pages. Nothing checked it. SQLite chooses the shape, so a change
    in how it fills pages would quietly turn the fixture into a shallow tree
    and the claim into fiction, with every test still passing.
    """
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        counts = dict(conn.execute(
            "SELECT pagetype, count(*) FROM dbstat WHERE name = ? GROUP BY pagetype",
            (table,)).fetchall())
        # dbstat's path is '/' for the root and one '/NNN' per level below it,
        # so the number of separators in the longest path is the depth.
        depth = conn.execute(
            "SELECT max(length(path) - length(replace(path, '/', '')))"
            " FROM dbstat WHERE name = ?", (table,)).fetchone()[0]
    except sqlite3.OperationalError as e:
        check(False, "%s/%s b-tree shape" % (os.path.basename(db), table),
              "dbstat unavailable in this SQLite build: %s" % e)
        return
    finally:
        conn.close()
    got = (depth, counts.get("internal", 0), counts.get("leaf", 0))
    want = (levels, internal, leaf)
    check(got == want,
          "%s: %s is a %d-level b-tree, %d interior and %d leaf pages"
          % (os.path.basename(db), table, levels, internal, leaf),
          "dbstat says depth %d, %d interior, %d leaf" % got)


def check_index_pages(db, name, overflow=False):
    """Assert that an index fixture really grew an INTERIOR page (type 2).

    An index that fits on one page is all type 10, and a suite that only ever
    sees leaf pages has not tested the interior cell format at all -- which is
    the one that carries a key and a child pointer in the same cell. SQLite
    decides how many rows fit, so the fixture has to be measured, not assumed.
    """
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        counts = dict(conn.execute(
            "SELECT pagetype, count(*) FROM dbstat WHERE name = ? GROUP BY pagetype",
            (name,)).fetchall())
    except sqlite3.OperationalError as e:
        check(False, "%s/%s index page mix" % (os.path.basename(db), name),
              "dbstat unavailable in this SQLite build: %s" % e)
        return
    finally:
        conn.close()
    ok = counts.get("internal", 0) >= 1 and counts.get("leaf", 0) >= 2
    want = "interior and leaf pages"
    if overflow:
        ok = ok and counts.get("overflow", 0) >= 1
        want = "interior, leaf and overflow pages"
    check(ok, "%s: index %s has %s" % (os.path.basename(db), name, want),
          "dbstat says %r" % (counts,))


CASES = []  # (dbpath, argv_tail, sql_for_oracle_or_None, kind)


def q(db, sql, json=False):
    CASES.append((db, (["--json"] if json else []) + [db, sql], sql, "json" if json else "tsv"))


def meta(db, which):
    CASES.append((db, [db, which], None, which))


def idx(db, name, sql):
    """Compare sqlr's dump of one index b-tree against SQLite's walk of it."""
    CASES.append((db, [db, ".index", name], sql, "index"))


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
    meta(db, ".indexes")  # a database with no index at all: no output
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
    check_btree_shape(db, "d", levels=4, internal=66, leaf=3266)

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

    # 8. The degenerate index: four entries, so the whole b-tree is one leaf
    #    page and there is no interior page anywhere. A table scan over the
    #    same table must still ignore the index and give the same answer.
    db = make("indexed", """
        CREATE TABLE ix(k TEXT, v INTEGER);
        INSERT INTO ix VALUES('a',1),('b',2),('c',3),('a',4);
        CREATE INDEX ix_k ON ix(k);
    """)
    meta(db, ".schema")
    meta(db, ".indexes")
    idx(db, "ix_k", "SELECT k, rowid FROM ix INDEXED BY ix_k")
    q(db, "SELECT * FROM ix")
    q(db, "SELECT * FROM ix WHERE k = 'a'")

    # 8b. Index b-trees with interior pages. 2,000 rows is enough that every
    #     index here is two levels, which is the point: an index interior cell
    #     carries a KEY as well as a child pointer, unlike a table interior
    #     cell, and a corpus of single-page indexes never exercises it. The
    #     shape is asserted from dbstat below rather than assumed.
    #
    #     The columns are chosen for the decisions each one forces: a REAL
    #     column, because SQLite shrinks an integral float to an integer
    #     serial type inside an index exactly as it does inside a table, so
    #     the index dump needs the column's affinity to print 0.0 and not 0;
    #     a BLOB column, which sorts after every text value; NULLs, which sort
    #     before everything; a COLLATE NOCASE index and a DESC index, whose
    #     stored order is neither the table's nor plain byte order.
    rnd = random.Random(20261018)
    emp = []
    for i in range(2000):
        nm = None if i % 97 == 0 else ("N%04d" if i % 3 else "n%04d") % rnd.randrange(3000)
        emp.append((nm, "d%d" % (i % 5), float(i % 7) * 50, bytes([i % 256, (i * 7) % 256])))
    db = make("idx", """
        CREATE TABLE emp(id INTEGER PRIMARY KEY, name TEXT, dept TEXT,
                         salary REAL, tag BLOB);
    """, rows=[("INSERT INTO emp(name,dept,salary,tag) VALUES(?,?,?,?)", emp)],
         extra="""
        CREATE INDEX i_name ON emp(name);
        CREATE INDEX i_dept_sal ON emp(dept, salary);
        CREATE UNIQUE INDEX i_uid ON emp(name, id);
        CREATE INDEX i_nocase ON emp(name COLLATE NOCASE);
        CREATE INDEX i_desc ON emp(name DESC);
        CREATE INDEX i_sal ON emp(salary);
        CREATE INDEX i_tag ON emp(tag);
        CREATE INDEX i_part ON emp(dept) WHERE salary > 100;
    """)
    meta(db, ".indexes")
    for name in ("i_name", "i_dept_sal", "i_uid", "i_nocase", "i_desc", "i_sal",
                 "i_tag", "i_part"):
        check_index_pages(db, name)
    idx(db, "i_name", "SELECT name, rowid FROM emp INDEXED BY i_name")
    idx(db, "i_dept_sal", "SELECT dept, salary, rowid FROM emp INDEXED BY i_dept_sal")
    idx(db, "i_uid", "SELECT name, id, rowid FROM emp INDEXED BY i_uid")
    idx(db, "i_nocase", "SELECT name, rowid FROM emp INDEXED BY i_nocase")
    idx(db, "i_desc", "SELECT name, rowid FROM emp INDEXED BY i_desc")
    idx(db, "i_sal", "SELECT salary, rowid FROM emp INDEXED BY i_sal")
    idx(db, "i_tag", "SELECT tag, rowid FROM emp INDEXED BY i_tag")
    idx(db, "i_part", "SELECT dept, rowid FROM emp INDEXED BY i_part WHERE salary > 100")
    q(db, "SELECT id, name FROM emp WHERE id = 1999")

    # 8c. Index payloads that overflow, on 512-byte pages. The local-payload
    #     threshold X is NOT the same on an index page as on a table leaf: it
    #     is 64/255ths of the usable space minus 23, about 102 bytes here,
    #     where a table leaf keeps 477. A 300-byte key therefore spills on an
    #     index page and not on the table page holding the same string, so a
    #     parser that reuses the table formula reads every long key wrong.
    #     The interior page's own cells carry keys too, and those overflow as
    #     well, which is what makes this fixture reach the overflow reader
    #     from an interior cell.
    #
    #     The same database carries an auto-index: a UNIQUE constraint makes
    #     SQLite build sqlite_autoindex_u_1, whose sqlite_schema row has no
    #     SQL at all, so its key columns cannot be read from a statement.
    db = make("ixovf", """
        CREATE TABLE lk(k TEXT);
        CREATE TABLE u(k TEXT UNIQUE, v INTEGER);
    """, rows=[("INSERT INTO lk VALUES(?)",
                [("L%03d-" % i + "x" * 300,) for i in range(60)]),
               ("INSERT INTO u VALUES(?,?)",
                [("key%03d" % i, i) for i in range(400)])],
         extra="CREATE INDEX ix_long ON lk(k);", page_size=512)
    meta(db, ".indexes")
    check_index_pages(db, "ix_long", overflow=True)
    check_index_pages(db, "sqlite_autoindex_u_1")
    idx(db, "ix_long", "SELECT k, rowid FROM lk INDEXED BY ix_long")
    idx(db, "sqlite_autoindex_u_1",
        "SELECT k, rowid FROM u INDEXED BY sqlite_autoindex_u_1")

    # 8d. Indexes a seek has to decline, one per reason, each of them sitting
    #     on exactly the column an equality WHERE names. Using any of them
    #     would give the wrong rows or the right rows in the wrong order, so
    #     the query falls back to the scan -- and a decline is a different
    #     failure from a wrong answer, which is why the harness counts them
    #     apart. 512-byte pages and 1,500 rows so that every one of these
    #     indexes is more than a single page, i.e. a seek really would have
    #     saved something. p_e is here so the fixture is not all declines.
    db = make("noseek", """
        CREATE TABLE p(a TEXT, b TEXT, c REAL, d TEXT, e INTEGER);
    """, rows=[("INSERT INTO p(a,b,c,d,e) VALUES(?,?,?,?,?)",
                [("a%03d" % (i % 200), "B%03d" % (i % 200), float(i % 50),
                  "d%d" % (i % 4), i) for i in range(1500)])],
         extra="""
        CREATE INDEX p_nocase ON p(a COLLATE NOCASE);
        CREATE INDEX p_desc ON p(b DESC);
        CREATE INDEX p_expr ON p(abs(c));
        CREATE INDEX p_part ON p(d) WHERE c > 5;
        CREATE INDEX p_e ON p(e);
    """, page_size=512)
    meta(db, ".indexes")
    for name in ("p_nocase", "p_desc", "p_expr", "p_part", "p_e"):
        check_index_pages(db, name)

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

    # 11. Reserved bytes at the end of every page. This is the one header
    #     field with no PRAGMA behind it, and every other fixture has it at
    #     zero -- which is also what .dbinfo used to be compared against, so
    #     the field was never really read. Reserved bytes are not just a
    #     number to print: they shrink the usable page, which moves the end of
    #     the cell content area and every overflow threshold. A parser that
    #     reports the byte but computes thresholds from the full page size
    #     gets the header line right and the rows wrong, so the fixture holds
    #     a value long enough to overflow and a 512-byte page to overflow it
    #     sooner.
    db = make("reserve", """
        CREATE TABLE r(k INTEGER PRIMARY KEY, short TEXT, long TEXT);
    """, rows=[("INSERT INTO r VALUES(?,?,?)",
                [(i, "s%d" % i, ("payload-%03d " % i) * 60) for i in range(40)])],
         page_size=512, reserve_bytes=32)
    meta(db, ".dbinfo")
    meta(db, ".tables")
    meta(db, ".schema")
    q(db, "SELECT k, short FROM r")
    q(db, "SELECT * FROM r")
    q(db, "SELECT long FROM r WHERE k = 17")
    q(db, "SELECT * FROM r WHERE k = 39")
    q(db, "SELECT * FROM r WHERE k IS NOT NULL", json=True)

    return WORK


# --------------------------------------------------------------- comparison

def plan_of(stderr):
    """The access path sqlr reported under --plan, as a tag, and the pages it
    touched. `plan: scan t` -> "scan"; `plan: search t using rowid (rowid=?)`
    -> "rowid"; `plan: search t using index i_name (name=?)` -> "index:i_name".
    """
    tag, pages = None, None
    for line in stderr.decode("utf-8", "replace").splitlines():
        if line.startswith("plan: "):
            w = line[6:].split()
            if w[0] == "scan":
                tag = "scan"
            elif w[3] == "rowid":
                tag = "rowid"
            else:
                tag = "index:" + w[4]
        elif line.startswith("pages: "):
            pages = int(line[7:])
    return tag, pages


# How the 113 comparison cases were answered, tallied by access path. A row
# comparison passes whichever path ran, so a suite where everything fell back
# to the scan would look exactly like this one and test nothing about indexes.
paths = {}


def compare_cases():
    for db, argv, sql, kind in CASES:
        # --plan goes on every SELECT: stdout is unaffected, and the tally of
        # which path answered each case is then taken from the comparisons
        # that are already byte-exact against SQLite rather than from a
        # separate set of runs that might not be wired up the same way.
        if kind in ("tsv", "json"):
            p = run(["--plan"] + argv)
            tag = plan_of(p.stderr)[0]
            paths[tag] = paths.get(tag, 0) + 1
        else:
            p = run(argv)
        if kind == ".tables":
            want = oracle_tables(db)
        elif kind == ".dbinfo":
            want = oracle_dbinfo(db)
        elif kind == ".schema":
            want = oracle_schema(db)
        elif kind == ".indexes":
            want = oracle_indexes(db)
        elif kind == "index":
            want = oracle_index(db, argv[-1], sql)
        else:
            want = oracle_query(db, sql, kind == "json")
        n = 2 if kind == "index" else 1
        label = "%s :: %s" % (os.path.basename(db), " ".join(argv[len(argv) - n:]))
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
        CREATE TABLE w(k TEXT PRIMARY KEY, v INTEGER, u TEXT UNIQUE) WITHOUT ROWID;
        INSERT INTO w VALUES('a',1,'x'),('b',2,'y');
        CREATE INDEX wi ON w(v);
    """)
    p = run([db, "SELECT * FROM w"])
    check(p.returncode == 1 and b"WITHOUT ROWID" in p.stderr,
          "WITHOUT ROWID table is refused loudly",
          "rc=%d err=%r" % (p.returncode, p.stderr[:200]), kind="refusal")

    # An index on a WITHOUT ROWID table is a readable b-tree whose entries end
    # with the table's primary key instead of a rowid. Dumping one would print
    # something that looks right and is not, so it is refused -- including the
    # auto-index, which has no CREATE statement and so has to be refused via
    # its table rather than via its own SQL.
    for which in ("wi", "sqlite_autoindex_w_2"):
        p = run([db, ".index", which])
        check(p.returncode == 1 and b"WITHOUT ROWID" in p.stderr,
              "index %s on a WITHOUT ROWID table is refused loudly" % which,
              "rc=%d err=%r" % (p.returncode, p.stderr[:200]), kind="refusal")

    p = run([db, ".index", "nosuchindex"])
    check(p.returncode == 1 and b"no such index" in p.stderr,
          "refusal: .index on a name that is not an index",
          "rc=%d err=%r" % (p.returncode, p.stderr[:200]), kind="refusal")
    p = run([db, ".index", "w"])
    check(p.returncode == 1 and b"no such index" in p.stderr,
          "refusal: .index on a table name",
          "rc=%d err=%r" % (p.returncode, p.stderr[:200]), kind="refusal")

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
              "refusal: %s" % bad, "rc=%d err=%r" % (p.returncode, p.stderr[:200]),
              kind="refusal")

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
    want = oracle_query(wal, "SELECT * FROM w", False)
    check(p.returncode == 0 and p.stdout == want,
          "checkpointed WAL-mode file reads correctly",
          "rc=%d out=%r want=%r" % (p.returncode, p.stdout[:120], want[:120]))


# ------------------------------------------------- the index against the scan
# An equality WHERE can now be answered by descending an index instead of
# reading every row. That is checkable twice over, and the two halves find
# different bugs, so they are counted apart:
#
#   the index path must return exactly what the scan path returns, byte for
#   byte. No outside authority is involved; sqlr is being compared against
#   itself, and a seek that prunes a subtree it should have entered fails here
#   without SQLite being consulted at all.
#
#   both must return what SQLite returns. A seek and a scan that agree with
#   each other and not with SQLite is a different bug from a seek that
#   disagrees with its own scan, and only this half can see it.
#
# EXPLAIN QUERY PLAN is the second half of the oracle for the decision rather
# than the rows: it says whether SQLite chose an index for the same query.
# Where sqlr declines an index SQLite uses, the rows still have to agree and
# the difference is recorded as a finding, not a failure -- a declined index
# is a slow answer, a wrongly used one is a wrong answer.

def sqlite_plan(db, sql):
    """How SQLite says it will answer `sql`, as the same tag sqlr prints."""
    conn = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    try:
        rows = [r[3] for r in conn.execute("EXPLAIN QUERY PLAN " + sql)]
    finally:
        conn.close()
    line = rows[0] if rows else ""
    if line.startswith("SEARCH"):
        if "INTEGER PRIMARY KEY" in line:
            return "rowid", line
        m = re.search(r"INDEX (\S+) ", line)
        if m:
            return "index:" + m.group(1), line
    return "scan", line


def index_vs_scan(work):
    conn = sqlite3.connect("file:%s?mode=ro" % os.path.join(work, "idx.db"), uri=True)
    # The key values at the edges of the index come from SQLite, not from an
    # idea of what the fixture holds: the first and last entry of a b-tree are
    # where a pruning rule that is off by one stops pruning or over-prunes.
    lo, hi = conn.execute(
        "SELECT min(name), max(name) FROM emp WHERE name IS NOT NULL").fetchone()
    common = conn.execute(
        "SELECT name FROM emp WHERE name IS NOT NULL GROUP BY name"
        " ORDER BY count(*) DESC, name LIMIT 1").fetchone()[0]
    # A blob key that is really in the index, taken from SQLite rather than
    # worked out from how the fixture was generated. X'0007' looked like a
    # plausible tag and matches no row, so that case was seeking a key that
    # was not there and never reached the table at all.
    blob = conn.execute("SELECT hex(tag) FROM emp WHERE id = 100").fetchone()[0]
    conn.close()
    # Two keys from the overflowing index, chosen by where they sit on their
    # leaf page rather than at random. The lowest key in the whole index is
    # the first cell of the first leaf, and L043 is the last of four on its
    # leaf, so between them the entries that FOLLOW the match differ -- which
    # is what a walk that fails to stop at the end of the matching run reads
    # on into. With "L042-" alone, which happens to sit where its successors
    # cost nothing, the mutation that drops `return 0` from the leaf survived.
    longkey = "L000-" + "x" * 300
    longkey2 = "L043-" + "x" * 300

    # (database, SQL, the path sqlr takes, the path SQLite reports, the pages
    # the chosen path touches or None for a scan).
    #
    # The SQLite column is measured, not predicted: it picks i_desc over
    # i_name for an equality on `name` because i_desc covers the query, which
    # is worth pinning -- if a later SQLite changes its mind, this says so
    # rather than quietly comparing against something else.
    #
    # The page count is the exact number, not "fewer than the scan". Three
    # mutations of the pruning rules return the right rows from the wrong
    # number of pages: descending a child that can only hold smaller keys,
    # reading on past the end of the matching run, and emitting an interior
    # key that is not equal. All three survive an inequality and die against
    # the count. It is pinned to how this SQLite fills pages, which the
    # dbstat assertions above already depend on.
    cases = [
        ("idx", "SELECT id, name FROM emp WHERE name = '%s'" % lo, "index:i_name", "index:i_desc", 4),
        ("idx", "SELECT id, name FROM emp WHERE name = '%s'" % hi, "index:i_name", "index:i_desc", 4),
        ("idx", "SELECT id, name FROM emp WHERE name = '%s'" % common, "index:i_name", "index:i_desc", 10),
        ("idx", "SELECT id, name FROM emp WHERE name = 'AAAA'", "index:i_name", "index:i_desc", 2),
        ("idx", "SELECT id, name FROM emp WHERE name = 'zzzzzz'", "index:i_name", "index:i_desc", 2),
        # 285 of the 2,000 rows match and the seek loses badly: 573 pages
        # against the scan's 12, because it re-descends the table b-tree once
        # per matching row. Asserted rather than hidden. See the README.
        ("idx", "SELECT id, salary FROM emp WHERE salary = 300.0", "index:i_sal", "index:i_sal", 573),
        ("idx", "SELECT id, salary FROM emp WHERE salary = 300", "index:i_sal", "index:i_sal", 573),
        # The tag values repeat every 256 rows, so eight rows match and eight
        # descents of the table cost 18 pages against the scan's 12. Same
        # shape as the salary case below, found by pinning the count.
        ("idx", "SELECT id, tag FROM emp WHERE tag = X'%s'" % blob, "index:i_tag", "index:i_tag", 18),
        ("idx", "SELECT id, dept FROM emp WHERE dept = 'd3'", "scan", "index:i_dept_sal", None),
        ("idx", "SELECT id, name FROM emp WHERE name IS NULL", "scan", "index:i_desc", None),
        ("idx", "SELECT id, name FROM emp WHERE name > 'N5'", "scan", "index:i_desc", None),
        ("idx", "SELECT id, name FROM emp WHERE id = 1999", "rowid", "rowid", 2),
        ("idx", "SELECT id, name FROM emp WHERE rowid = 1", "rowid", "rowid", 2),
        ("idx", "SELECT id FROM emp WHERE id = 2.0", "scan", "rowid", None),
        ("idx", "SELECT id FROM emp WHERE id = '7'", "rowid", "rowid", 2),
        ("noseek", "SELECT a FROM p WHERE a = 'a042'", "scan", "scan", None),
        ("noseek", "SELECT b FROM p WHERE b = 'B042'", "scan", "index:p_desc", None),
        ("noseek", "SELECT c FROM p WHERE c = 7.0", "scan", "scan", None),
        ("noseek", "SELECT d FROM p WHERE d = 'd1'", "scan", "scan", None),
        ("noseek", "SELECT e FROM p WHERE e = 1234", "index:p_e", "index:p_e", 5),
        ("ixovf", "SELECT k FROM lk WHERE k = '%s'" % longkey, "index:ix_long", "index:ix_long", 8),
        ("ixovf", "SELECT k FROM lk WHERE k = '%s'" % longkey2, "index:ix_long", "index:ix_long", 22),
        ("ixovf", "SELECT k, v FROM u WHERE k = 'key100'", "scan", "index:sqlite_autoindex_u_1", None),
        ("tiny", "SELECT * FROM one WHERE a = 42", "scan", "scan", None),
        ("deep", "SELECT s FROM d WHERE id = 33333", "rowid", "rowid", 4),
    ]

    seeks = scans = 0
    best = (1.0, "")
    for name, sql, want_path, want_sqlite, want_pages in cases:
        db = os.path.join(work, name + ".db")
        tag = "%s :: %s" % (name, sql if len(sql) < 70 else sql[:67] + "...")

        chosen = run(["--plan", db, sql])
        got, pages = plan_of(chosen.stderr)
        forced = run(["--plan", "--scan", db, sql])
        scan_tag, scan_pages = plan_of(forced.stderr)

        if chosen.returncode != 0 or forced.returncode != 0 or got is None \
                or pages is None or scan_pages is None:
            # Report rather than raise. A mutation that turns one of the scan
            # cases into a seek leaves want_pages at None, and the first
            # version of the page assertion below formatted that with %d and
            # died with a TypeError -- after the per-kind counters had been
            # updated and before they were printed. The mutant was caught and
            # the run said so with a traceback instead of a failure line,
            # which a sweep reading exit codes scores as an unknown.
            check(False, tag, "sqlr exited %d/%d, plan %r, pages %r/%r: %r"
                  % (chosen.returncode, forced.returncode, got, pages, scan_pages,
                     chosen.stderr[:300]), kind="self")
            continue

        # Which path ran, asserted per query rather than left to the row
        # comparison, which passes either way.
        check(got == want_path, "%s: sqlr takes the %s path" % (tag, want_path),
              "sqlr says %s" % got, kind="self")
        check(scan_tag == "scan", "%s: --scan forces the scan" % tag,
              "sqlr says %s" % scan_tag, kind="self")
        if got == "scan":
            scans += 1
        else:
            seeks += 1
            # A seek that quietly reads every page is a seek in name only, and
            # every row comparison passes for it.
            check(pages == want_pages, "%s: the seek touches %s pages" % (tag, want_pages),
                  "touched %d, and the scan touches %d" % (pages, scan_pages), kind="self")
            if pages < scan_pages and pages:
                ratio = scan_pages / pages
                if ratio > best[0]:
                    best = (ratio, "%s: %d pages against %d" % (tag, pages, scan_pages))

        # The two paths against each other. Byte-exact, no oracle involved.
        check(chosen.stdout == forced.stdout,
              "%s: the chosen path and the forced scan return the same bytes" % tag,
              "chosen %d bytes, scan %d bytes\n    %r\n    %r"
              % (len(chosen.stdout), len(forced.stdout),
                 chosen.stdout[:200], forced.stdout[:200]), kind="self")

        # And both against SQLite, byte for byte. The oracle query carries an
        # ORDER BY rowid that sqlr is never given: neither query asks for an
        # order, so SQLite is free to return a different one when it picks a
        # different index, and comparing the rows as a set instead would stop
        # checking the order altogether. A table b-tree walk emits rows in
        # rowid order by construction, the seek emits the same bytes as that
        # walk (asserted above), and SQLite is the authority for what rowid
        # order contains.
        sq_tag, sq_line = sqlite_plan(db, sql)
        check(sq_tag == want_sqlite, "%s: SQLite reports the %s path" % (tag, want_sqlite),
              "EXPLAIN QUERY PLAN says %r" % sq_line)
        want = oracle_query(db, sql + " ORDER BY rowid", False)
        check(chosen.stdout == want, "%s: same bytes as SQLite in rowid order" % tag,
              "sqlr %d bytes, sqlite %d bytes\n    %r\n    %r"
              % (len(chosen.stdout), len(want), chosen.stdout[:200], want[:200]))
        if sq_tag != got:
            notes.append("%s\n      sqlr %s, SQLite %r" % (tag, got, sq_line))

    # Both halves non-zero. A corpus where every query fell back to the scan
    # would pass every row comparison above and prove nothing about indexes.
    check(seeks > 0 and scans > 0,
          "both access paths were exercised: %d seeks, %d fell back to a scan"
          % (seeks, scans), kind="self")
    check(best[0] >= 100,
          "the sharpest seek reads at least 100x fewer pages than its scan",
          "best was %.0fx -- %s" % (best[0], best[1]), kind="self")
    return seeks, scans

def overread_gate(work):
    """Plant a read one byte past an assembled index key and require ASan to
    see it. This is a gate on the SANITIZER, not on sqlr: a payload buffer
    rounded up to the next power of two leaves such a read inside live memory,
    ASan says nothing, and every bounds check in the file stops being checked
    by the sanitised run. rec_room allocates exactly the payload length for
    that reason, and the only way to know it still does is to plant the read.

    The fixture is as much the subject as the buffer is. ixovf's index keys are
    305 bytes on 512-byte pages, so every one of them overflows and is
    assembled into an exact allocation; the same plant against idx.db, whose
    keys are short enough to stay on their page, reports NOTHING, because the
    read then lands inside the buffer holding the whole file. That is measured,
    and it is in the README. A suite that planted this against the short-key
    fixture alone would have called the check green forever.

    Exit code 86: sqlr itself produces 0, 1 or 2 and nothing else, so 86 cannot
    be a crash, a usage error or a clean refusal. detect_leaks=0 because sqlr
    never frees its schema, and LeakSanitizer would otherwise exit 86 by
    itself and pass this gate with no plant in the tree at all.
    """
    cc = os.environ.get("CC", "cc")
    exe = os.path.join(work, "sqlr-overread")
    build = subprocess.run(
        [cc, "-std=c11", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
         "-DSQLR_OVERREAD", "-o", exe, os.path.join(ROOT, "sqlr.c"), "-lm"],
        capture_output=True)
    if build.returncode != 0:
        check(False, "overread: the planted-read build compiles",
              build.stderr.decode("utf-8", "replace")[:400], kind="robust")
        return
    sql = "SELECT k FROM lk WHERE k = '%s'" % ("L042-" + "x" * 300)
    p = subprocess.run([exe, os.path.join(work, "ixovf.db"), sql], capture_output=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:exitcode=86"))
    check(p.returncode == 86 and b"heap-buffer-overflow" in p.stderr,
          "overread: ASan sees a read one byte past an assembled index key",
          "exit %d, stderr %r" % (p.returncode, p.stderr[-300:]), kind="robust")

# ------------------------------------------------------------- robustness
# A file-format parser is fed untrusted bytes. Truncated and corrupted input
# must produce an error, not a crash and not a hang.

def truncation_sweep(src, cmds, psize, nspread):
    """Cut a valid database short at many offsets. Every page boundary matters:
    that is where a b-tree child pointer, an overflow link or a cell array stops
    existing. The requirement is an error, never a crash and never a hang."""
    size = os.path.getsize(src)
    victim = os.path.join(WORK, "trunc.db")
    with open(src, "rb") as f:
        whole = f.read()
    offsets = set(range(0, min(size, 130)))
    for pg in range(0, size // psize + 2):
        for d in (-1, 0, 1):
            o = pg * psize + d
            if 0 <= o <= size:
                offsets.add(o)
    step = max(1, size // nspread)
    offsets.update(range(0, size, step))

    # Cap the work, but spread the cap over the whole file instead of taking
    # the lowest offsets. `sorted(offsets)[:1400]` was a prefix: deep.db is
    # 1.7 MB and generates 10,274 candidate offsets, so the cap kept only the
    # first 13% of the file and ovlong.db the first 49%. Both fixtures exist
    # for what lives past that line -- deep.db for the interior pages of a
    # four-level b-tree, ovlong.db for the tail of a 200 KB overflow chain --
    # so the two sweeps that most needed the depth were the two that never
    # reached it. Keep every offset inside the 100-byte header and the page
    # after it, then take an even sample of the rest.
    CAP = 1400
    offsets = sorted(offsets)
    if len(offsets) > CAP:
        head = [o for o in offsets if o < 130]
        rest = [o for o in offsets if o >= 130]
        room = max(1, CAP - len(head))
        stride = max(1, len(rest) // room)
        offsets = head + rest[::stride][:room]
        # The last byte is a boundary of its own: a file cut to exactly its
        # own length is the one truncation that must still read correctly.
        if rest[-1] not in offsets:
            offsets.append(rest[-1])
    bad = []
    for off in offsets:
        with open(victim, "wb") as f:
            f.write(whole[:off])
        for c in cmds:
            p = run([victim] + c, timeout=30)
            if p.returncode not in (0, 1) or b"Sanitizer" in p.stderr or b"runtime error" in p.stderr:
                bad.append("truncate %s@%d %s -> rc=%d %r"
                           % (os.path.basename(src), off, c[0], p.returncode, p.stderr[:300]))
    check(not bad,
          "%s: %d truncation offsets x %d commands error cleanly"
          % (os.path.basename(src), len(offsets), len(cmds)),
          "\n    ".join(bad[:8]), kind="robust")


def corruption_sweep(src, cmds, trials, seed, limit=None):
    """Flip one byte and require an error, never a crash and never a hang."""
    with open(src, "rb") as f:
        whole = f.read()
    victim = os.path.join(WORK, "corrupt.db")
    rnd = random.Random(seed)
    bad = []
    for _ in range(trials):
        off = rnd.randrange(0, min(len(whole), limit) if limit else len(whole))
        b = bytearray(whole)
        b[off] = rnd.randrange(256)
        with open(victim, "wb") as f:
            f.write(bytes(b))
        c = rnd.choice(cmds)
        p = run([victim] + c, timeout=30)
        if p.returncode not in (0, 1) or b"Sanitizer" in p.stderr or b"runtime error" in p.stderr:
            bad.append("flip@%d %s -> rc=%d %r" % (off, c[0], p.returncode, p.stderr[:300]))
    check(not bad, "%s: %d single-byte corruptions error cleanly"
          % (os.path.basename(src), trials), "\n    ".join(bad[:8]), kind="robust")


def robustness():
    # types.db: 4 KiB pages, every storage class, one b-tree level.
    truncation_sweep(
        os.path.join(WORK, "types.db"),
        [[".tables"], [".schema"], ["SELECT * FROM allt"],
         ["SELECT i FROM allt WHERE i = 7"]], 4096, 120)
    # ovlong.db: 512-byte pages and a 200 KB value, so most of the file is a
    # single overflow chain and truncation always cuts it mid-link.
    truncation_sweep(os.path.join(WORK, "ovlong.db"),
                     [[".tables"], ["SELECT * FROM big"]], 512, 150)
    # deep.db: a four-level b-tree, so truncation removes interior pages.
    truncation_sweep(os.path.join(WORK, "deep.db"),
                     [[".schema"], ["SELECT * FROM d"],
                      ["SELECT id FROM d WHERE id = 50000"]], 512, 150)
    # ixovf.db: index interior pages, index leaves and the overflow chains
    #     hanging off both, so truncation cuts an index cell in half as well
    #     as a table one. The index reader has its own cell format and its
    #     own payload threshold; neither is reached by the three sweeps
    #     above, all of which run over table b-trees only.
    #     The fourth command is an index SEEK rather than a dump, so the
    #     pruning rules, the per-entry rowid and the table descent that
    #     follows each match all get the truncation sweep as well. Those are
    #     error paths of their own: an entry whose last column is not a rowid,
    #     and a table root that a truncated file no longer holds.
    ixseek = "SELECT k FROM lk WHERE k = '%s'" % ("L000-" + "x" * 300)
    truncation_sweep(os.path.join(WORK, "ixovf.db"),
                     [[".indexes"], [".index", "ix_long"],
                      [".index", "sqlite_autoindex_u_1"], [ixseek]], 512, 120)

    # Single-byte corruption across the first few pages: page types, cell
    # counts, cell pointers and record varints all live here, and a wrong value
    # must not become a wild read or an endless loop.
    corruption_sweep(os.path.join(WORK, "types.db"),
                     [[".dbinfo"], [".tables"], [".schema"], ["SELECT * FROM allt"],
                      ["SELECT i FROM allt WHERE i = 7"]], 500, 20261005, 16384)
    # The same over a database of index pages, with no limit on where the
    # flip lands, so index cell pointers and index payload lengths get the
    # same treatment. The sweep above cannot reach them: types.db has no
    # index, and .index is not one of its commands.
    corruption_sweep(os.path.join(WORK, "ixovf.db"),
                     [[".indexes"], [".index", "ix_long"],
                      [".index", "sqlite_autoindex_u_1"],
                      ["SELECT * FROM u WHERE v = 7"], [ixseek]], 300, 20261018)

    with open(os.path.join(WORK, "types.db"), "rb") as f:
        whole = f.read()
    cmds = [[".dbinfo"], [".tables"], [".schema"], ["SELECT * FROM allt"],
            ["SELECT i FROM allt WHERE i = 7"]]

    # Hand-built hostile files, including the cycles a visited-page bitmap has
    # to catch: a b-tree root that is its own child, and a self-linked overflow
    # page.
    for name, blob in (
        ("empty", b""),
        ("short", b"SQLite format 3\x00"),
        ("header-only", whole[:100]),
        ("bad-magic", b"NotASQLiteFile!\x00" + whole[16:8192]),
        ("zero-pagesize", whole[:16] + b"\x00\x00" + whole[18:8192]),
        ("odd-pagesize", whole[:16] + b"\x00\x07" + whole[18:8192]),
        ("utf16", whole[:56] + b"\x00\x00\x00\x02" + whole[60:8192]),
        ("huge-pagecount", whole[:28] + b"\x7f\xff\xff\xff" + whole[32:8192]),
        ("bad-fractions", whole[:21] + b"\x01\x01\x01" + whole[24:8192]),
        ("all-zero-pages", whole[:100] + b"\x00" * (8192 - 100)),
        ("interior-root-self-cycle",
         whole[:100] + b"\x05\x00\x00\x00\x00\x0f\xf8\x00\x00\x00\x00\x01"
         + whole[112:8192]),
    ):
        pth = os.path.join(WORK, "deg-%s.db" % name)
        with open(pth, "wb") as f:
            f.write(blob)
        okall = True
        for c in cmds:
            p = run([pth] + c, timeout=30)
            if p.returncode not in (0, 1) or b"Sanitizer" in p.stderr or b"runtime error" in p.stderr:
                okall = False
                check(False, "degenerate %s" % name,
                      "%s rc=%d %r" % (c[0], p.returncode, p.stderr[:300]),
                      kind="robust")
                break
        if okall:
            check(True, "degenerate %s errors cleanly" % name, kind="robust")


def index_page_corruptions():
    """A page's type byte and the rootpage sqlite_schema records for it are two
    separate claims about the same page, and a corrupt file makes them
    disagree. Decoding an index cell as a table cell would read a child
    pointer as a payload length, so the mismatch has to be an error.

    The third file here is the one that pays for the two visited-page bitmaps.
    An overflow chain is followed in the middle of a b-tree walk, and while
    both shared one bitmap, reading a long value erased the record of every
    page the walk had already visited. On a table that is nearly unreachable,
    because a table interior cell has no payload to overflow; on an index it
    is ordinary, because every interior cell carries a key. With one bitmap
    this file reports "deeper than 40 levels" after emitting the same pages
    forty times over; with two it is caught on the first revisit.
    """
    src = os.path.join(WORK, "ixovf.db")
    psize = 512
    conn = sqlite3.connect("file:%s?mode=ro" % src, uri=True)
    roots = dict(conn.execute(
        "SELECT name, rootpage FROM sqlite_schema WHERE name IN ('ix_long','lk')"))
    conn.close()
    with open(src, "rb") as f:
        whole = f.read()
    victim = os.path.join(WORK, "ixbad.db")
    iroot, troot = roots["ix_long"], roots["lk"]
    ioff, toff = (iroot - 1) * psize, (troot - 1) * psize

    # Before corrupting anything: the fixture must really contain a type 2
    # page and a type 5 page, read out of the file rather than inferred.
    check(whole[ioff] == 2,
          "ixovf: the ix_long root is an index interior page (type 2)",
          "page %d type byte is %d" % (iroot, whole[ioff]), kind="self")
    check(whole[toff] == 5,
          "ixovf: the lk root is a table interior page (type 5)",
          "page %d type byte is %d" % (troot, whole[toff]), kind="self")

    def attack(name, patch, argv, needle):
        b = bytearray(whole)
        patch(b)
        with open(victim, "wb") as f:
            f.write(bytes(b))
        p = run([victim] + argv, timeout=30)
        check(p.returncode == 1 and needle in p.stderr
              and b"Sanitizer" not in p.stderr and b"runtime error" not in p.stderr,
              "corrupt ixovf: %s" % name,
              "rc=%d err=%r" % (p.returncode, p.stderr[:300]), kind="robust")

    def settype(off, t):
        return lambda b: b.__setitem__(off, t)

    attack("an index root whose page says table leaf", settype(ioff, 13),
           [".index", "ix_long"], b"table b-tree page but an index")
    attack("a table root whose page says index leaf", settype(toff, 10),
           ["SELECT * FROM lk"], b"index b-tree page but a table")

    def selfchild(b):
        b[ioff + 8:ioff + 12] = iroot.to_bytes(4, "big")
    attack("an index interior page that is its own right-most child", selfchild,
           [".index", "ix_long"], b"revisits page")


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
          "database file is unchanged after reading it", kind="robust")
    check(not any(os.path.exists(src + s) for s in ("-journal", "-wal", "-shm")),
          "sqlr created no journal or WAL sidecar", kind="robust")


def main():
    if not os.path.exists(SQLR):
        print("sqlr binary not found at %s; run make first" % SQLR, file=sys.stderr)
        return 2
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)

    # try/finally rather than a tidy-up at the end: the corpus is several
    # megabytes of databases, and a suite that only cleans up when it passes
    # leaves them behind forever, because the next green run never looks. That
    # is how three sibling repos acquired permanent scratch directories, one of
    # which made a different tool unusable.
    try:
        print("building corpus with SQLite %s ..." % sqlite3.sqlite_version)
        build_corpus()
        n = len(os.listdir(WORK))
        print("%d databases, %d comparison cases" % (n, len(CASES)))

        compare_cases()
        print("access paths over those cases: "
              + ", ".join("%d %s" % (v, k) for k, v in sorted(paths.items())))
        refusals()
        seeks, scans = index_vs_scan(WORK)
        print("index against scan: %d queries seeked, %d fell back to a scan"
              % (seeks, scans))
        index_page_corruptions()
        overread_gate(WORK)
        readonly_check()
        print("robustness: truncating and corrupting ...")
        robustness()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)

    if notes:
        print("\nSQLITE PICKED A DIFFERENT ACCESS PATH (rows still compared):")
        for n in notes:
            print("  - " + n)
    if failures:
        print("\nFAILURES:")
        for f in failures:
            print("  - " + f)
    print()
    for k in KINDS:
        print("%4d %s, %d failed" % (passed[k], KIND_WHAT[k], failed[k]))
    bad = sum(failed.values())
    print("%4d checks, %d failed" % (sum(passed.values()) + bad, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
