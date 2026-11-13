# Plan: AerDB, a database of AerOS's own

> **Where it stands:** steps 1 to 5 are built, in `src/db/` and `lang/bi_database.c`, and so
> are COMPACT, the `SQL` command and SHOW's notes from step 6. Still to do: a WHY explanation
> for database problems, and everything under "Later". The tests are `tools/test/tests/
> test_database.py` and `tools/test/files/aerdb.aer`.

AerDB is a small database engine of AerOS's own: tables, rows and a little SQL, kept in one file.
This plan gets the **basics working first**, in a simple file format of AerDB's own. Speed
(B-trees), crash safety (a journal) and SQLite compatibility come later, once the basics are solid,
and the design leaves room for them (see "Later" at the end).

There are six steps. Each one adds one thing, shows the code it needs, and ends with a test.


## The idea in one picture

A database is one file, made of **pages** of 4096 bytes. Page 1 is the database's own header:
which tables there are and where they start. Every other page holds rows, and each table is a
**chain** of pages: each page says which page comes next.

```
 shop.db:  [ page 1          ][ page 2          ][ page 3          ][ page 4         ]
             the tables:        items, rows 1-60   people, rows 1-5   items, rows 61-90
             items:  2 ... 4    next page: 4       next page: none    next page: none
             people: 3 ... 3
```

- **Adding a row** puts it on the end of the table's last page, or on a new page when that one
  is full.
- **Reading a table** follows its chain from the first page, reading every row.
- **Deleting a row** marks it deleted; its space is only given back by a "compact" later.

No tree, nothing moves once it is written, and every row is found the same way: that is what
keeps it simple.


## The file format

Numbers are stored the way the x86 processor keeps them in memory (little end first), so a
`memcpy` into a `uint32_t` reads one, and C structs can describe the layout directly.

**Page 1:** the header, then one entry for each table.

```c
#define AERDB_PAGE_SIZE  4096
#define AERDB_MAGIC      "AERDB01"       // 7 characters and a '\0': 8 bytes
#define AERDB_TABLES_MAX 20              // as many as fit on page 1

typedef struct __attribute__((packed)) {
    char name[24];         // the table's name
    char columns[128];     // its columns as written in CREATE TABLE: "name TEXT, price REAL"
    uint32_t first_page;   // where its chain of pages starts (0: no rows yet)
    uint32_t last_page;    // where it ends: new rows go here
    uint32_t next_rowid;   // the number the next row gets
} aerdb_table_t;           // 164 bytes

typedef struct __attribute__((packed)) {
    char magic[8];         // AERDB_MAGIC: "this is an AerDB file"
    uint32_t page_count;   // how many pages the file has
    uint32_t table_count;
    aerdb_table_t tables[AERDB_TABLES_MAX];
} aerdb_header_t;          // 3296 bytes: it fits on page 1
```

**A data page:** a small header, then the rows one after another.

```c
typedef struct __attribute__((packed)) {
    uint32_t next_page;    // the next page of this table's chain, 0 for the last
    uint16_t used;         // bytes used on this page, counting this header
    uint16_t rows;         // rows on this page (deleted ones too)
} aerdb_page_header_t;     // 8 bytes, then the rows
```

**A row:** its length, a flags byte, its rowid, then its values.

```
 [ length: 2 bytes ][ flags: 1 byte ][ rowid: 4 bytes ][ value ][ value ] ...
                      bit 0: deleted
```

and each **value** is a type byte, then what that type needs:

