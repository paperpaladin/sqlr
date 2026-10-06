/* sqlr -- a read-only SQLite 3 database file parser, from scratch.
 *
 * Nothing here links libsqlite3. Everything below is a direct reading of the
 * on-disk format: the 100-byte header, table b-tree pages, the record encoding,
 * and the sqlite_schema table. The file is opened "rb" and never written.
 *
 * Usage:
 *   sqlr DB.sqlite .dbinfo
 *   sqlr DB.sqlite .tables
 *   sqlr DB.sqlite .schema
 *
 * Exit status: 0 ok, 1 error (bad file, bad query), 2 usage.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ errors */

static char g_err[512];

static int err(const char *fmt, ...) {
	va_list a;
	va_start(a, fmt);
	vsnprintf(g_err, sizeof g_err, fmt, a);
	va_end(a);
	return -1;
}

static void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) {
		fprintf(stderr, "sqlr: out of memory\n");
		exit(1);
	}
	return p;
}

/* ------------------------------------------------------------- byte reading */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static uint32_t be32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* SQLite's varint: big-endian, 1..9 bytes. The high bit of each of the first
 * eight bytes is a "more follows" flag and the low seven bits are payload, so
 * eight bytes carry 56 bits; if a ninth byte is needed it contributes all eight
 * of its bits, for 64 in total. That asymmetry is the part nobody guesses, and
 * it is why a 9-byte varint has no continuation bit to check.
 * Returns bytes consumed, or -1 if the varint would run past `end`. */
static int varint(const uint8_t *p, const uint8_t *end, uint64_t *out) {
	uint64_t r = 0;
	for (int i = 0; i < 8; i++) {
		if (p + i >= end) return -1;
		r = (r << 7) | (p[i] & 0x7f);
		if (!(p[i] & 0x80)) {
			*out = r;
			return i + 1;
		}
	}
	if (p + 8 >= end) return -1;
	*out = (r << 8) | p[8];
	return 9;
}

/* n-byte big-endian two's-complement integer, for serial types 1-6 (widths 1,
 * 2, 3, 4, 6, 8). The sign lives in the top bit of the first byte, so start
 * from a signed char and shift the rest in. */
static int64_t be_int(const uint8_t *p, int n) {
	int64_t v = (int8_t)p[0];
	for (int i = 1; i < n; i++) v = (v << 8) | p[i];
	return v;
}

static double be_double(const uint8_t *p) {
	uint64_t u = 0;
	double d;
	for (int i = 0; i < 8; i++) u = (u << 8) | p[i];
	memcpy(&d, &u, 8);
	return d;
}

/* --------------------------------------------------------------- open a file */

typedef struct {
	uint8_t *d;
	size_t n;
	uint32_t psize;  /* page size in bytes */
	uint32_t usable; /* psize minus the per-page reserved region */
	uint32_t npages;
} Db;

/* shortcut: the whole file is slurped into RAM. Ceiling is available memory;
 * upgrade path is mmap(2) (still read-only) or a small LRU page cache. */
static int db_open(Db *db, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return err("%s: %s", path, strerror(errno));
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return err("%s: not seekable", path);
	}
	long sz = ftell(f);
	rewind(f);
	if (sz < 0) {
		fclose(f);
		return err("%s: cannot size file", path);
	}
	if (sz < 100) {
		fclose(f);
		return err("%s: %ld bytes, too short for a 100-byte SQLite header", path, sz);
	}
	db->d = xmalloc((size_t)sz);
	db->n = (size_t)sz;
	if (fread(db->d, 1, db->n, f) != db->n) {
		fclose(f);
		free(db->d);
		return err("%s: short read", path);
	}
	fclose(f);

	if (memcmp(db->d, "SQLite format 3", 16) != 0)
		return err("%s: not a SQLite 3 database (bad magic)", path);

	/* Bytes 16-17 hold the page size, except that 1 means 65536 -- the value
	 * does not fit in two bytes, so the format special-cases it. */
	uint32_t raw = be16(db->d + 16);
	db->psize = (raw == 1) ? 65536u : raw;
	if (db->psize < 512 || (db->psize & (db->psize - 1)))
		return err("bad page size %u (must be a power of two, >= 512)", db->psize);

	uint32_t reserved = db->d[20];
	if (reserved >= db->psize) return err("reserved bytes %u >= page size", reserved);
	db->usable = db->psize - reserved;
	if (db->usable < 480) return err("usable page size %u too small", db->usable);

	/* Bytes 21-23 are fixed by the format. A file where they differ is not
	 * something this parser's cell-overflow math is valid for. */
	if (db->d[21] != 64 || db->d[22] != 32 || db->d[23] != 32)
		return err("unsupported payload fractions %u/%u/%u", db->d[21], db->d[22], db->d[23]);

	/* The header's page count (bytes 28-31) is only authoritative when the
	 * change counter equals version-valid-for; otherwise an old SQLite wrote
	 * the file and the size must come from the file length. We additionally
	 * clamp to what is actually present, which is what makes a truncated file
	 * report "page out of range" instead of reading past the buffer. */
	uint32_t hdr_pages = be32(db->d + 28);
	uint32_t have = (uint32_t)(db->n / db->psize);
	db->npages = (hdr_pages && hdr_pages <= have) ? hdr_pages : have;
	if (db->npages == 0)
		return err("file holds no complete %u-byte page (%zu bytes)", db->psize, db->n);

	uint32_t enc = be32(db->d + 56);
	if (enc > 1) /* shortcut: UTF-16 databases are rejected, not transcoded. */
		return err("text encoding %u (UTF-16) unsupported; only UTF-8", enc);
	return 0;
}

