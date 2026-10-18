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
 *   sqlr [--json] DB.sqlite 'SELECT a,b FROM t WHERE a = 1'
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
 * 2, 3, 4, 6, 8). Accumulate in unsigned and sign-extend from bit 8n-1 at the
 * end: shifting a negative signed value left is undefined behaviour, and
 * UBSan in `make test-asan` found exactly that in the first version of this. */
static int64_t be_int(const uint8_t *p, int n) {
	uint64_t u = 0;
	for (int i = 0; i < n; i++) u = (u << 8) | p[i];
	if (n < 8 && (u & ((uint64_t)1 << (n * 8 - 1)))) u |= ~(uint64_t)0 << (n * 8);
	return (int64_t)u;
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
	uint8_t *seen;    /* pages visited by the b-tree walk */
	uint8_t *ovfseen; /* pages visited by the current overflow chain */
	size_t seenbytes;
} Scan;

static void scan_init(Scan *s, Db *db) {
	s->db = db;
	s->rec = NULL;
	s->reccap = 0;
	s->seenbytes = db->npages / 8 + 1;
	s->seen = xmalloc(s->seenbytes);
	/* Two bitmaps, not one. An overflow chain is followed in the middle of a
	 * b-tree walk, so sharing the bitmap meant reading one long value wiped
	 * the record of every page the walk had already visited and the cycle
	 * check stopped working for the rest of the page. Found by reading, and
	 * now reachable: an index cell carries a key, so an index interior page
	 * reads overflow chains between descending into its children. */
	s->ovfseen = xmalloc(s->seenbytes);
}

static void scan_free(Scan *s) {
	free(s->rec);
	free(s->seen);
	free(s->ovfseen);
}

static int bit_mark(uint8_t *bm, size_t nbytes, uint32_t pg) {
	size_t byte = pg / 8;
	uint8_t bit = (uint8_t)(1u << (pg % 8));
	if (byte >= nbytes) return 0;
	if (bm[byte] & bit) return 1; /* already visited: a cycle */
	bm[byte] |= bit;
	return 0;
}

/* Exactly `need` bytes, never a rounded-up or a kept-around larger buffer.
 * A buffer bigger than its data turns the sanitizer off: a read one byte past
 * a 310-byte payload sitting in a 512-byte allocation lands inside live
 * memory and ASan says nothing. Measured here rather than assumed -- a
 * deliberate one-past-the-end read of an assembled payload is silent with the
 * rounded-up buffer this replaced, and reported with this one. Index payloads
 * are what made it worth fixing: an index page spills at about a fifth of the
 * length a table leaf does, so almost every long key goes through here. */
static uint8_t *rec_room(Scan *s, size_t need) {
	if (need != s->reccap) {
		free(s->rec);
		s->rec = xmalloc(need ? need : 1);
		s->reccap = need;
	}
	return s->rec;
}

/* How much of a cell's payload lives on the cell's own page.
 *
 * X is the largest payload a cell may hold without spilling. On a table leaf
 * it is the usable size minus 35, the worst-case cell overhead; on an index
 * page, leaf or interior, it is instead 64/255ths of the usable space minus
 * the same 23-byte allowance M uses, which is far smaller -- an index key of
 * a couple of hundred bytes already overflows on a 512-byte page where a
 * table row of the same size does not. If the payload is bigger,
 * SQLite does NOT simply store X bytes locally. It stores either M bytes or
 * K bytes, chosen so the final overflow page is as full as possible while the
 * local part stays >= M. M is the minimum local payload, 32/255ths of the
 * usable space minus the header allowance; K is M plus however much of the
 * surplus does not divide evenly into whole overflow pages (each of which
 * carries usable-4 bytes, the 4 being the next-page pointer). If that K still
 * fits on the page, use it; otherwise fall back to M. Getting this wrong
 * silently misreads every long value in the database, so it is spelled out. */
static uint32_t local_payload(uint32_t usable, uint64_t p, int is_index) {
	uint32_t x = is_index ? ((usable - 12) * 64 / 255) - 23 : usable - 35;
	if (p <= x) return (uint32_t)p;
	uint32_t m = ((usable - 12) * 32 / 255) - 23;
	uint32_t k = m + (uint32_t)((p - m) % (usable - 4));
	return (k <= x) ? k : m;
}

/* Copy a cell's full payload (local part plus the overflow chain) into the
 * scan's record buffer. */