| Type byte | Value | Then |
| --- | --- | --- |
| 0 | NULL | nothing |
| 1 | an integer | 4 bytes (`int32_t`, the same size as AerLang's numbers) |
| 2 | a real number | 8 bytes (`double`) |
| 3 | text | a 2 byte length, then the characters (no `'\0'`) |
| 4 | a blob | a 2 byte length, then the bytes |

A row has to fit on one page (about 4000 bytes) in this first version: anything bigger is
refused with a clear message.


## Where things go

All of AerDB goes in `src/db/` (add `db/*.c` and `db/*.h` to `SOURCES` and `HEADERS` in the
Makefile, whose wildcards don't reach a new folder).

| File | Holds | Step |
| --- | --- | --- |
| `db/aerdb.h` | the file format above, the public functions, and how AerDB works | 1 |
| `db/file.c` | making, opening and closing a database; reading and writing pages | 1 |
| `db/table.c` | making and finding tables; adding, reading and deleting rows | 2, 3 |
| `db/value.c` | values, and turning a row into bytes and back | 3 |
| `db/sql.c` | splitting SQL into words, and parsing it | 4 |
| `db/run.c` | running each kind of statement | 4 |
| `lang/bi_database.c` | the AerLang functions | 5 |


## AerOS functions you'll use

| Function | For |
| --- | --- |
| `fs_split_path(input, folder, size, name, size)` | turning `"shop.db"` into a folder and a name |
| `fs_file_exists(folder, name)` | does the database exist yet? |
| `fs_create_bytes(folder, name, data, length)` | making a new database file (page 1) |
| `fs_open(path, &file)` | opening it to read |
| `fs_read_at(&file, offset, buffer, length)` | reading a page |
| `fs_write_at(folder, name, offset, data, length)` | writing a page, in place |
| `fs_truncate(folder, name, size)` | making the file shorter (after a compact) |
| `versions_keep(folder, name)` | keeping a version before the first change, for UNDO |
| `malloc`, `free`, `memcpy`, `memset`, `memcmp`, `scpy`, `strcasecmp` | memory and text |
| `prtf`, `sfmt` | printing, and making strings |
| `TRACE("...")` | notes for SHOW |

They are in `src/sys/f_sys.h`, `src/sys/versions.h`, `src/lib/` and `src/sys/trace.h`. The file
system's functions return `FS_OK` or a negative `FS_ERR_` code.


## Step 1: make, open and close a database

```c
// db/aerdb.h

typedef struct {
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    aerdb_header_t header;     // page 1, kept in memory while the database is open
    bool changed;              // has anything been written yet? (for versions_keep, once)
} aerdb_t;
```

```c
// db/file.c

// the byte where page n starts (pages count from 1)
static uint32_t page_offset (uint32_t n) {
    return (n - 1) * AERDB_PAGE_SIZE;
}

// read page n into buffer (AERDB_PAGE_SIZE bytes). False on a disk error
bool page_read (aerdb_t* db, uint32_t n, void* buffer) {
    char path[FS_PATH_MAX + FS_NAME_MAX];
    sfmt(path, sizeof(path), "%s%s", db->folder, db->name);
    fs_file_t file;
    return fs_open(path, &file) &&
           fs_read_at(&file, page_offset(n), buffer, AERDB_PAGE_SIZE) == AERDB_PAGE_SIZE;
}

// write page n from buffer, in place (the first change keeps a version of the file, for UNDO)
bool page_write (aerdb_t* db, uint32_t n, const void* buffer) {
    if (!db->changed) {
        versions_keep(db->folder, db->name);
        db->changed = true;
    }
    return fs_write_at(db->folder, db->name, page_offset(n), buffer, AERDB_PAGE_SIZE) == FS_OK;
}

// open a database, making it if it isn't there. NULL if it isn't an AerDB file
aerdb_t* aerdb_open (const char* folder, const char* name) {
    aerdb_t* db = malloc(sizeof(aerdb_t));
    if (db == NULL) {
        return NULL;
    }
    memset(db, 0, sizeof(aerdb_t));
    scpy(db->folder, folder, sizeof(db->folder));
    scpy(db->name, name, sizeof(db->name));

    if (!fs_file_exists(folder, name)) {
        // a new database: page 1 alone, with no tables
        uint8_t page[AERDB_PAGE_SIZE] = { 0 };
        aerdb_header_t* header = (aerdb_header_t*)page;
        memcpy(header->magic, AERDB_MAGIC, 8);
        header->page_count = 1;
        if (fs_create_bytes(folder, name, page, AERDB_PAGE_SIZE) != FS_OK) {
            free(db);
            return NULL;
        }
    }

    // page 1 is the header: keep it in memory, and check it really is an AerDB file
    uint8_t page[AERDB_PAGE_SIZE];
    if (!page_read(db, 1, page) || memcmp(page, AERDB_MAGIC, 8) != 0) {
        free(db);
        return NULL;
    }
    memcpy(&db->header, page, sizeof(aerdb_header_t));
    return db;
}

// write the header (page 1) back after it changed
bool header_save (aerdb_t* db);

// a new, empty page on the end of the file: its number
uint32_t page_new (aerdb_t* db);

// close a database (everything is written as it happens, so this only frees it)
void aerdb_close (aerdb_t* db);
```

`page_new` adds one to `header.page_count`, writes an empty page there (`fs_write_at` past the
end makes the file longer) and saves the header.

**You write:** `page_offset`, `page_read`, `page_write`, `page_new`, `header_save`,
`aerdb_open`, `aerdb_close`.
**Test:** open a new database twice: the second time it is found and read back. Use `rdat` in
an AerLang test to check the file is 4096 bytes and starts with `AERDB01`.


## Step 2: make and find tables

```c
// db/table.c

// the table called `name`, NULL if there is no such table (names ignore case, like AerOS's files)
aerdb_table_t* table_find (aerdb_t* db, const char* name) {
    for (uint32_t i = 0; i < db->header.table_count; i++) {
        if (strcasecmp(db->header.tables[i].name, name) == 0) {
            return &db->header.tables[i];
        }
    }
    return NULL;
}

// make a table. columns is the text between CREATE TABLE's brackets: "name TEXT, price REAL"
int table_create (aerdb_t* db, const char* name, const char* columns) {
    if (table_find(db, name) != NULL) {
        return AERDB_ERR_EXISTS;
    }
    if (db->header.table_count == AERDB_TABLES_MAX) {
        return AERDB_ERR_FULL;
    }
    aerdb_table_t* t = &db->header.tables[db->header.table_count++];
    memset(t, 0, sizeof(*t));
    scpy(t->name, name, sizeof(t->name));
    scpy(t->columns, columns, sizeof(t->columns));
    t->next_rowid = 1;
    TRACE("made the table %s (%s), with no pages yet", name, columns);
    return header_save(db) ? AERDB_OK : AERDB_ERR_IO;
}

// the column names of a table, split from its columns text: "name TEXT, price REAL" -> name, price
int table_columns (const aerdb_table_t* t, char names[][24], int max);
```

A table has no pages until its first row arrives. Give AerDB its own result codes
(`AERDB_OK`, `AERDB_ERR_EXISTS`, `AERDB_ERR_NOT_FOUND`, `AERDB_ERR_FULL`, `AERDB_ERR_IO`,
`AERDB_ERR_SYNTAX`...) and an `aerdb_strerror(code)`, as the file system has.

**You write:** `table_find`, `table_create`, `table_columns`, `table_drop` (later),
`aerdb_strerror`.
**Test:** make two tables, close the database, open it again, and both are found with their
columns.


## Step 3: add and read rows

**Values** in memory:

```c
// db/value.c

typedef enum { DB_NULL, DB_INT, DB_REAL, DB_TEXT, DB_BLOB } db_type_t;

typedef struct {
    db_type_t type;
    int32_t i;               // DB_INT
    double r;                // DB_REAL
    const uint8_t* bytes;    // DB_TEXT, DB_BLOB: where they are (in the page, or the caller's)
    uint16_t length;
} db_value_t;

// write a row's values as bytes (the format above) into out. Gives back the size, 0 if too big
uint16_t row_encode (const db_value_t* values, int count, uint8_t* out, uint16_t room);

// read a row's values from its bytes into values[], giving back how many there were
int row_decode (const uint8_t* row, uint16_t length, db_value_t* values, int max);
```

**Adding a row** goes on the end of the table's last page:

```c
// db/table.c

// add a row to a table, on the end of its last page (or a new one if that is full)
int table_insert (aerdb_t* db, aerdb_table_t* t, const db_value_t* values, int count) {
    uint8_t row[AERDB_PAGE_SIZE];
    // the row: length, flags, rowid, then the values
    uint16_t size = row_encode(values, count, row + 7, sizeof(row) - 7 - sizeof(aerdb_page_header_t));
    if (size == 0) {
        return AERDB_ERR_TOO_BIG;
    }
    size += 7;
    uint32_t rowid = t->next_rowid++;
    memcpy(row, &size, 2);
    row[2] = 0;                       // flags: not deleted
    memcpy(row + 3, &rowid, 4);

    // the last page, or a new one if there is none yet or the row won't fit on it
    uint8_t page[AERDB_PAGE_SIZE];
    aerdb_page_header_t* ph = (aerdb_page_header_t*)page;
    if (t->last_page == 0 || !page_read(db, t->last_page, page) || ph->used + size > AERDB_PAGE_SIZE) {
        uint32_t fresh = page_new(db);
        if (t->last_page != 0) {
            // link the old last page to the new one
            ph->next_page = fresh;
            page_write(db, t->last_page, page);
        } else {
            t->first_page = fresh;
        }
        t->last_page = fresh;
        memset(page, 0, sizeof(page));
        ph->used = sizeof(aerdb_page_header_t);
        TRACE("%s's last page was full: started page %u", t->name, fresh);
    }

    // the row goes on the end of what is used
    memcpy(page + ph->used, row, size);
    ph->used += size;
    ph->rows++;
    page_write(db, t->last_page, page);
    header_save(db);                  // next_rowid (and maybe first / last page) changed
    return AERDB_OK;
}
```

**Reading a table** follows the chain, skipping deleted rows:

```c
// call fn for every row of a table that isn't deleted, in the order they were added
typedef bool (*row_fn)(uint32_t rowid, const db_value_t* values, int count, void* ctx);  // true stops

void table_scan (aerdb_t* db, const aerdb_table_t* t, row_fn fn, void* ctx) {
    uint8_t page[AERDB_PAGE_SIZE];
    for (uint32_t n = t->first_page; n != 0; ) {
        if (!page_read(db, n, page)) {
            return;
        }
        aerdb_page_header_t* ph = (aerdb_page_header_t*)page;
        uint16_t at = sizeof(aerdb_page_header_t);
        for (uint16_t r = 0; r < ph->rows; r++) {
            uint16_t size;
            uint32_t rowid;
            memcpy(&size, page + at, 2);
            memcpy(&rowid, page + at + 3, 4);
            if ((page[at + 2] & 1) == 0) {            // not deleted
                db_value_t values[16];
                int count = row_decode(page + at + 7, size - 7, values, 16);
                if (fn(rowid, values, count, ctx)) {
                    return;
                }
            }
            at += size;
        }
        n = ph->next_page;
    }
}
```

**You write:** `row_encode`, `row_decode`, `table_insert`, `table_scan`.
**Test:** add a few hundred rows of all five types (enough to fill several pages), close and
open the database, and read every one back.


## Step 4: SQL

Now the same things, written as SQL. The first version understands:

```sql
CREATE TABLE items (name TEXT, price REAL)
INSERT INTO items VALUES ('tea', 2.5)
INSERT INTO items VALUES (?, ?)
SELECT * FROM items
SELECT name, price FROM items WHERE price < 3 AND name != 'milk'
DELETE FROM items WHERE name = 'tea'
UPDATE items SET price = 3.0 WHERE name = 'tea'
```

and anything else gets a clear "AerDB doesn't support ... yet".

**1. Split it into words** (`db/sql.c`):

```
 SELECT name, price FROM items WHERE price < ?
   ->  [SELECT] [name] [,] [price] [FROM] [items] [WHERE] [price] [<] [?]
```

```c
typedef enum { W_NAME, W_NUMBER, W_TEXT, W_SYMBOL, W_PARAM, W_END } word_kind_t;

typedef struct {
    word_kind_t kind;
    char text[64];         // the word: SELECT, items, 2.5, tea (without its quotes), <=
} word_t;

// split SQL into words, giving back how many (at most max)
int sql_words (const char* sql, word_t* words, int max);
```

**2. Parse it** into a statement, one small function for each kind:

```c
typedef struct {
    char column[24];       // compare this column...
    char op[3];            // ...with "=", "!=", "<", "<=", ">" or ">="...
    db_value_t value;      // ...against this (from the SQL, or a ? filled in from the params)
} condition_t;

typedef struct {
    enum { S_CREATE, S_INSERT, S_SELECT, S_DELETE, S_UPDATE } kind;
    char table[24];
    char columns[16][24];      // SELECT's columns ("*" is column_count 0); UPDATE's SET columns
    int column_count;
    db_value_t values[16];     // INSERT's values; UPDATE's new values
    int value_count;
    condition_t where[8];      // WHERE ... AND ... AND ... (AND only, at first)
    int where_count;
    char create[128];          // CREATE TABLE's columns text
} statement_t;

// parse SQL into a statement, giving back AERDB_OK or AERDB_ERR_SYNTAX
int sql_parse (const word_t* words, int count, statement_t* out);
```

Keeping WHERE to "column op value AND column op value" makes the first version much simpler
than a full expression parser, and covers most of what programs ask. OR and brackets can come
later.

**3. Run it** (`db/run.c`):

```c
// does a row pass every condition of the WHERE?
bool row_matches (const statement_t* s, const char names[][24], int columns, const db_value_t* row);

// compare two values: -1, 0 or 1. NULL first, then numbers (integers and reals together), then text
int value_compare (const db_value_t* a, const db_value_t* b);

// run a statement. SELECT calls row_out for each row it gives back
int aerdb_exec (aerdb_t* db, const char* sql, const db_value_t* params, int param_count,
                row_fn row_out, void* ctx);
```

| Statement | What running it does |
| --- | --- |
| CREATE TABLE | `table_create` |
| INSERT | the values (with `?`s filled in from params) go to `table_insert` |
| SELECT | `table_scan`, and for each row that `row_matches`, the columns asked for go to `row_out` |
| DELETE | `table_scan`, and each row that matches gets its deleted flag set (`table_delete`) |
| UPDATE | the same, then the changed row is added again with `table_insert` (same rowid) |

`table_delete(db, t, rowid)` finds the row's page, sets bit 0 of its flags and writes the page
back. UPDATE keeping the rowid means passing it to a version of `table_insert` that doesn't
take a new one.

**You write:** `sql_words`, `sql_parse` (with `parse_create`, `parse_insert`, `parse_select`,
`parse_where`...), `value_compare`, `row_matches`, `table_delete`, `aerdb_exec`.
**Test:** an AerLang test program (step 5) that runs each kind of statement and checks the rows
that come back, like `tools/test/files/pages.aer` does for files.


## Step 5: AerLang

```c
// lang/bi_database.c, following the steps at the top of lang/builtins.h

// dbopen(path): open a database (making it if it isn't there): a number for it, nil if it can't
value_t bi_dbopen (int argc, value_t* argv, int line);
// dbrun(db, sql, params): run a statement that gives back no rows. True if it worked
value_t bi_dbrun (int argc, value_t* argv, int line);
// dbquery(db, sql, params): run a SELECT: a list of rows, each a list of values
value_t bi_dbquery (int argc, value_t* argv, int line);
// dbclose(db): close it
value_t bi_dbclose (int argc, value_t* argv, int line);
```

```
let db = dbopen("shop.db")
dbrun(db, "CREATE TABLE items (name TEXT, price REAL)")
dbrun(db, "INSERT INTO items VALUES (?, ?)", ["tea", 2.5])
dbrun(db, "INSERT INTO items VALUES (?, ?)", ["cake", 4.0])
for row in dbquery(db, "SELECT name, price FROM items WHERE price < ?", [3]) do
    print(row[0], row[1])                         # tea 2.5
end
dbclose(db)
```

- **Handles:** keep the open databases in a small array, and give AerLang the position as its
  number, the way `splay` gives a number for a sound. Close any still open when the program
  ends (`aer_builtins_finish` in `lang/aer_builtins.c`).
- **Permissions:** `dbopen` gets the path with `file_arg(argv, 0, line, "dbopen", PERM_CHANGE,
  folder, name)`, so a program asks before opening a database in your home folder.
- **Values:** AerLang to AerDB: `V_INT`, `V_FLOAT`, `V_STR`, `V_BYTES` and `V_NIL` map straight
  onto the five AerDB types. AerDB back to AerLang: `aer_int()`, `aer_float()`,
  `aer_string(chars, length)`, `aer_bytes(length)` then a `memcpy`, and `aer_nil()`. A row is an
  `aer_list()` filled with `aer_list_push()`, and the result is a list of those.
- **Errors:** a mistake in the SQL stops the program with
  `aer_error(line, "dbrun(): %s", aerdb_strerror(result))`, so WHY shows the line it was on.
- **App mode:** add the four names to the "files" list in `lang/aer_builtins.c`.

**Test:** `tools/test/files/aerdb.aer` (made like `pages.aer`: `check()` lines and ALL PASSED),
run on a test disk by a test in `tools/test/tests/`, with `t.check_disk(disk)` afterwards.


## Step 6: tidying up

- **Compact:** deleted rows still take space. `aerdb_compact(db)` (or SQL `VACUUM`) copies each
  table's live rows into a fresh chain of pages, then `fs_truncate`s the file to its new length.
- **An `sql` command** (`sys/terminal/cmd_database.c`, following `commands.h`): `sql shop.db`
  opens a prompt that runs SQL and prints the rows as a table.
- **WHY:** a `PROBLEM_DATABASE` in `sys/explain.c`, for "no such table", "the row is too big"
  and the like.
- **SHOW:** a few more `TRACE()` lines ("read page 4 of items: 52 rows", "row 17 marked
  deleted"), so `show dbrun(...)` explains what a statement did.
- **Docs:** a manual page `src/disk/docs/database.md`, a section in `docs/AERLANG.md`, and a
  first app that uses it: high scores for Snake, or a notes app.


## Later

Once the basics work and have tests, these make AerDB faster, safer and compatible. Each one
changes the inside of one layer and leaves the rest alone:

- **A page cache:** keep recently used pages in memory, instead of reading them every time.
- **Indexes:** a way to find a row without reading the whole table (`CREATE INDEX`, as a
  B-tree or even a sorted list of rowids per value).
- **A journal**, so a change is all there or not at all if the power goes: save each page's old
  bytes in `shop.dbj` before changing it, and delete the journal at COMMIT. On opening, a
  journal still there means "put those pages back".
- **Bigger rows** that carry on onto extra pages, and more of SQL: OR, brackets, ORDER BY,
  LIMIT, `count(*)`, joins.
- **SQLite's file format**, so Python and other programs can open AerDB's files
  (https://www.sqlite.org/fileformat2.html). The biggest change: B-trees instead of chains, and
  big-endian numbers (read with `get16` / `get32` helpers rather than `memcpy`). The SQL and
  AerLang layers above can stay as they are.


## Things to watch for

- **The header is kept in memory** while a database is open: save it (`header_save`) every time
  a table, a page count or a `next_rowid` changes, or a reopened database won't know about it.
- **Pages are 4096 bytes:** put them on the heap (`malloc`) rather than the stack where a
  function needs several at once.
- **Write whole pages:** every disk write is flushed straight away, and a whole page in one
  `fs_write_at` is far quicker than many small writes.
- **Test as you go:** each step's test is small, and catches mistakes before the next step
  builds on them.
