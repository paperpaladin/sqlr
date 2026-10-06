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

    if failures:
        print("\nFAILURES:")
        for f in failures:
            print("  - " + f)
    print("\n%d agree, %d disagree" % (agree, disagree))
    return 1 if disagree else 0


if __name__ == "__main__":
    sys.exit(main())
