/*
    aerdb.h - AerDB, AerOS's own database

    AerDB keeps tables of rows in one file, and understands a little SQL:

        CREATE TABLE items (name TEXT, price REAL)
        INSERT INTO items VALUES ('tea', 2.5)
        SELECT name, price FROM items WHERE price < 3
        UPDATE items SET price = 3.0 WHERE name = 'tea'
        DELETE FROM items WHERE name = 'tea'

    How it is laid out, file by file:

        aerdb.h    this file: the file format, values, and every function
        file.c     opening and closing a database, reading and writing its pages
        table.c    making and finding tables; adding, reading and deleting rows
        value.c    values, and turning a row into bytes and back
        sql.h      how SQL is read: words, and the statement_t that says what to do
        sql.c      splitting SQL into words, and making sense of them (a statement_t)
        run.c      running a statement, and compacting a database

    THE FILE

    A database is one file of pages, AERDB_PAGE_SIZE bytes each. Page 1 is the header: which
    tables there are, and where each one's pages are. Every other page holds rows. A table is a
    chain of pages, each saying which page comes next:

        shop.db:  [ page 1        ][ page 2         ][ page 3         ][ page 4         ]
                    the tables:      items, rows 1-60  people, rows 1-5  items, rows 61-90
                    items: 2 ... 4   next page: 4      next page: none   next page: none

    A new row goes on the end of its table's last page, or on a new page when that one is full.
    Reading a table follows its chain from the first page. Deleting a row only marks it
    deleted; COMPACT (aerdb_compact) gives the space back. Nothing moves once it is written,
    which is what keeps AerDB simple.

    Numbers in the file are stored the way the x86 processor keeps them in memory, so the
    structs below describe the bytes exactly (__attribute__((packed)) stops the compiler adding
    gaps between their fields).

    A ROW is its length, a flags byte, its rowid (its number in the table), then its values:

        [ length: 2 bytes ][ flags: 1 byte ][ rowid: 4 bytes ][ value ][ value ] ...
                             bit 0 set: deleted

    and each VALUE is a type byte (db_type_t, below), then what that type needs:

        DB_NULL   nothing
        DB_INT    4 bytes: a whole number, the same size as AerLang's numbers
        DB_REAL   8 bytes: a double
        DB_TEXT   a 2 byte length, then the characters (no '\0')
        DB_BLOB   a 2 byte length, then the bytes

    A row has to fit on one page in this version.
*/

#ifndef AERDB_H
#define AERDB_H

#include <stdint.h>
#include <stdbool.h>
#include "../sys/f_sys.h"

// ---- sizes ------------------------------------------------------------------------------------

#define AERDB_PAGE_SIZE   4096
#define AERDB_MAGIC       "AERDB01"   // the first 8 bytes of every AerDB file (with its '\0')
#define AERDB_TABLES_MAX  20          // tables in one database: as many as fit on page 1
#define AERDB_NAME_MAX    24          // a table or column name, counting its '\0'
#define AERDB_COLUMNS_MAX 16          // columns in one table
#define AERDB_ROW_HEADER  7           // a row's length, flags and rowid, before its values

// ---- results ----------------------------------------------------------------------------------
// Every function that can fail gives back one of these. aerdb_strerror() says what each means,
// and aerdb_error() gives the last one in full ("no such table: items")

#define AERDB_OK               0
#define AERDB_ERR_NOT_FOUND   -1   // no such table, or no such column
#define AERDB_ERR_EXISTS      -2   // a table with that name is there already
#define AERDB_ERR_FULL        -3   // too many tables, or the disk is full
#define AERDB_ERR_IO          -4   // the file couldn't be read or written
#define AERDB_ERR_SYNTAX      -5   // the SQL doesn't make sense
#define AERDB_ERR_TOO_BIG     -6   // a row too big for a page, or too many columns
#define AERDB_ERR_NOT_DB      -7   // the file isn't an AerDB database
#define AERDB_ERR_UNSUPPORTED -8   // SQL that AerDB doesn't do yet
#define AERDB_ERR_VALUES      -9   // the wrong number of values (or of ? parameters)
#define AERDB_ERR_NO_MEMORY  -10

// ---- the file format ----------------------------------------------------------------------------

// a table, as page 1 lists it
typedef struct __attribute__((packed)) {
    char name[AERDB_NAME_MAX];  // its name
    char columns[128];          // its columns as CREATE TABLE gave them: "name TEXT, price REAL"
    uint32_t first_page;        // where its chain of pages starts (0: no rows yet)
    uint32_t last_page;         // where the chain ends: new rows go here
    uint32_t next_rowid;        // the number the next new row gets
} aerdb_table_t;

// page 1
typedef struct __attribute__((packed)) {
    char magic[8];              // AERDB_MAGIC: "this is an AerDB file"
    uint32_t page_count;        // how many pages the file has
    uint32_t table_count;       // how many of tables[] are in use
    aerdb_table_t tables[AERDB_TABLES_MAX];
} aerdb_header_t;