static const uint8_t *page_at(Db *db, uint32_t pgno) {
	if (pgno < 1 || pgno > db->npages) {
		err("page %u out of range (database has %u pages)", pgno, db->npages);
		return NULL;
	}
	return db->d + (size_t)(pgno - 1) * db->psize;
}

/* ------------------------------------------------------------------- values */

typedef enum { V_NULL, V_INT, V_REAL, V_TEXT, V_BLOB } Vt;

typedef struct {
	Vt t;
	int64_t i;
	double r;
	const uint8_t *s; /* points into the assembled payload */
	uint32_t n;
} Val;

#define MAXCOL 512

/* The serial-type table from the format spec. Types 0 and 8/9 occupy no bytes
 * in the body at all: 8 and 9 encode the literal integers 0 and 1, which is why
 * a row of booleans costs one byte per column in the header and nothing else.
 * Even types >= 12 are blobs of (t-12)/2 bytes, odd types >= 13 are text of
 * (t-13)/2 bytes. 10 and 11 are reserved and must not appear. */
static int serial_len(uint64_t t, uint32_t *len) {
	switch (t) {
	case 0: case 8: case 9: *len = 0; return 0;
	case 1: *len = 1; return 0;
	case 2: *len = 2; return 0;
	case 3: *len = 3; return 0;
	case 4: *len = 4; return 0;
	case 5: *len = 6; return 0;
	case 6: case 7: *len = 8; return 0;
	case 10: case 11: return err("reserved serial type %" PRIu64 " in record", t);
	}
	if (t > 0x7fffffffu) return err("absurd serial type %" PRIu64, t);
	*len = (uint32_t)((t - 12) / 2);
	return 0;
}

/* A record is: a varint giving the total header size, then one varint serial
 * type per column, then the bodies back to back. The header size counts itself,
 * which is the subtle bit -- you cannot find the first body byte without first
 * decoding a varint whose value includes the varint's own width. */
static int rec_parse(const uint8_t *p, size_t plen, Val *v, int *nout) {
	uint64_t hdrlen;
	int k = varint(p, p + plen, &hdrlen);
	if (k < 0) return err("record header size varint runs past end of payload");
	if (hdrlen < (uint64_t)k || hdrlen > plen)
		return err("record header size %" PRIu64 " outside %zu-byte payload", hdrlen, plen);

	const uint8_t *ht = p + k, *hend = p + hdrlen;
	const uint8_t *body = p + hdrlen, *end = p + plen;
	int n = 0;

	while (ht < hend) {
		uint64_t st;
		int m = varint(ht, hend, &st);
		if (m < 0) return err("serial type varint runs past end of record header");
		ht += m;
		uint32_t blen;
		if (serial_len(st, &blen) < 0) return -1;
		if (n >= MAXCOL) return err("record has more than %d columns", MAXCOL);
		if ((size_t)(end - body) < blen)
			return err("record body is %zu bytes short of serial type %" PRIu64,
			           (size_t)(blen - (end - body)), st);
		Val *o = &v[n++];
		memset(o, 0, sizeof *o);
		switch (st) {
		case 0: o->t = V_NULL; break;
		case 8: o->t = V_INT; o->i = 0; break;
		case 9: o->t = V_INT; o->i = 1; break;
		case 7: o->t = V_REAL; o->r = be_double(body); break;
		case 1: case 2: case 3: case 4: case 5: case 6:
			o->t = V_INT;
			o->i = be_int(body, (int)blen);
			break;
		default:
			o->t = (st & 1) ? V_TEXT : V_BLOB;
			o->s = body;
			o->n = blen;
		}
		body += blen;
	}
	*nout = n;
	return 0;
}

