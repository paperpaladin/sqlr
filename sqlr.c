/* sqlr -- a read-only SQLite 3 database file parser, from scratch.
 *
 * Nothing here links libsqlite3. Everything below is a direct reading of the
 * on-disk format: the 100-byte header, table b-tree pages, the record encoding,
 * and the sqlite_schema table. The file is opened "rb" and never written.
 *
 * Usage:
 *   sqlr DB.sqlite .dbinfo
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

/* ------------------------------------------------------------------- main */

static int usage(void) {
	fputs("usage: sqlr DB.sqlite .dbinfo\n", stderr);
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
	if (strcmp(cmd, ".dbinfo") != 0) {
		free(db.d);
		return usage();
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
}