static int payload_read(Scan *s, const uint8_t *cell_payload, uint64_t plen,
                        size_t page_room, int is_index, const uint8_t **out) {
	Db *db = s->db;
	uint32_t local = local_payload(db->usable, plen, is_index);

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
	memset(s->ovfseen, 0, s->seenbytes);
	while (got < plen) {
		if (next == 0) return err("overflow chain ended %zu bytes early", (size_t)plen - got);
		if (bit_mark(s->ovfseen, s->seenbytes, next))
			return err("overflow chain loops back to page %u", next);
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

/* One walker serves both kinds of b-tree. They differ in exactly three places:
 * the page type bytes, whether an interior cell carries a key as well as a
 * child pointer, and the local-payload threshold.
 *
 * The traversal order is the key order. On an index interior page a cell's
 * own key sorts AFTER everything in that cell's left child, so the child is
 * descended first and the key emitted afterwards; the right-most child, which
 * hangs off the page header rather than the cell array, comes last. A table
 * interior cell has a rowid but no payload, so there is nothing to emit in
 * between and only the order of the descents matters.
 *
 * shortcut: an index on a WITHOUT ROWID table ends each entry with the
 * table's primary key instead of a rowid, and nothing here knows that. Those
 * tables are refused in parse_create, which is what keeps this honest; the
 * upgrade path is PLAN item 08. */
static int walk(Scan *s, uint32_t pgno, int depth, int want_index, RowFn fn, void *ctx) {
	Db *db = s->db;
	if (depth > 40) return err("b-tree deeper than 40 levels; refusing to recurse");
	if (bit_mark(s->seen, s->seenbytes, pgno))
		return err("b-tree revisits page %u (corrupt)", pgno);

	const uint8_t *pg = page_at(db, pgno);
	if (!pg) return -1;
	/* Page 1 is unique: the 100-byte database header sits in front of its
	 * b-tree header, but cell pointers are still offsets from the page start. */
	uint32_t off = (pgno == 1) ? 100 : 0;
	const uint8_t *h = pg + off;
	uint8_t type = h[0];
	if (type != 2 && type != 5 && type != 10 && type != 13)
		return err("page %u has unknown b-tree page type %u", pgno, type);
	int is_index = (type == 2 || type == 10);
	int interior = (type == 2 || type == 5);
	/* A root page of the wrong kind means the schema and the file disagree,
	 * which is corruption, not something to decode on a guess. */
	if (is_index != want_index)
		return err("page %u is %s b-tree page but %s b-tree was expected", pgno,
		           is_index ? "an index" : "a table", want_index ? "an index" : "a table");
	uint32_t ncell = be16(h + 3);
	uint32_t hsize = interior ? 12 : 8;
	uint32_t cellptr = off + hsize;
	if (cellptr + 2 * ncell > db->usable)
		return err("page %u claims %u cells, which overruns the page", pgno, ncell);

	for (uint32_t i = 0; i < ncell; i++) {
		uint32_t co = be16(pg + cellptr + 2 * i);
		if (co < cellptr + 2 * ncell || co >= db->usable)
			return err("page %u cell %u offset %u outside the content area", pgno, i, co);
		const uint8_t *c = pg + co;
		const uint8_t *cend = pg + db->usable;

		if (interior) {
			/* Both interior cell formats start with the 4-byte left child. */
			if (cend - c < 4) return err("page %u cell %u truncated", pgno, i);
			if (walk(s, be32(c), depth + 1, want_index, fn, ctx) < 0) return -1;
			if (!is_index) continue; /* table interior: the rowid is not needed */
			c += 4;
		}
		/* A leaf cell, or the key half of an index interior cell: varint
		 * payload length, then (table only) a varint rowid, then the payload. */
		uint64_t plen, rowid = 0;
		int k = varint(c, cend, &plen);
		if (k < 0) return err("page %u cell %u: bad payload length varint", pgno, i);
		c += k;
		if (!is_index) {
			k = varint(c, cend, &rowid);
			if (k < 0) return err("page %u cell %u: bad rowid varint", pgno, i);
			c += k;
		}
		const uint8_t *pay;
		if (payload_read(s, c, plen, (size_t)(cend - c), is_index, &pay) < 0) return -1;
		if (fn(ctx, (int64_t)rowid, pay, (size_t)plen) < 0) return -1;
	}
	if (interior) {
		/* The right-most child hangs off the page header, not the cell array;
		 * visiting it last is what keeps a scan in key order. */
		if (walk(s, be32(h + 8), depth + 1, want_index, fn, ctx) < 0) return -1;
	}
	return 0;
}

static int scan_table(Scan *s, uint32_t root, RowFn fn, void *ctx) {
	memset(s->seen, 0, s->seenbytes);
	return walk(s, root, 0, 0, fn, ctx);
}

static int scan_index(Scan *s, uint32_t root, RowFn fn, void *ctx) {
	memset(s->seen, 0, s->seenbytes);
	return walk(s, root, 0, 1, fn, ctx);
}

/* ------------------------------------------------------- string helpers */

static char *dupn(const uint8_t *p, size_t n) {
	char *r = xmalloc(n + 1);
	if (n) memcpy(r, p, n);
	r[n] = 0;
	return r;
}

static int ci_eq(const char *a, const char *b) {
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
	return *a == *b;
}

static char *upper_dup(const char *s) {
	char *r = xmalloc(strlen(s) + 1);
	for (size_t i = 0; s[i]; i++) r[i] = (char)toupper((unsigned char)s[i]);
	r[strlen(s)] = 0;
	return r;
}

/* ----------------------------------------------------------- type affinity */

typedef enum { AFF_BLOB, AFF_TEXT, AFF_NUM, AFF_INT, AFF_REAL } Aff;

/* The declared-type to affinity rules, in the order the spec applies them. */
static Aff affinity(const char *ty) {
	char *u = upper_dup(ty ? ty : "");
	Aff a;
	if (strstr(u, "INT")) a = AFF_INT;
	else if (strstr(u, "CHAR") || strstr(u, "CLOB") || strstr(u, "TEXT")) a = AFF_TEXT;
	else if (strstr(u, "BLOB") || !*u) a = AFF_BLOB;
	else if (strstr(u, "REAL") || strstr(u, "FLOA") || strstr(u, "DOUB")) a = AFF_REAL;
	else a = AFF_NUM;
	free(u);
	return a;
}

/* ------------------------------------------------------------------- schema */

typedef struct {
	char *name;
	char *sql;
	uint32_t root;
	int ncol;
	char *col[MAXCOL];
	char *type[MAXCOL];
	Aff aff[MAXCOL];
	int ipk; /* index of the INTEGER PRIMARY KEY column, or -1 */
} Table;

/* An index as sqlite_schema records it. `sql` is NULL for the indexes SQLite
 * creates for itself (sqlite_autoindex_*, one per UNIQUE or PRIMARY KEY
 * constraint), which is why it is kept separately from the name. */
typedef struct {
	char *name;
	char *tbl;
	char *sql;
	uint32_t root;
} Idx;

typedef struct {
	Table *t;
	int n, cap;
	Idx *ix;
	int nix, ixcap;
	char **sql; /* every schema row's SQL text, in sqlite_schema order, for .schema */
	int nsql, sqlcap;
} Schema;

/* Pull one identifier off `*pp`, handling "quoted", [bracketed], `backticked`
 * and bare names. */
static char *ident(const char **pp) {
	const char *p = *pp;
	while (*p && isspace((unsigned char)*p)) p++;
	char close = 0;
	if (*p == '"') close = '"';
	else if (*p == '`') close = '`';
	else if (*p == '[') close = ']';
	if (close) {
		p++;
		char *out = xmalloc(strlen(p) + 1);
		size_t k = 0;
		while (*p) {
			if (*p == close) {
				if (close != ']' && p[1] == close) { out[k++] = *p; p += 2; continue; }
				p++;
				break;
			}
			out[k++] = *p++;
		}
		out[k] = 0;
		*pp = p;
		return out;
	}
	const char *st = p;
	while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '$')) p++;
	if (p == st) return NULL;
	char *out = dupn((const uint8_t *)st, (size_t)(p - st));
	*pp = p;
	return out;
}