/* --------------------------------------------------------- payload assembly */

/* Per-scan scratch: a growable record buffer and a visited-page bitmap. The
 * bitmap is what stops a corrupt file whose page pointers form a cycle from
 * looping forever -- a read-only parser must terminate on any input. */
typedef struct {
	Db *db;
	uint8_t *rec;
	size_t reccap;
	uint8_t *seen;
	size_t seenbytes;
} Scan;

static void scan_init(Scan *s, Db *db) {
	s->db = db;
	s->rec = NULL;
	s->reccap = 0;
	s->seenbytes = db->npages / 8 + 1;
	s->seen = xmalloc(s->seenbytes);
}

static void scan_free(Scan *s) {
	free(s->rec);
	free(s->seen);
}

static void seen_clear(Scan *s) { memset(s->seen, 0, s->seenbytes); }

static int seen_mark(Scan *s, uint32_t pg) {
	size_t byte = pg / 8;
	uint8_t bit = (uint8_t)(1u << (pg % 8));
	if (byte >= s->seenbytes) return 0;
	if (s->seen[byte] & bit) return 1; /* already visited: a cycle */
	s->seen[byte] |= bit;
	return 0;
}

static uint8_t *rec_room(Scan *s, size_t need) {
	if (need > s->reccap) {
		s->reccap = need < 512 ? 512 : need;
		free(s->rec);
		s->rec = xmalloc(s->reccap);
	}
	return s->rec;
}

/* How much of a leaf cell's payload lives on the leaf page itself.
 *
 * X is the largest payload a leaf cell may hold without spilling: the usable
 * size minus 35, the worst-case cell overhead. If the payload is bigger,
 * SQLite does NOT simply store X bytes locally. It stores either M bytes or
 * K bytes, chosen so the final overflow page is as full as possible while the
 * local part stays >= M. M is the minimum local payload, 32/255ths of the
 * usable space minus the header allowance; K is M plus however much of the
 * surplus does not divide evenly into whole overflow pages (each of which
 * carries usable-4 bytes, the 4 being the next-page pointer). If that K still
 * fits on the page, use it; otherwise fall back to M. Getting this wrong
 * silently misreads every long value in the database, so it is spelled out. */
static uint32_t local_payload(uint32_t usable, uint64_t p) {
	uint32_t x = usable - 35;
	if (p <= x) return (uint32_t)p;
	uint32_t m = ((usable - 12) * 32 / 255) - 23;
	uint32_t k = m + (uint32_t)((p - m) % (usable - 4));
	return (k <= x) ? k : m;
}

/* Copy a cell's full payload (local part plus the overflow chain) into the
 * scan's record buffer. */
static int payload_read(Scan *s, const uint8_t *cell_payload, uint64_t plen,
                        size_t page_room, const uint8_t **out) {
	Db *db = s->db;
	uint32_t local = local_payload(db->usable, plen);

	/* A payload cannot be larger than the bytes the file actually contains;
	 * checking that first turns a corrupt length into an error instead of a
	 * multi-gigabyte malloc. */
	if (plen > (uint64_t)db->npages * db->usable)
		return err("cell payload of %" PRIu64 " bytes exceeds the whole database", plen);
	if (local > page_room)
		return err("cell payload of %u local bytes runs past the end of its page", local);

	if (plen == local) { /* no overflow: read it in place, no copy */
		*out = cell_payload;
		return 0;
	}

	uint8_t *buf = rec_room(s, (size_t)plen);
	memcpy(buf, cell_payload, local);
	size_t got = local;
	uint32_t next = be32(cell_payload + local);

	/* The 4 bytes after the local payload are the first overflow page number;
	 * each overflow page is 4 bytes of "next page, or 0" then raw payload. */
	seen_clear(s);
	while (got < plen) {
		if (next == 0) return err("overflow chain ended %zu bytes early", (size_t)plen - got);
		if (seen_mark(s, next)) return err("overflow chain loops back to page %u", next);
		const uint8_t *op = page_at(db, next);
		if (!op) return -1;
		size_t chunk = db->usable - 4;
		if (chunk > plen - got) chunk = plen - got;
		memcpy(buf + got, op + 4, chunk);
		got += chunk;
		next = be32(op);
	}
	*out = buf;
	return 0;
}

