/*
    file.c - opening and closing an AerDB database, and reading and writing its pages

    Page n of a database starts at byte (n - 1) * AERDB_PAGE_SIZE of its file: page 1 at byte 0,
    page 2 at byte 4096, and so on. Pages are always read and written whole, with the file
    system's fs_read_at and fs_write_at, which change only the bytes asked for.

    Page 1, the header, is read once when the database is opened and kept in memory (in
    aerdb_t's `header`). Whatever changes it (a new table, a new page, a row's new rowid) calls
    header_save() to write it back straight away, so the file is always up to date.
*/

#include "aerdb.h"
#include <stdarg.h>
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../sys/versions.h"
#include "../sys/trace.h"
#include "../lib/fpu.h"

// The header has to fit on page 1. If someone makes AERDB_TABLES_MAX bigger than fits, this
// stops the build with a message, rather than letting tables run off the end of the page
_Static_assert(sizeof(aerdb_header_t) <= AERDB_PAGE_SIZE, "aerdb_header_t doesn't fit on page 1");

// ---- pages --------------------------------------------------------------------------------------

// the byte of the file where page n starts (pages count from 1)
static uint32_t page_offset (uint32_t n) {
    return (n - 1) * AERDB_PAGE_SIZE;
}

// read page n into buffer (which must have room for AERDB_PAGE_SIZE bytes). False if it couldn't
bool page_read (aerdb_t* db, uint32_t n, void* buffer) {
    // fs_open wants the folder and name as one path
    char path[FS_PATH_MAX + FS_NAME_MAX];
    sfmt(path, sizeof(path), "%s%s", db->folder, db->name);

    fs_file_t file;
    if (!fs_open(path, &file)) {
        return false;
    }
    int32_t got = fs_read_at(&file, page_offset(n), buffer, AERDB_PAGE_SIZE);
    return got == AERDB_PAGE_SIZE;
}

// write page n from buffer, in place. The first change to a database keeps a version of the
// whole file first, so AerOS's UNDO can still bring it back as it was
bool page_write (aerdb_t* db, uint32_t n, const void* buffer) {
    if (!db->changed) {
        versions_keep(db->folder, db->name);
        db->changed = true;
    }
    return fs_write_at(db->folder, db->name, page_offset(n), buffer, AERDB_PAGE_SIZE) == FS_OK;
}

// add a new, empty page on the end of the file, and give back its number (0 if it couldn't).
// Writing past the end of the file makes it longer
uint32_t page_new (aerdb_t* db) {
    uint8_t page[AERDB_PAGE_SIZE];
    memset(page, 0, sizeof(page));

    uint32_t n = db->header.page_count + 1;
    if (!page_write(db, n, page)) {
        return 0;
    }
    db->header.page_count = n;
    if (!header_save(db)) {
        return 0;
    }
    return n;
}

// write the header (kept in memory) back to page 1
bool header_save (aerdb_t* db) {
    uint8_t page[AERDB_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    memcpy(page, &db->header, sizeof(aerdb_header_t));
    return page_write(db, 1, page);
}

// ---- opening and closing ----------------------------------------------------------------------

// make a new, empty database file: page 1 alone, with no tables
static int create_file (const char* folder, const char* name) {
    uint8_t page[AERDB_PAGE_SIZE];
    memset(page, 0, sizeof(page));

    aerdb_header_t* header = (aerdb_header_t*)page;
    memcpy(header->magic, AERDB_MAGIC, sizeof(header->magic));
    header->page_count = 1;
    header->table_count = 0;

    if (fs_create_bytes(folder, name, page, AERDB_PAGE_SIZE) != FS_OK) {
        return AERDB_ERR_IO;
    }
    TRACE("made a new, empty database %s%s", folder, name);
    return AERDB_OK;
}

// open a database (see aerdb.h)
aerdb_t* aerdb_open (const char* folder, const char* name, int* result) {
    // REAL values are doubles, which need the processor's maths unit switched on. AerLang does
    // that before a program runs, but the terminal's SQL command doesn't go through AerLang
    fpu_enable();

    // a new database the first time
    if (!fs_file_exists(folder, name)) {
        *result = create_file(folder, name);
        if (*result != AERDB_OK) {
            return NULL;
        }
    }

    aerdb_t* db = malloc(sizeof(aerdb_t));
    if (db == NULL) {
        *result = AERDB_ERR_NO_MEMORY;
        return NULL;
    }
    memset(db, 0, sizeof(aerdb_t));
    scpy(db->folder, folder, sizeof(db->folder));
    scpy(db->name, name, sizeof(db->name));

    // page 1 is the header: read it, and check it really is an AerDB file
    uint8_t page[AERDB_PAGE_SIZE];
    if (!page_read(db, 1, page)) {
        free(db);
        *result = AERDB_ERR_NOT_DB; // too short to be a database (or unreadable)
        return NULL;
    }
    if (memcmp(page, AERDB_MAGIC, sizeof(AERDB_MAGIC)) != 0) {
        free(db);
        *result = AERDB_ERR_NOT_DB;
        return NULL;
    }
    memcpy(&db->header, page, sizeof(aerdb_header_t));

    TRACE("opened %s%s: %u table(s) in %u page(s)", folder, name,
          (unsigned int)db->header.table_count, (unsigned int)db->header.page_count);
    *result = AERDB_OK;
    return db;
}

// close a database: everything is on the disk already, so only the memory goes
void aerdb_close (aerdb_t* db) {
    free(db);
}

// ---- what went wrong ----------------------------------------------------------------------------

// Remember what went wrong, in full, for aerdb_error, and give back the result code, so a
// function can say   return aerdb_fail(db, AERDB_ERR_NOT_FOUND, "no such table: %s", name);
int aerdb_fail (aerdb_t* db, int result, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(db->error, sizeof(db->error), format, args);
    va_end(args);
    return result;
}

// what went wrong last (see aerdb.h)
const char* aerdb_error (const aerdb_t* db) {
    return db->error[0] != '\0' ? db->error : "no problem";
}

// a short description of a result code (see aerdb.h)
const char* aerdb_strerror (int result) {
    switch (result) {
        case AERDB_OK:              return "no problem";
        case AERDB_ERR_NOT_FOUND:   return "no such table or column";
        case AERDB_ERR_EXISTS:      return "there is a table with that name already";
        case AERDB_ERR_FULL:        return "the database or the disk is full";
        case AERDB_ERR_IO:          return "the file couldn't be read or written";
        case AERDB_ERR_SYNTAX:      return "the SQL doesn't make sense";
        case AERDB_ERR_TOO_BIG:     return "a row is too big";
        case AERDB_ERR_NOT_DB:      return "it isn't an AerDB database";
        case AERDB_ERR_UNSUPPORTED: return "AerDB can't do that yet";
        case AERDB_ERR_VALUES:      return "the wrong number of values";
        case AERDB_ERR_NO_MEMORY:   return "not enough memory";
        default:                    return "something went wrong";
    }
}