/* Parse the column list out of a CREATE TABLE statement.
 *
 * shortcut: this is a shape-matcher, not an SQL parser. It finds the outermost
 * parenthesised list, splits it on top-level commas, takes the first token of
 * each part as a column name and the run of bare words after it as the declared
 * type, and skips parts that begin with a table-constraint keyword. That covers
 * everything sqlite3 emits for ordinary CREATE TABLE. Upgrade path: a real
 * tokeniser; the ceiling today is exotic DDL (expressions in DEFAULT that
 * contain unbalanced quotes, generated columns). */
static int parse_create(Table *t, const char *sql) {
	const char *p = strchr(sql, '(');
	if (!p) return err("table %s: CREATE statement has no column list", t->name);
	p++;
	int depth = 0;
	const char *part = p;
	for (;; p++) {
		if (*p == '\'' || *p == '"' || *p == '`') {
			char qc = *p++;
			while (*p && *p != qc) p++;
			if (!*p) return err("table %s: unterminated quote in CREATE", t->name);
			continue;
		}
		/* Decide whether this byte ends a column-def BEFORE touching depth: a
		 * ')' that closes a nested group like VARCHAR(10,2) brings depth back
		 * to zero but must not be mistaken for the end of the column list. The
		 * first version got this wrong and the oracle caught it: SELECT * on a
		 * table whose first column was VARCHAR(10,2) returned one column of
		 * four, which is a wrong answer rather than an error. */
		int split = 0, done = 0;
		if (*p == 0) {
			split = done = 1;
		} else if (*p == '(') {
			depth++;
		} else if (*p == ')') {
			if (depth > 0) depth--;
			else split = done = 1;
		} else if (*p == ',' && depth == 0) {
			split = 1;
		}
		if (split) {
			/* [part, p) is one column-def or table-constraint */
			char *frag = dupn((const uint8_t *)part, (size_t)(p - part));
			const char *q = frag;
			char *nm = ident(&q);
			if (nm) {
				char *u = upper_dup(nm);
				int is_constraint = !strcmp(u, "PRIMARY") || !strcmp(u, "UNIQUE") ||
				                    !strcmp(u, "CHECK") || !strcmp(u, "FOREIGN") ||
				                    !strcmp(u, "CONSTRAINT");
				free(u);
				if (is_constraint) {
					free(nm);
				} else if (t->ncol >= MAXCOL) {
					free(nm);
					free(frag);
					return err("table %s: more than %d columns", t->name, MAXCOL);
				} else {
					/* Declared type is the words up to the first keyword that
					 * starts a constraint, or '(' of a type like VARCHAR(10). */
					while (*q && isspace((unsigned char)*q)) q++;
					const char *ts = q;
					while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == ' ' ||
					              *q == '(' || *q == ')' || *q == ',')) {
						/* stop at a constraint keyword */
						if (isalpha((unsigned char)*q)) {
							const char *w = q;
							while (*q && isalnum((unsigned char)*q)) q++;
							size_t wl = (size_t)(q - w);
							static const char *stop[] = {"PRIMARY", "NOT", "NULL", "UNIQUE",
							                             "CHECK", "DEFAULT", "COLLATE",
							                             "REFERENCES", "GENERATED", "AS", 0};
							char wu[32];
							if (wl < sizeof wu) {
								for (size_t i = 0; i < wl; i++) wu[i] = (char)toupper((unsigned char)w[i]);
								wu[wl] = 0;
								int st2 = 0;
								for (int i = 0; stop[i]; i++) if (!strcmp(wu, stop[i])) st2 = 1;
								if (st2) { q = w; break; }
							}
							continue;
						}
						q++;
					}
					size_t tl = (size_t)(q - ts);
					while (tl && isspace((unsigned char)ts[tl - 1])) tl--;
					char *ty = dupn((const uint8_t *)ts, tl);

					/* An INTEGER PRIMARY KEY column on a rowid table is an
					 * alias for the rowid itself: the record stores NULL there
					 * and the real value is the cell's rowid. Only the exact
					 * type name "INTEGER" qualifies -- "INT PRIMARY KEY" does
					 * not -- and "DESC" disqualifies it. */
					char *tu = upper_dup(ty);
					char *ru = upper_dup(q);
					if (!strcmp(tu, "INTEGER") && strstr(ru, "PRIMARY KEY") && !strstr(ru, "DESC"))
						t->ipk = t->ncol;
					free(tu);
					free(ru);

					t->col[t->ncol] = nm;
					t->type[t->ncol] = ty;
					t->aff[t->ncol] = affinity(ty);
					t->ncol++;
				}
			}
			free(frag);
			if (done) break;
			part = p + 1;
		}
	}
	if (t->ncol == 0) return err("table %s: no columns found", t->name);
	/* shortcut: WITHOUT ROWID tables are index b-trees, which this parser does
	 * not read. Refuse loudly rather than print wrong rows. */
	char *tail = upper_dup(p);
	if (strstr(tail, "WITHOUT ROWID")) {
		free(tail);
		return err("table %s is WITHOUT ROWID; sqlr reads rowid tables only", t->name);
	}
	free(tail);
	return 0;
}

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
	int is_table = (v[0].t == V_TEXT && v[0].n == 5 && memcmp(v[0].s, "table", 5) == 0);
	int is_index = (v[0].t == V_TEXT && v[0].n == 5 && memcmp(v[0].s, "index", 5) == 0);
	if (!is_table && !is_index) return 0; /* a view or a trigger: no b-tree */
	if (v[1].t != V_TEXT) return err("sqlite_schema row has a non-text name");
	if (v[3].t != V_INT) return err("sqlite_schema row has a non-integer rootpage");
	if (v[3].i <= 0) return 0; /* virtual table: no b-tree of its own */

	if (is_index) {
		if (sc->nix == sc->ixcap) {
			sc->ixcap = sc->ixcap ? sc->ixcap * 2 : 16;
			sc->ix = realloc(sc->ix, (size_t)sc->ixcap * sizeof *sc->ix);
			if (!sc->ix) exit(1);
		}
		Idx *x = &sc->ix[sc->nix++];
		x->name = dupn(v[1].s, v[1].n);
		x->tbl = (v[2].t == V_TEXT) ? dupn(v[2].s, v[2].n) : NULL;
		x->sql = (v[4].t == V_TEXT) ? dupn(v[4].s, v[4].n) : NULL;
		x->root = (uint32_t)v[3].i;
		return 0;
	}

	if (sc->n == sc->cap) {
		sc->cap = sc->cap ? sc->cap * 2 : 16;
		sc->t = realloc(sc->t, (size_t)sc->cap * sizeof *sc->t);
		if (!sc->t) exit(1);
	}
	Table *t = &sc->t[sc->n];
	memset(t, 0, sizeof *t);
	t->name = dupn(v[1].s, v[1].n);
	t->root = (uint32_t)v[3].i;
	t->ipk = -1;
	t->sql = (v[4].t == V_TEXT) ? dupn(v[4].s, v[4].n) : NULL;
	sc->n++;
	return 0;
}