/* ------------------------------------------------------------ b-tree walking */

typedef int (*RowFn)(void *ctx, int64_t rowid, const uint8_t *payload, size_t plen);

/* shortcut: table b-trees only (page types 5 and 13). Index b-trees (2 and 10)
 * are recognised and refused; nothing here needs them because every query is a
 * full table scan. Upgrade path: index leaf cells are the same record format
 * with the rowid as the trailing column, so adding them is a new cell decoder
 * plus a seek, not a redesign. */
static int walk(Scan *s, uint32_t pgno, int depth, RowFn fn, void *ctx) {
	Db *db = s->db;
	if (depth > 40) return err("b-tree deeper than 40 levels; refusing to recurse");
	if (seen_mark(s, pgno)) return err("b-tree revisits page %u (corrupt)", pgno);

	const uint8_t *pg = page_at(db, pgno);
	if (!pg) return -1;
	/* Page 1 is unique: the 100-byte database header sits in front of its
	 * b-tree header, but cell pointers are still offsets from the page start. */
	uint32_t off = (pgno == 1) ? 100 : 0;
	const uint8_t *h = pg + off;
	uint8_t type = h[0];
	if (type != 5 && type != 13) {
		if (type == 2 || type == 10)
			return err("page %u is an index b-tree page; sqlr reads table b-trees only", pgno);
		return err("page %u has unknown b-tree page type %u", pgno, type);
	}
	uint32_t ncell = be16(h + 3);
	uint32_t hsize = (type == 5) ? 12 : 8;
	uint32_t cellptr = off + hsize;
	if (cellptr + 2 * ncell > db->usable)
		return err("page %u claims %u cells, which overruns the page", pgno, ncell);

	for (uint32_t i = 0; i < ncell; i++) {
		uint32_t co = be16(pg + cellptr + 2 * i);
		if (co < cellptr + 2 * ncell || co >= db->usable)
			return err("page %u cell %u offset %u outside the content area", pgno, i, co);
		const uint8_t *c = pg + co;
		const uint8_t *cend = pg + db->usable;

		if (type == 5) {
			/* Interior table cell: 4-byte left child page number, then a
			 * varint rowid we do not need for a full scan. */
			if (cend - c < 4) return err("page %u cell %u truncated", pgno, i);
			if (walk(s, be32(c), depth + 1, fn, ctx) < 0) return -1;
		} else {
			/* Leaf table cell: varint total payload length, varint rowid,
			 * then the payload. */
			uint64_t plen, rowid;
			int k = varint(c, cend, &plen);
			if (k < 0) return err("page %u cell %u: bad payload length varint", pgno, i);
			c += k;
			k = varint(c, cend, &rowid);
			if (k < 0) return err("page %u cell %u: bad rowid varint", pgno, i);
			c += k;
			const uint8_t *pay;
			if (payload_read(s, c, plen, (size_t)(cend - c), &pay) < 0) return -1;
			if (fn(ctx, (int64_t)rowid, pay, (size_t)plen) < 0) return -1;
		}
	}
	if (type == 5) {
		/* The right-most child hangs off the page header, not the cell array;
		 * visiting it last is what keeps a scan in rowid order. */
		if (walk(s, be32(h + 8), depth + 1, fn, ctx) < 0) return -1;
	}
	return 0;
}

static int scan_table(Scan *s, uint32_t root, RowFn fn, void *ctx) {
	seen_clear(s);
	return walk(s, root, 0, fn, ctx);
}

/* ------------------------------------------------------- string helpers */

static char *dupn(const uint8_t *p, size_t n) {
	char *r = xmalloc(n + 1);
	if (n) memcpy(r, p, n);
	r[n] = 0;
	return r;
}

/* ------------------------------------------------------------------- schema */

typedef struct {
	char *name;
	char *sql;
	uint32_t root;
} Table;

typedef struct {
	Table *t;
	int n, cap;
	char **sql; /* every schema row's SQL text, in sqlite_schema order, for .schema */
	int nsql, sqlcap;
} Schema;