// the start of every other page
typedef struct __attribute__((packed)) {
    uint32_t next_page;         // the next page of this table's chain, 0 for the last one
    uint16_t used;              // bytes used on this page, counting this header
    uint16_t rows;              // rows on this page, deleted ones too
} aerdb_page_header_t;

// ---- values -------------------------------------------------------------------------------------

typedef enum { DB_NULL = 0, DB_INT = 1, DB_REAL = 2, DB_TEXT = 3, DB_BLOB = 4 } db_type_t;

// One value. Text and blobs aren't copied: `bytes` points at them where they are (in a page
// being read, or the caller's own string), so a value is only good while that is
typedef struct {
    db_type_t type;
    int32_t i;                  // DB_INT
    double r;                   // DB_REAL
    const uint8_t* bytes;       // DB_TEXT, DB_BLOB
    uint16_t length;            // DB_TEXT, DB_BLOB: how many bytes
} db_value_t;

// ---- an open database ---------------------------------------------------------------------------

typedef struct {
    char folder[FS_PATH_MAX];   // where the file is, e.g. "/home/alex/"
    char name[FS_NAME_MAX];     // its name, e.g. "shop.db"
    aerdb_header_t header;      // page 1, kept in memory while the database is open
    bool changed;               // has anything been written yet? (a version is kept the first time)
    char error[96];             // what went wrong last, in full (see aerdb_error)
    int changes;                // rows the last INSERT, UPDATE or DELETE added, changed or deleted
    // the columns of the last SELECT, so whoever prints the rows can give them a heading
    char result_names[AERDB_COLUMNS_MAX][AERDB_NAME_MAX];
    int result_count;
} aerdb_t;

// What a SELECT calls with each row it gives back: the row's values, how many, and the `ctx`
// that was passed to aerdb_exec. The values are only good until it returns (copy any you keep).
// Give back true to stop early
typedef bool (*aerdb_row_fn)(const db_value_t* values, int count, void* ctx);

// ================================================================================================
//  Using a database (what the rest of AerOS calls)
// ================================================================================================

// open a database, making an empty one if the file isn't there. NULL if it can't be opened
// (*result says why: AERDB_ERR_NOT_DB, AERDB_ERR_IO...)
aerdb_t* aerdb_open (const char* folder, const char* name, int* result);

// close a database. Everything is written as it happens, so this only frees the memory
void aerdb_close (aerdb_t* db);

// Run one SQL statement. A ? in it takes the next of params[] (param_count of them). A SELECT
// calls row_out for each row (it may be NULL for statements that give back no rows)
int aerdb_exec (aerdb_t* db, const char* sql, const db_value_t* params, int param_count,
                aerdb_row_fn row_out, void* ctx);

// give back the space of deleted rows and dropped tables, making the file smaller
int aerdb_compact (aerdb_t* db);

// what went wrong last, in full: "no such table: items"
const char* aerdb_error (const aerdb_t* db);

// a short description of a result code
const char* aerdb_strerror (int result);

// ================================================================================================
//  Inside AerDB (shared between its own files)
// ================================================================================================

// ---- file.c: pages ----
bool page_read (aerdb_t* db, uint32_t n, void* buffer);
bool page_write (aerdb_t* db, uint32_t n, const void* buffer);
uint32_t page_new (aerdb_t* db);
bool header_save (aerdb_t* db);
int aerdb_fail (aerdb_t* db, int result, const char* format, ...) __attribute__((format(printf, 3, 4)));

// ---- table.c: tables and rows ----
aerdb_table_t* table_find (aerdb_t* db, const char* name);
int table_create (aerdb_t* db, const char* name, const char* columns);
int table_drop (aerdb_t* db, const char* name);
int table_columns (const aerdb_table_t* table, char names[][AERDB_NAME_MAX], int max);
int table_insert (aerdb_t* db, aerdb_table_t* table, const db_value_t* values, int count);
int table_append_row (aerdb_t* db, aerdb_table_t* table, uint32_t rowid, const uint8_t* row, uint16_t size);

// what table_scan calls for each row (true stops), and what table_delete_where asks (true deletes)
typedef bool (*table_row_fn)(uint32_t rowid, const db_value_t* values, int count, void* ctx);
void table_scan (aerdb_t* db, const aerdb_table_t* table, table_row_fn fn, void* ctx);
int table_delete_where (aerdb_t* db, const aerdb_table_t* table, table_row_fn should_delete, void* ctx);

// ---- value.c: values and rows ----
uint16_t row_encode (const db_value_t* values, int count, uint8_t* out, uint16_t room);
int row_decode (const uint8_t* bytes, uint16_t length, db_value_t* values, int max);
int value_compare (const db_value_t* a, const db_value_t* b);
db_value_t value_null (void);

#endif /* AERDB_H */