static int schema_load(Scan *s, Schema *sc) {
	memset(sc, 0, sizeof *sc);
	/* sqlite_schema is always rooted at page 1. */
	return scan_table(s, 1, schema_row, sc);
}

static Table *table_find(Schema *sc, const char *name) {
	for (int i = 0; i < sc->n; i++)
		if (ci_eq(sc->t[i].name, name)) return &sc->t[i];
	return NULL;
}

/* ------------------------------------------------------------------ output */

/* SQLite renders a REAL with %.15g and then makes sure the result cannot be
 * mistaken for an integer: if the mantissa has no '.', one is inserted. That is
 * why sqlite3 prints 1.0 and 1.0e+300 rather than 1 and 1e+300. Matching it
 * exactly is required for byte-identical output. */
static void put_real(FILE *f, double d) {
	if (isnan(d)) { fputs("", f); return; } /* SQLite stores NaN as NULL */
	if (isinf(d)) { fputs(d < 0 ? "-Inf" : "Inf", f); return; }
	char b[64];
	if (d == 0.0) { fputs("0.0", f); return; } /* also normalises -0.0 */
	snprintf(b, sizeof b, "%.15g", d);
	if (!strchr(b, '.')) {
		char *e = strpbrk(b, "eE");
		if (!e) {
			strncat(b, ".0", sizeof b - strlen(b) - 1);
		} else {
			char tail[64];
			snprintf(tail, sizeof tail, "%s", e);
			size_t cut = (size_t)(e - b);
			snprintf(b + cut, sizeof b - cut, ".0%s", tail);
		}
	}
	fputs(b, f);
}

static const char hexd[] = "0123456789ABCDEF";

static void put_blob(FILE *f, const Val *v) {
	/* shortcut: blobs render as SQLite's own X'..' literal syntax, in both TSV
	 * and JSON. It is lossless and unambiguous for a blob, at the cost of being
	 * indistinguishable from a text value that happens to spell X'41'. */
	fputs("X'", f);
	for (uint32_t i = 0; i < v->n; i++) {
		fputc(hexd[v->s[i] >> 4], f);
		fputc(hexd[v->s[i] & 15], f);
	}
	fputc('\'', f);
}

static void put_tsv(FILE *f, const Val *v) {
	switch (v->t) {
	case V_NULL: break; /* empty field, like sqlite3's default nullvalue */
	case V_INT: fprintf(f, "%" PRId64, v->i); break;
	case V_REAL: put_real(f, v->r); break;
	case V_TEXT: fwrite(v->s, 1, v->n, f); break;
	case V_BLOB: put_blob(f, v); break;
	}
}