/* Each sqlite_schema row is (type, name, tbl_name, rootpage, sql). */
static int schema_row(void *ctx, int64_t rowid, const uint8_t *pay, size_t plen) {
	Schema *sc = ctx;
	Val v[MAXCOL];
	int n;
	(void)rowid;
	if (rec_parse(pay, plen, v, &n) < 0) return -1;
	if (n < 5) return err("sqlite_schema row has %d columns, expected 5", n);

	if (v[4].t == V_TEXT) {
		if (sc->nsql == sc->sqlcap) {
			sc->sqlcap = sc->sqlcap ? sc->sqlcap * 2 : 16;
			sc->sql = realloc(sc->sql, (size_t)sc->sqlcap * sizeof *sc->sql);
			if (!sc->sql) exit(1);
		}
		sc->sql[sc->nsql++] = dupn(v[4].s, v[4].n);
	}
	if (v[0].t != V_TEXT || v[0].n != 5 || memcmp(v[0].s, "table", 5) != 0) return 0;
	if (v[1].t != V_TEXT) return err("sqlite_schema row has a non-text name");
	if (v[3].t != V_INT) return err("sqlite_schema row has a non-integer rootpage");
	if (v[3].i <= 0) return 0; /* virtual table: no b-tree of its own */

	if (sc->n == sc->cap) {
		sc->cap = sc->cap ? sc->cap * 2 : 16;
		sc->t = realloc(sc->t, (size_t)sc->cap * sizeof *sc->t);
		if (!sc->t) exit(1);
	}
	Table *t = &sc->t[sc->n];
	memset(t, 0, sizeof *t);
	t->name = dupn(v[1].s, v[1].n);
	t->root = (uint32_t)v[3].i;
	t->sql = (v[4].t == V_TEXT) ? dupn(v[4].s, v[4].n) : NULL;
	sc->n++;
	return 0;
}

static int schema_load(Scan *s, Schema *sc) {
	memset(sc, 0, sizeof *sc);
	/* sqlite_schema is always rooted at page 1. */
	return scan_table(s, 1, schema_row, sc);
}

/* ------------------------------------------------------------------- main */

static int cmp_name(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int usage(void) {
	fputs("usage: sqlr DB.sqlite {.dbinfo | .tables | .schema}\n", stderr);
	return 2;
}

int main(int argc, char **argv) {
	if (argc != 3) return usage();
	const char *path = argv[1], *cmd = argv[2];

	Db db;
	if (db_open(&db, path) < 0) {
		fprintf(stderr, "sqlr: %s\n", g_err);
		return 1;
	}
	if (!strcmp(cmd, ".dbinfo")) {
		/* Everything here is a field of the 100-byte header, so this answers
		 * even for a file whose b-trees are corrupt. Each line has a PRAGMA
		 * that reports the same number, which is how the harness checks them. */
		printf("page_size\t%u\n", db.psize);          /* PRAGMA page_size */
		printf("page_count\t%u\n", db.npages);        /* PRAGMA page_count */
		printf("reserved_bytes\t%u\n", db.psize - db.usable);
		printf("text_encoding\tUTF-8\n");             /* PRAGMA encoding */
		printf("schema_cookie\t%u\n", be32(db.d + 40));   /* PRAGMA schema_version */
		printf("user_version\t%u\n", be32(db.d + 60));    /* PRAGMA user_version */
		printf("freelist_pages\t%u\n", be32(db.d + 36));  /* PRAGMA freelist_count */
		free(db.d);
		return fflush(stdout) == 0 ? 0 : 1;
	}

	Scan sc;
	scan_init(&sc, &db);
	Schema schema;
	if (schema_load(&sc, &schema) < 0) {
		fprintf(stderr, "sqlr: %s\n", g_err);
		return 1;
	}

	int rc = 0;
	if (!strcmp(cmd, ".tables")) {
		/* sqlite3's .tables hides the internal sqlite_* tables and sorts. */
		char **names = xmalloc((size_t)(schema.n ? schema.n : 1) * sizeof *names);
		int k = 0;
		for (int j = 0; j < schema.n; j++)
			if (strncmp(schema.t[j].name, "sqlite_", 7) != 0) names[k++] = schema.t[j].name;
		qsort(names, (size_t)k, sizeof *names, cmp_name);
		for (int j = 0; j < k; j++) printf("%s\n", names[j]);
		free(names);
	} else if (!strcmp(cmd, ".schema")) {
		/* In sqlite_schema order, each statement terminated with a semicolon,
		 * which is what sqlite3's .schema emits. */
		for (int j = 0; j < schema.nsql; j++) printf("%s;\n", schema.sql[j]);
	} else {
		fprintf(stderr, "sqlr: unsupported command: %s\n", cmd);
		scan_free(&sc);
		free(db.d);
		return usage();
	}
	if (fflush(stdout) != 0) {
		fprintf(stderr, "sqlr: write failed: %s\n", strerror(errno));
		rc = 1;
	}
	scan_free(&sc);
	free(db.d);
	return rc;
}
