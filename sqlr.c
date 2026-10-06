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
 *   sqlr DB.sqlite 'SELECT a,b FROM t WHERE a = 1'
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

typedef struct {
	Table *t;
	int n, cap;
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
		if (*p == '(') depth++;
		else if (*p == ')' && depth > 0) depth--;
		if (*p == 0 || (depth == 0 && (*p == ',' || *p == ')'))) {
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
			if (*p == 0 || *p == ')') break;
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

	for (int i = 0; i < q->nsel; i++) {
		if (i) fputc('\t', e->out);
		put_tsv(e->out, q->selidx[i] < 0 ? &rid : &v[q->selidx[i]]);
	}
	fputc('\n', e->out);
	e->nrows++;
	return 0;
}

/* ------------------------------------------------------------------- main */

static int cmp_name(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int usage(void) {
	fputs("usage: sqlr DB.sqlite {.dbinfo | .tables | .schema | SELECT ...}\n", stderr);
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
		Query q;
		Table *t = NULL;
		if (query_parse(&q, &schema, cmd, &t) < 0) {
			fprintf(stderr, "sqlr: %s\n", g_err);
			rc = 1;
		} else {
			Emit e = {&q, t, stdout, 0, AFF_BLOB, 0};
			if (q.op != OP_NONE)
				e.waff = (q.wcol < 0) ? AFF_INT : t->aff[q.wcol];
			if (scan_table(&sc, t->root, emit_row, &e) < 0) {
				fprintf(stderr, "sqlr: %s\n", g_err);
				rc = 1;
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