static void put_json_str(FILE *f, const uint8_t *s, uint32_t n) {
	fputc('"', f);
	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = s[i];
		switch (c) {
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\r': fputs("\\r", f); break;
		case '\t': fputs("\\t", f); break;
		case '\b': fputs("\\b", f); break;
		case '\f': fputs("\\f", f); break;
		default:
			if (c < 0x20) fprintf(f, "\\u%04x", c);
			else fputc(c, f);
		}
	}
	fputc('"', f);
}

static void put_json(FILE *f, const Val *v) {
	switch (v->t) {
	case V_NULL: fputs("null", f); break;
	case V_INT: fprintf(f, "%" PRId64, v->i); break;
	case V_REAL: put_real(f, v->r); break; /* bare token; Inf is not legal JSON */
	case V_TEXT: put_json_str(f, v->s, v->n); break;
	case V_BLOB:
		/* A single quote needs no escape in JSON, so the X'..' literal goes in
		 * verbatim and the result is still valid JSON. */
		fputc('"', f);
		put_blob(f, v);
		fputc('"', f);
		break;
	}
}

/* ------------------------------------------------------------- query parsing */

typedef enum { OP_NONE, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_ISNULL, OP_NOTNULL } Op;

typedef struct {
	char *table;
	int star;
	int nsel;
	char *sel[MAXCOL];
	int selidx[MAXCOL]; /* column index, or -1 meaning "the rowid" */
	Op op;
	int wcol; /* -1 means rowid */
	Val wval;
	uint8_t *wbuf; /* owns wval's bytes */
} Query;

static void skipws(const char **p) {
	while (**p && isspace((unsigned char)**p)) (*p)++;
}

static int word(const char **p, const char *kw) {
	const char *q = *p;
	skipws(&q);
	size_t n = strlen(kw);
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)q[i]) != tolower((unsigned char)kw[i])) return 0;
	if (isalnum((unsigned char)q[n]) || q[n] == '_') return 0;
	*p = q + n;
	return 1;
}

static int hexval(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Literals: NULL, integer, real, 'text with '' escapes', X'hex'. */
static int parse_literal(const char **pp, Val *v, uint8_t **own) {
	const char *p = *pp;
	skipws(&p);
	memset(v, 0, sizeof *v);
	*own = NULL;
	if (word(&p, "null")) { v->t = V_NULL; *pp = p; return 0; }
	if ((*p == 'x' || *p == 'X') && p[1] == '\'') {
		p += 2;
		const char *st = p;
		while (*p && *p != '\'') p++;
		if (*p != '\'') return err("unterminated blob literal");
		size_t nh = (size_t)(p - st);
		if (nh % 2) return err("blob literal has an odd number of hex digits");
		uint8_t *b = xmalloc(nh / 2 + 1);
		for (size_t i = 0; i < nh; i += 2) {
			int hi = hexval(st[i]), lo = hexval(st[i + 1]);
			if (hi < 0 || lo < 0) { free(b); return err("bad hex digit in blob literal"); }
			b[i / 2] = (uint8_t)(hi << 4 | lo);
		}
		v->t = V_BLOB;
		v->s = b;
		v->n = (uint32_t)(nh / 2);
		*own = b;
		*pp = p + 1;
		return 0;
	}
	if (*p == '\'') {
		p++;
		char *out = xmalloc(strlen(p) + 1);
		size_t k = 0;
		while (*p) {
			if (*p == '\'') {
				if (p[1] == '\'') { out[k++] = '\''; p += 2; continue; }
				p++;
				v->t = V_TEXT;
				v->s = (uint8_t *)out;
				v->n = (uint32_t)k;
				*own = (uint8_t *)out;
				*pp = p;
				return 0;
			}
			out[k++] = *p++;
		}
		free(out);
		return err("unterminated string literal");
	}
	{
		char *endp = NULL;
		const char *st = p;
		int isreal = 0;
		if (*p == '+' || *p == '-') p++;
		if (!isdigit((unsigned char)*p) && *p != '.') return err("expected a literal value");
		while (isdigit((unsigned char)*p)) p++;
		if (*p == '.' || *p == 'e' || *p == 'E') isreal = 1;
		if (isreal) {
			double d = strtod(st, &endp);
			if (endp == st) return err("bad numeric literal");
			v->t = V_REAL;
			v->r = d;
			*pp = endp;
		} else {
			errno = 0;
			long long n = strtoll(st, &endp, 10);
			if (endp == st) return err("bad integer literal");
			if (errno == ERANGE) { /* out of int64 range: SQLite makes it a REAL */
				v->t = V_REAL;
				v->r = strtod(st, &endp);
			} else {
				v->t = V_INT;
				v->i = n;
			}
			*pp = endp;
		}
		return 0;
	}
}

static int col_index(Table *t, const char *name) {
	for (int i = 0; i < t->ncol; i++)
		if (ci_eq(t->col[i], name)) return i;
	if (ci_eq(name, "rowid") || ci_eq(name, "_rowid_") || ci_eq(name, "oid")) return -1;
	return -2;
}

/* ----------------------------------------------------------------- indexes */

/* An index entry is an ordinary record: the indexed columns in index order,
 * then the rowid of the table row it points at. Nothing in the record says
 * which column is which, so the affinities have to come from the CREATE INDEX
 * statement mapped onto the table's columns. They are needed because a
 * REAL-affinity column stores an integral value as an integer serial type in
 * the index exactly as it does in the table, and printing it back as an
 * integer is a wrong answer, not a formatting difference.
 *
 * shortcut: the column list is shape-matched, as in parse_create. An index on
 * an expression, and an index SQLite wrote for itself (sqlite_autoindex_*,
 * whose sqlite_schema row has no SQL at all), both come out with no affinity,
 * which is only observable on a REAL column. Upgrade path is a real
 * tokeniser, plus reading the UNIQUE constraints out of the CREATE TABLE to
 * recover the key columns of an auto-index. */
static int index_affinities(Schema *sc, Idx *x, Aff *aff, int *naff) {
	*naff = 0;
	if (!x->tbl) return 0;
	Table *t = table_find(sc, x->tbl);
	if (!t || !t->sql) return 0;
	/* parse_create is also what refuses a WITHOUT ROWID table. Its indexes
	 * end each entry with the table's primary key columns instead of a
	 * rowid, so dumping one would print something plausible and wrong. The
	 * refusal has to happen here rather than inside the column-list parse
	 * below, because an auto-index has no CREATE statement to parse. */
	if (t->ncol == 0 && parse_create(t, t->sql) < 0) return -1;
	if (!x->sql) return 0;

	const char *p = strchr(x->sql, '(');
	if (!p) return 0;
	p++;
	int depth = 0;
	const char *part = p;
	for (;; p++) {
		if (*p == '\'' || *p == '"' || *p == '`') {
			char qc = *p++;
			while (*p && *p != qc) p++;
			if (!*p) return err("index %s: unterminated quote in CREATE", x->name);
			continue;
		}
		int split = 0, done = 0;
		if (*p == 0) {
			split = done = 1;
		} else if (*p == '(') {
			depth++;
		} else if (*p == ')') {
			if (depth > 0) depth--;
			else split = done = 1;
		} else if (*p == ',' && depth == 0) {
			split = 1;
		}
		if (split) {
			char *frag = dupn((const uint8_t *)part, (size_t)(p - part));
			const char *f = frag;
			char *nm = ident(&f);
			Aff a = AFF_BLOB; /* no affinity: leaves the stored value alone */
			if (nm) {
				int ci = col_index(t, nm);
				if (ci >= 0) a = t->aff[ci];
				free(nm);
			}
			if (*naff < MAXCOL) aff[(*naff)++] = a;
			free(frag);
			if (done) break;
			part = p + 1;
		}
	}
	return 0;
}

typedef struct {
	FILE *out;
	Aff aff[MAXCOL];
	int naff;
} IdxEmit;

static int index_row(void *ctx, int64_t rowid, const uint8_t *pay, size_t plen) {
	IdxEmit *e = ctx;
	Val v[MAXCOL];
	int n;
	(void)rowid; /* an index entry carries its rowid inside the record */
	if (rec_parse(pay, plen, v, &n) < 0) return -1;
	for (int i = 0; i < n; i++) {
		if (i) fputc('\t', e->out);
		if (i < e->naff && e->aff[i] == AFF_REAL && v[i].t == V_INT) {
			v[i].t = V_REAL;
			v[i].r = (double)v[i].i;
		}
		put_tsv(e->out, &v[i]);
	}
	fputc('\n', e->out);
	return 0;
}

static Idx *index_find(Schema *sc, const char *name) {
	for (int i = 0; i < sc->nix; i++)
		if (ci_eq(sc->ix[i].name, name)) return &sc->ix[i];
	return NULL;
}

/* ------------------------------------------------------------- query parsing */

/* shortcut: this recognises exactly
 *   SELECT <*|col,...> FROM <table> [WHERE <col> <op> <literal> | <col> IS [NOT] NULL]
 * and nothing else -- no joins, no ORDER BY, no LIMIT, no expressions, no
 * aggregates, no OR/AND. Upgrade path is a real tokeniser plus an expression
 * tree with an eval() over Val; the scan loop below already has the right shape
 * for it (one predicate call per row). */
static int query_parse(Query *q, Schema *sc, const char *sql, Table **tout) {
	memset(q, 0, sizeof *q);
	q->op = OP_NONE;
	const char *p = sql;
	if (!word(&p, "select")) return err("only SELECT is supported");

	/* Column list: remember the names now, resolve them once we know the table. */
	skipws(&p);
	if (*p == '*') { q->star = 1; p++; }
	else {
		for (;;) {
			char *nm = ident(&p);
			if (!nm) return err("expected a column name in the select list");
			if (q->nsel >= MAXCOL) { free(nm); return err("too many columns selected"); }
			q->sel[q->nsel++] = nm;
			skipws(&p);
			if (*p == ',') { p++; continue; }
			break;
		}
	}
	if (!word(&p, "from")) return err("expected FROM");
	q->table = ident(&p);
	if (!q->table) return err("expected a table name after FROM");

	Table *t = table_find(sc, q->table);
	if (!t) return err("no such table: %s", q->table);
	if (!t->sql) return err("table %s has no CREATE statement in sqlite_schema", q->table);
	if (t->ncol == 0 && parse_create(t, t->sql) < 0) return -1;
	*tout = t;

	if (q->star) {
		q->nsel = t->ncol;
		for (int i = 0; i < t->ncol; i++) {
			q->sel[i] = dupn((const uint8_t *)t->col[i], strlen(t->col[i]));
			q->selidx[i] = i;
		}
	} else {
		for (int i = 0; i < q->nsel; i++) {
			int ix = col_index(t, q->sel[i]);
			if (ix == -2) return err("no such column: %s", q->sel[i]);
			q->selidx[i] = ix;
		}
	}

	skipws(&p);
	if (*p == 0 || *p == ';') return 0;
	if (!word(&p, "where")) return err("unexpected trailing input: %s", p);

	char *wname = ident(&p);
	if (!wname) return err("expected a column name after WHERE");
	q->wcol = col_index(t, wname);
	if (q->wcol == -2) {
		err("no such column: %s", wname);
		free(wname);
		return -1;
	}
	free(wname);

	skipws(&p);
	if (word(&p, "is")) {
		int neg = word(&p, "not");
		if (!word(&p, "null")) return err("expected NULL after IS");
		q->op = neg ? OP_NOTNULL : OP_ISNULL;
	} else {
		if (p[0] == '=' && p[1] == '=') { q->op = OP_EQ; p += 2; }
		else if (p[0] == '=') { q->op = OP_EQ; p += 1; }
		else if (p[0] == '!' && p[1] == '=') { q->op = OP_NE; p += 2; }
		else if (p[0] == '<' && p[1] == '>') { q->op = OP_NE; p += 2; }
		else if (p[0] == '<' && p[1] == '=') { q->op = OP_LE; p += 2; }
		else if (p[0] == '>' && p[1] == '=') { q->op = OP_GE; p += 2; }
		else if (p[0] == '<') { q->op = OP_LT; p += 1; }
		else if (p[0] == '>') { q->op = OP_GT; p += 1; }
		else return err("unsupported operator at: %s", p);
		if (parse_literal(&p, &q->wval, &q->wbuf) < 0) return -1;
	}
	skipws(&p);
	if (*p == ';') { p++; skipws(&p); }
	if (*p) return err("unexpected trailing input: %s", p);
	return 0;
}

/* ------------------------------------------------- comparison and affinity */

/* Storage classes sort NULL < numeric < text < blob, and that ordering is what
 * makes 'abc' > 99 true in SQLite. */
static int classof(const Val *v) {
	switch (v->t) {
	case V_NULL: return 0;
	case V_INT: case V_REAL: return 1;
	case V_TEXT: return 2;
	default: return 3;
	}
}

static int cmp_vals(const Val *a, const Val *b) {
	int ca = classof(a), cb = classof(b);
	if (ca != cb) return ca < cb ? -1 : 1;
	switch (ca) {
	case 0: return 0;
	case 1: {
		if (a->t == V_INT && b->t == V_INT) return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
		double x = a->t == V_INT ? (double)a->i : a->r;
		double y = b->t == V_INT ? (double)b->i : b->r;
		return x < y ? -1 : x > y ? 1 : 0;
	}
	default: {
		uint32_t n = a->n < b->n ? a->n : b->n;
		int c = n ? memcmp(a->s, b->s, n) : 0;
		if (c) return c < 0 ? -1 : 1;
		return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
	}
	}
}

/* Apply a column's affinity to the literal side of the comparison, which is
 * what SQLite does before comparing. The stored value already had affinity
 * applied when it was inserted, so only the literal needs converting. */
static void apply_affinity(Aff a, Val *v, char *scratch, size_t slen) {
	if (a == AFF_TEXT && (v->t == V_INT || v->t == V_REAL)) {
		if (v->t == V_INT) snprintf(scratch, slen, "%" PRId64, v->i);
		else {
			FILE *m = fmemopen(scratch, slen, "w");
			if (!m) return;
			put_real(m, v->r);
			fclose(m);
		}
		v->t = V_TEXT;
		v->s = (uint8_t *)scratch;
		v->n = (uint32_t)strlen(scratch);
		return;
	}
	if ((a == AFF_INT || a == AFF_REAL || a == AFF_NUM) && v->t == V_TEXT) {
		/* Numeric affinity converts text only if it is a complete, well-formed
		 * number; otherwise the text is left alone. */
		if (v->n >= slen) return;
		memcpy(scratch, v->s, v->n);
		scratch[v->n] = 0;
		char *e = NULL;
		errno = 0;
		long long n = strtoll(scratch, &e, 10);
		if (e && *e == 0 && e != scratch && errno != ERANGE) {
			v->t = V_INT;
			v->i = n;
			return;
		}
		e = NULL;
		double d = strtod(scratch, &e);
		if (e && *e == 0 && e != scratch) {
			v->t = V_REAL;
			v->r = d;
		}
	}
}

/* --------------------------------------------------------------- the scan */

typedef struct {
	Query *q;
	Table *t;
	FILE *out;
	int json;
	Aff waff;
	long long nrows;
} Emit;

static int emit_row(void *ctx, int64_t rowid, const uint8_t *pay, size_t plen) {
	Emit *e = ctx;
	Query *q = e->q;
	Table *t = e->t;
	Val v[MAXCOL];
	int n;
	if (rec_parse(pay, plen, v, &n) < 0) return -1;

	/* A row written before an ALTER TABLE ADD COLUMN has fewer values than the
	 * table has columns; the missing trailing ones read as NULL.
	 * shortcut: a column added with a non-NULL DEFAULT would need that default
	 * from the CREATE statement; we always use NULL. */
	for (int i = n; i < t->ncol; i++) {
		memset(&v[i], 0, sizeof v[i]);
		v[i].t = V_NULL;
	}
	if (n > t->ncol) n = t->ncol;
	/* A column with REAL affinity may hold an INTEGER serial type on disk:
	 * SQLite stores a float whose value is an exact integer as the shortest
	 * integer that fits, to save bytes, and converts it back to a float when
	 * the column is read. Skipping this makes sqlr print 1 where sqlite3 prints
	 * 1.0 -- the oracle caught exactly that. */
	for (int i = 0; i < t->ncol; i++)
		if (t->aff[i] == AFF_REAL && v[i].t == V_INT) {
			v[i].t = V_REAL;
			v[i].r = (double)v[i].i;
		}
	/* The INTEGER PRIMARY KEY column is stored as NULL; its value is the rowid. */
	if (t->ipk >= 0 && t->ipk < t->ncol && v[t->ipk].t == V_NULL) {
		v[t->ipk].t = V_INT;
		v[t->ipk].i = rowid;
	}
	Val rid = {V_INT, rowid, 0, NULL, 0};

	if (q->op != OP_NONE) {
		const Val *cv = (q->wcol < 0) ? &rid : &v[q->wcol];
		if (q->op == OP_ISNULL) {
			if (cv->t != V_NULL) return 0;
		} else if (q->op == OP_NOTNULL) {
			if (cv->t == V_NULL) return 0;
		} else {
			/* NULL compares unknown, so a NULL row never matches =, <, etc. */
			if (cv->t == V_NULL || q->wval.t == V_NULL) return 0;
			char scratch[64];
			Val lit = q->wval;
			apply_affinity(e->waff, &lit, scratch, sizeof scratch);
			int c = cmp_vals(cv, &lit);
			int ok = 0;
			switch (q->op) {
			case OP_EQ: ok = (c == 0); break;
			case OP_NE: ok = (c != 0); break;
			case OP_LT: ok = (c < 0); break;
			case OP_LE: ok = (c <= 0); break;
			case OP_GT: ok = (c > 0); break;
			case OP_GE: ok = (c >= 0); break;
			default: break;
			}
			if (!ok) return 0;
		}
	}

	if (e->json) {
		fputs(e->nrows ? ",\n{" : "{", e->out);
		for (int i = 0; i < q->nsel; i++) {
			if (i) fputc(',', e->out);
			put_json_str(e->out, (const uint8_t *)q->sel[i], (uint32_t)strlen(q->sel[i]));
			fputc(':', e->out);
			put_json(e->out, q->selidx[i] < 0 ? &rid : &v[q->selidx[i]]);
		}
		fputc('}', e->out);
	} else {
		for (int i = 0; i < q->nsel; i++) {
			if (i) fputc('\t', e->out);
			put_tsv(e->out, q->selidx[i] < 0 ? &rid : &v[q->selidx[i]]);
		}
		fputc('\n', e->out);
	}
	e->nrows++;
	return 0;
}

/* ------------------------------------------------------------------- main */

static int cmp_name(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int usage(void) {
	fputs("usage: sqlr [--json] DB.sqlite "
	      "{.dbinfo | .tables | .schema | .indexes | .index NAME | SELECT ...}\n", stderr);
	return 2;
}

int main(int argc, char **argv) {
	int json = 0, i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--json")) json = 1;
		else if (!strcmp(argv[i], "--tsv")) json = 0;
		else return usage();
	}
	/* `.index NAME` is the one form that takes an argument of its own. */
	const char *ixname = NULL;
	if (argc - i == 3 && !strcmp(argv[i + 1], ".index")) ixname = argv[i + 2];
	else if (argc - i != 2) return usage();
	const char *path = argv[i], *cmd = argv[i + 1];

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
	} else if (!strcmp(cmd, ".indexes")) {
		/* Every index in sqlite_schema, sorted. The auto-indexes SQLite
		 * creates for UNIQUE and PRIMARY KEY constraints are included: they
		 * are real b-trees in the file and .index dumps them like any other. */
		char **names = xmalloc((size_t)(schema.nix ? schema.nix : 1) * sizeof *names);
		for (int j = 0; j < schema.nix; j++) names[j] = schema.ix[j].name;
		qsort(names, (size_t)schema.nix, sizeof *names, cmp_name);
		for (int j = 0; j < schema.nix; j++) printf("%s\n", names[j]);
		free(names);
	} else if (ixname) {
		/* The index b-tree in key order, one entry per line: the indexed
		 * columns, then the rowid the entry points at. */
		Idx *x = index_find(&schema, ixname);
		if (!x) {
			fprintf(stderr, "sqlr: no such index: %s\n", ixname);
			rc = 1;
		} else {
			IdxEmit e;
			e.out = stdout;
			if (index_affinities(&schema, x, e.aff, &e.naff) < 0 ||
			    scan_index(&sc, x->root, index_row, &e) < 0) {
				fprintf(stderr, "sqlr: %s\n", g_err);
				rc = 1;
			}
		}
	} else if (!strcmp(cmd, ".schema")) {
		/* In sqlite_schema order, each statement terminated with a semicolon,
		 * which is what sqlite3's .schema emits. */
		for (int j = 0; j < schema.nsql; j++) printf("%s;\n", schema.sql[j]);
	} else {
		Query q;
		Table *t = NULL;
		if (query_parse(&q, &schema, cmd, &t) < 0) {
			fprintf(stderr, "sqlr: %s\n", g_err);
			rc = 1;
		} else {
			Emit e = {&q, t, stdout, json, AFF_BLOB, 0};
			if (q.op != OP_NONE)
				e.waff = (q.wcol < 0) ? AFF_INT : t->aff[q.wcol];
			if (json) fputc('[', stdout);
			if (scan_table(&sc, t->root, emit_row, &e) < 0) {
				if (json) fputc('\n', stdout);
				fprintf(stderr, "sqlr: %s\n", g_err);
				rc = 1;
			} else if (json) {
				fputs("]\n", stdout);
			}
		}
	}
	if (fflush(stdout) != 0) {
		fprintf(stderr, "sqlr: write failed: %s\n", strerror(errno));
		rc = 1;
	}
	scan_free(&sc);
	free(db.d);
	return rc;
}
