/*
    run.c - running a statement: what CREATE, INSERT, SELECT, UPDATE, DELETE and COMPACT do

    aerdb_exec turns the SQL into words, the words into a statement_t (sql.c), then calls the
    function below for that kind of statement:

        CREATE TABLE   table_create
        DROP TABLE     table_drop
        INSERT         puts the values in the table's column order, then table_insert
        SELECT         table_scan, giving back the columns asked for of each row the WHERE allows.
                       With ORDER BY, the rows are kept until they have all been read, then
                       sorted, then given back (see "ORDER BY" below)
        DELETE         table_delete_where, deleting each row the WHERE allows
        UPDATE         the same, but each deleted row first gets its new values, and is added
                       again afterwards with the same rowid
        COMPACT        aerdb_compact: copies every row into fresh pages, leaving out the deleted

    A WHERE is checked against a row by row_matches. Column names are turned into column
    numbers once, before the table is read, so checking a row is only comparing values.

    Until AerDB has a journal (see docs/plans/DATABASE.md), UPDATE and COMPACT are the two that
    a power cut in the middle can lose rows to: both take rows out before putting them back.
*/

#include "sql.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/primary_definitions.h"
#include "../sys/trace.h"

// SELECT ... ORDER BY: one row, kept until every row has been read so they can be sorted.
// It is a copy of the row's bytes, in memory of its own: the values table_scan gives point into
// the page it is reading, and that memory holds a different page once it moves on
typedef struct {
    uint8_t* bytes;     // the row, as row_encode makes it
    uint16_t size;      // how many bytes
    db_value_t key;     // its value in the ORDER BY column (text in it points into `bytes`)
} sort_row_t;

// what running one statement needs to know while it goes through the rows
typedef struct {
    aerdb_t* db;
    const statement_t* s;
    char names[AERDB_COLUMNS_MAX][AERDB_NAME_MAX];   // the table's column names, in order
    int name_count;
    int where_columns[SQL_CONDITIONS_MAX];           // the column number for each condition
    int chosen[AERDB_COLUMNS_MAX];                   // SELECT: the columns to give back. UPDATE: SET's
    int chosen_count;
    aerdb_row_fn row_out;                            // SELECT: where the rows go
    void* ctx;
    int given;                                       // SELECT: rows given back so far
    // SELECT ... ORDER BY
    int order_column;                                // the column to sort by, -1 for no ORDER BY
    sort_row_t* kept;                                // the rows the WHERE allowed, waiting to be sorted
    int kept_count;
    bool out_of_memory;                              // a row couldn't be kept
    // UPDATE: the changed rows, ready to be added again once the old ones are deleted
    struct pending_row {
        uint32_t rowid;
        uint16_t size;
        uint8_t* bytes;
    }* pending;
    int pending_count;
} run_t;

// ---- columns and conditions -----------------------------------------------------------------

// the number of the column called `name` (0 for the first), -1 if the table has no such column
static int column_number (const run_t* run, const char* name) {
    for (int c = 0; c < run->name_count; c++) {
        if (strcasecmp(run->names[c], name) == 0) {
            return c;
        }
    }
    return -1;
}

// Turn a list of column names into column numbers (into numbers[]). The error says which
// name the table doesn't have
static int columns_to_numbers (run_t* run, const char names[][AERDB_NAME_MAX], int count, int* numbers) {
    for (int i = 0; i < count; i++) {
        numbers[i] = column_number(run, names[i]);
        if (numbers[i] < 0) {
            return aerdb_fail(run->db, AERDB_ERR_NOT_FOUND, "%s has no column called %s", run->s->table, names[i]);
        }
    }
    return AERDB_OK;
}

// does `value` pass one condition? A comparison with NULL never does (that's what SQL says):
// IS NULL and IS NOT NULL are how NULLs are looked for
static bool condition_holds (const condition_t* c, const db_value_t* value) {
    if (seq(c->op, "IS")) {
        return value->type == DB_NULL;
    }
    if (seq(c->op, "ISN")) {
        return value->type != DB_NULL;
    }
    if (value->type == DB_NULL || c->value.type == DB_NULL) {
        return false;
    }
    int order = value_compare(value, &c->value);
    if (seq(c->op, "="))  return order == 0;
    if (seq(c->op, "!=")) return order != 0;
    if (seq(c->op, "<"))  return order < 0;
    if (seq(c->op, "<=")) return order <= 0;
    if (seq(c->op, ">"))  return order > 0;
    return order >= 0;    // ">="
}

// does a row pass every condition of the WHERE? A row with fewer values than the table has
// columns counts the missing ones as NULL
static bool row_matches (const run_t* run, const db_value_t* values, int count) {
    for (int w = 0; w < run->s->where_count; w++) {
        int column = run->where_columns[w];
        db_value_t value = column < count ? values[column] : value_null();
        if (!condition_holds(&run->s->where[w], &value)) {
            return false;
        }
    }
    return true;
}

// ---- each kind of statement -------------------------------------------------------------------

// INSERT: put the values in the table's column order (NULL for any column not named), then add them
static int run_insert (run_t* run, aerdb_table_t* table) {
    const statement_t* s = run->s;
    db_value_t row[AERDB_COLUMNS_MAX];
    for (int c = 0; c < run->name_count; c++) {
        row[c] = value_null();
    }

    if (s->column_count == 0) {
        // no columns named: one value for each column, in order
        if (s->value_count != run->name_count) {
            return aerdb_fail(run->db, AERDB_ERR_VALUES, "%s has %d column(s), but %d value(s) were given",
                              table->name, run->name_count, s->value_count);
        }
        for (int c = 0; c < run->name_count; c++) {
            row[c] = s->values[c];
        }
    } else {
        // columns named: each value goes in its column
        if (s->value_count != s->column_count) {
            return aerdb_fail(run->db, AERDB_ERR_VALUES, "%d column(s) were named, but %d value(s) given",
                              s->column_count, s->value_count);
        }
        int numbers[AERDB_COLUMNS_MAX];
        int result = columns_to_numbers(run, s->columns, s->column_count, numbers);
        if (result != AERDB_OK) {
            return result;
        }
        for (int i = 0; i < s->column_count; i++) {
            row[numbers[i]] = s->values[i];
        }
    }

    int result = table_insert(run->db, table, row, run->name_count);
    if (result == AERDB_OK) {
        run->db->changes = 1;
    }
    return result;
}

// SELECT: give one row back to whoever asked (the columns they chose, in their order). Gives
// back true (stop) when they have had enough, or once LIMIT rows have been given
static bool give_row (run_t* run, const db_value_t* values, int count) {
    db_value_t out[AERDB_COLUMNS_MAX];
    for (int i = 0; i < run->chosen_count; i++) {
        int column = run->chosen[i];
        out[i] = column < count ? values[column] : value_null();
    }
    run->given++;
    bool caller_stops = run->row_out != NULL && run->row_out(out, run->chosen_count, run->ctx);
    bool limit_reached = run->s->limit >= 0 && run->given >= run->s->limit;
    return caller_stops || limit_reached;
}

// ---- ORDER BY -----------------------------------------------------------------------------------
//
// The first row given back has to be the smallest, and that might be the table's last row. So
// with ORDER BY, rows aren't given back as they are read: in four steps,
//     keep_row          keeps a copy of each row the WHERE allows (while table_scan reads them)
//     sort_kept_rows    sorts the copies
//     give_kept_rows    gives them back, in order, up to the LIMIT
//     forget_kept_rows  frees the copies

// Keep a copy of one row, and its value in the ORDER BY column, on the end of run->kept. Gives
// back true (stop reading) if there isn't the memory for it
static bool keep_row (run_t* run, const db_value_t* values, int count) {
    // the row as bytes, the way it is stored in a page
    uint8_t bytes[AERDB_PAGE_SIZE];
    uint16_t size = row_encode(values, count, bytes, sizeof(bytes));

    // room for one more in the list, and memory of its own for the bytes
    sort_row_t* bigger = realloc(run->kept, (size_t)(run->kept_count + 1) * sizeof(sort_row_t));
    if (bigger == NULL) {
        run->out_of_memory = true;
        return true;
    }
    run->kept = bigger;
    uint8_t* copy = malloc(size > 0 ? size : 1);
    if (copy == NULL) {
        run->out_of_memory = true;
        return true;
    }
    memcpy(copy, bytes, size);

    // The key is read from the copy, not from `values`: text in `values` points into the page
    // being read, which is about to change. (A missing value at the end of the row is a NULL)
    db_value_t copied[AERDB_COLUMNS_MAX];
    int copied_count = row_decode(copy, size, copied, AERDB_COLUMNS_MAX);
    sort_row_t* row = &run->kept[run->kept_count];
    row->bytes = copy;
    row->size = size;
    if (run->order_column < copied_count) {
        row->key = copied[run->order_column];
    } else {
        row->key = value_null();
    }
    run->kept_count++;
    return false;
}

// Should row a come after row b? Smallest first (ASC), or biggest first (DESC). value_compare
// knows the order of values: NULL first, then numbers by size, then text by its bytes
static bool comes_after (const run_t* run, const sort_row_t* a, const sort_row_t* b) {
    int order = value_compare(&a->key, &b->key);   // -1: a is smaller, 0: the same, 1: a is bigger
    if (run->s->order_desc) {
        return order < 0;
    }
    return order > 0;
}

// Sort the kept rows (an insertion sort): each row in turn slides back past every row before it
// that should come after it. Rows with the same value never pass each other, so they stay in
// the order they were read
static void sort_kept_rows (run_t* run) {
    for (int i = 1; i < run->kept_count; i++) {
        sort_row_t moving = run->kept[i];
        int place = i;
        while (place > 0 && comes_after(run, &run->kept[place - 1], &moving)) {
            run->kept[place] = run->kept[place - 1];   // that one moves along one place
            place--;
        }
        run->kept[place] = moving;
    }
}

// Give the kept rows back, in their sorted order, until the LIMIT (or whoever asked stops)
static void give_kept_rows (run_t* run) {
    for (int i = 0; i < run->kept_count; i++) {
        db_value_t values[AERDB_COLUMNS_MAX];
        int count = row_decode(run->kept[i].bytes, run->kept[i].size, values, AERDB_COLUMNS_MAX);
        if (give_row(run, values, count)) {
            return;
        }
    }
}

// Free every kept row (those past the LIMIT too), and the list
static void forget_kept_rows (run_t* run) {
    for (int i = 0; i < run->kept_count; i++) {
        free(run->kept[i].bytes);
    }
    free(run->kept);
    run->kept = NULL;
    run->kept_count = 0;
}

// ---- SELECT -------------------------------------------------------------------------------------

// SELECT, for one row: if the WHERE allows it, give it back (or with ORDER BY, keep it to sort
// later). Gives back true to stop reading
static bool select_row (uint32_t rowid, const db_value_t* values, int count, void* ctx) {
    (void)rowid;
    run_t* run = ctx;
    if (!row_matches(run, values, count)) {
        return false;
    }
    if (run->order_column >= 0) {
        return keep_row(run, values, count);
    }
    return give_row(run, values, count);
}

// SELECT: which columns, then every row of the table through select_row
static int run_select (run_t* run, aerdb_table_t* table) {
    const statement_t* s = run->s;
    if (s->column_count == 0) {
        // * : every column, in order
        run->chosen_count = run->name_count;
        for (int c = 0; c < run->name_count; c++) {
            run->chosen[c] = c;
        }
    } else {
        run->chosen_count = s->column_count;
        int result = columns_to_numbers(run, s->columns, s->column_count, run->chosen);
        if (result != AERDB_OK) {
            return result;
        }
    }

    // the columns' names, so whoever prints the rows can give them a heading
    for (int i = 0; i < run->chosen_count; i++) {
        scpy(run->db->result_names[i], run->names[run->chosen[i]], AERDB_NAME_MAX);
    }
    run->db->result_count = run->chosen_count;

    // ORDER BY: which column, as a number (-1 means there's no ORDER BY)
    run->order_column = -1;
    if (s->order_by[0] != '\0') {
        run->order_column = column_number(run, s->order_by);
        if (run->order_column < 0) {
            return aerdb_fail(run->db, AERDB_ERR_NOT_FOUND, "%s has no column called %s", s->table, s->order_by);
        }
    }

    if (s->limit != 0) {
        table_scan(run->db, table, select_row, run);
    }

    // without ORDER BY, the rows have been given back already
    if (run->order_column < 0) {
        TRACE("read %s from its first page to its last: %d row(s) matched", table->name, run->given);
        return AERDB_OK;
    }

    // with it, they've only been kept: now sort them and give them back
    if (run->out_of_memory) {
        forget_kept_rows(run);
        return aerdb_fail(run->db, AERDB_ERR_NO_MEMORY, "not enough memory to sort %s", table->name);
    }
    int matched = run->kept_count;
    sort_kept_rows(run);
    give_kept_rows(run);
    forget_kept_rows(run);
    TRACE("read %s from its first page to its last: %d row(s) matched, sorted by %s, %d given back",
          table->name, matched, s->order_by, run->given);
    return AERDB_OK;
}

// DELETE, for one row: delete it if the WHERE allows it
static bool delete_row (uint32_t rowid, const db_value_t* values, int count, void* ctx) {
    (void)rowid;
    return row_matches(ctx, values, count);
}

// UPDATE, for one row: if the WHERE allows it, make the row with its new values (as bytes, kept
// in `pending` until the old rows are deleted), and delete the old one
static bool update_row (uint32_t rowid, const db_value_t* values, int count, void* ctx) {
    run_t* run = ctx;
    if (!row_matches(run, values, count)) {
        return false;
    }

    // the row as it is, with SET's columns changed
    db_value_t row[AERDB_COLUMNS_MAX];
    for (int c = 0; c < run->name_count; c++) {
        row[c] = c < count ? values[c] : value_null();
    }
    for (int i = 0; i < run->chosen_count; i++) {
        row[run->chosen[i]] = run->s->values[i];
    }

    // as bytes, copied: `values` points into a page that is gone once this returns
    uint8_t bytes[AERDB_PAGE_SIZE];
    uint16_t room = AERDB_PAGE_SIZE - sizeof(aerdb_page_header_t) - AERDB_ROW_HEADER;
    uint16_t size = row_encode(row, run->name_count, bytes, room);
    struct pending_row* bigger = realloc(run->pending, (size_t)(run->pending_count + 1) * sizeof(struct pending_row));
    uint8_t* copy = malloc(size > 0 ? size : 1);
    if (bigger == NULL || copy == NULL || (size == 0 && run->name_count > 0)) {
        if (bigger != NULL) {
            run->pending = bigger;
        }
        free(copy);
        return false; // leave this row as it was
    }
    memcpy(copy, bytes, size);
    run->pending = bigger;
    run->pending[run->pending_count].rowid = rowid;
    run->pending[run->pending_count].size = size;
    run->pending[run->pending_count].bytes = copy;
    run->pending_count++;
    return true;
}

// UPDATE: delete each matching row (keeping its new version), then add the new versions back
static int run_update (run_t* run, aerdb_table_t* table) {
    run->chosen_count = run->s->column_count;
    int result = columns_to_numbers(run, run->s->columns, run->s->column_count, run->chosen);
    if (result != AERDB_OK) {
        return result;
    }

    int deleted = table_delete_where(run->db, table, update_row, run);
    result = deleted < 0 ? deleted : AERDB_OK;

    // the new versions go on the end of the table, with the rowids they had
    for (int i = 0; i < run->pending_count; i++) {
        if (result == AERDB_OK) {
            result = table_append_row(run->db, table, run->pending[i].rowid, run->pending[i].bytes,
                                      run->pending[i].size);
        }
        free(run->pending[i].bytes);
    }
    free(run->pending);
    if (result == AERDB_OK) {
        run->db->changes = run->pending_count;
        TRACE("changed %d row(s) of %s: each old one marked deleted, its new one added on the end",
              run->pending_count, table->name);
    }
    return result;
}

// ---- running a statement ------------------------------------------------------------------------

// run a statement that has been parsed (see the top of this file)
static int run_statement (aerdb_t* db, const statement_t* s, aerdb_row_fn row_out, void* ctx) {
    // the ones that don't work on an existing table's rows
    if (s->kind == S_CREATE) {
        // IF NOT EXISTS: a table that is there already is left as it is
        if (s->if_not_exists && table_find(db, s->table) != NULL) {
            return AERDB_OK;
        }
        return table_create(db, s->table, s->create);
    }
    if (s->kind == S_DROP) {
        return table_drop(db, s->table);
    }
    if (s->kind == S_COMPACT) {
        return aerdb_compact(db);
    }

    // the rest need the table, its column names, and the WHERE's columns as numbers
    aerdb_table_t* table = table_find(db, s->table);
    if (table == NULL) {
        return aerdb_fail(db, AERDB_ERR_NOT_FOUND, "no such table: %s", s->table);
    }
    run_t* run = malloc(sizeof(run_t));
    if (run == NULL) {
        return aerdb_fail(db, AERDB_ERR_NO_MEMORY, "not enough memory");
    }
    memset(run, 0, sizeof(run_t));
    run->db = db;
    run->s = s;
    run->row_out = row_out;
    run->ctx = ctx;
    run->name_count = table_columns(table, run->names, AERDB_COLUMNS_MAX);

    int result = AERDB_OK;
    for (int w = 0; w < s->where_count && result == AERDB_OK; w++) {
        run->where_columns[w] = column_number(run, s->where[w].column);
        if (run->where_columns[w] < 0) {
            result = aerdb_fail(db, AERDB_ERR_NOT_FOUND, "%s has no column called %s", s->table, s->where[w].column);
        }
    }

    if (result == AERDB_OK) {
        if (s->kind == S_INSERT) {
            result = run_insert(run, table);
        } else if (s->kind == S_SELECT) {
            result = run_select(run, table);
        } else if (s->kind == S_UPDATE) {
            result = run_update(run, table);
        } else {
            int deleted = table_delete_where(db, table, delete_row, run);
            result = deleted < 0 ? deleted : AERDB_OK;
            db->changes = deleted < 0 ? 0 : deleted;
        }
    }
    free(run);
    return result;
}

// run one SQL statement (see aerdb.h)
int aerdb_exec (aerdb_t* db, const char* sql, const db_value_t* params, int param_count, aerdb_row_fn row_out, void* ctx) {
    db->error[0] = '\0';
    db->changes = 0;
    db->result_count = 0;

    // the words and the statement are big, so they go on the heap rather than the stack
    word_t* words = malloc(SQL_WORDS_MAX * sizeof(word_t));
    statement_t* s = malloc(sizeof(statement_t));
    int result = AERDB_ERR_NO_MEMORY;

    if (words != NULL && s != NULL) {
        int count = sql_words(db, sql, words, SQL_WORDS_MAX);
        result = count < 0 ? count : sql_parse(db, words, count, params, param_count, s);
        if (result == AERDB_OK) {
            result = run_statement(db, s, row_out, ctx);
        }
    } else {
        aerdb_fail(db, AERDB_ERR_NO_MEMORY, "not enough memory");
    }
    free(words);
    free(s);

    // every failure leaves a message; one without (it can't happen, but just in case) gets one
    if (result != AERDB_OK && db->error[0] == '\0') {
        aerdb_fail(db, result, "%s", aerdb_strerror(result));
    }
    return result;
}

// ---- compacting ---------------------------------------------------------------------------------

// one row kept by aerdb_compact while the pages are emptied: which table, its rowid, its bytes
typedef struct {
    int table;
    uint32_t rowid;
    uint16_t size;
    uint8_t* bytes;
} kept_row_t;

// what compact_keep adds to
typedef struct {
    kept_row_t* rows;
    int count;
    int table;           // the table being read
    bool out_of_memory;
} compact_t;

// aerdb_compact, for one row: keep a copy of it (as bytes)
static bool compact_keep (uint32_t rowid, const db_value_t* values, int count, void* ctx) {
    compact_t* compact = ctx;
    uint8_t bytes[AERDB_PAGE_SIZE];
    uint16_t size = row_encode(values, count, bytes, sizeof(bytes));

    kept_row_t* bigger = realloc(compact->rows, (size_t)(compact->count + 1) * sizeof(kept_row_t));
    uint8_t* copy = malloc(size > 0 ? size : 1);
    if (bigger == NULL || copy == NULL) {
        if (bigger != NULL) {
            compact->rows = bigger;
        }
        free(copy);
        compact->out_of_memory = true;
        return true; // stop
    }
    memcpy(copy, bytes, size);
    compact->rows = bigger;
    compact->rows[compact->count] = (kept_row_t){ compact->table, rowid, size, copy };
    compact->count++;
    return false;
}

// Give back the space of deleted rows and dropped tables (see aerdb.h). In four steps:
//   1. copy every row that isn't deleted, of every table, into memory
//   2. forget every table's pages: the file is back to page 1 alone
//   3. add the rows back, table by table, in the same order and with the same rowids
//   4. cut the file down to the pages it now uses
int aerdb_compact (aerdb_t* db) {
    // 1. every row, copied (nothing has changed yet, so running out of memory changes nothing)
    compact_t compact = { NULL, 0, 0, false };
    uint32_t pages_before = db->header.page_count;
    for (uint32_t t = 0; t < db->header.table_count && !compact.out_of_memory; t++) {
        compact.table = (int)t;
        table_scan(db, &db->header.tables[t], compact_keep, &compact);
    }
    if (compact.out_of_memory) {
        for (int i = 0; i < compact.count; i++) {
            free(compact.rows[i].bytes);
        }
        free(compact.rows);
        return aerdb_fail(db, AERDB_ERR_NO_MEMORY, "not enough memory to compact the database");
    }

    // 2. no pages but page 1 (each table keeps its next_rowid, so rowids are never reused)
    db->header.page_count = 1;
    for (uint32_t t = 0; t < db->header.table_count; t++) {
        db->header.tables[t].first_page = 0;
        db->header.tables[t].last_page = 0;
    }
    int result = header_save(db) ? AERDB_OK : aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");

    // 3. the rows back, onto fresh pages
    for (int i = 0; i < compact.count; i++) {
        kept_row_t* row = &compact.rows[i];
        if (result == AERDB_OK) {
            result = table_append_row(db, &db->header.tables[row->table], row->rowid, row->bytes, row->size);
        }
        free(row->bytes);
    }
    free(compact.rows);

    // 4. the pages past the new end belong to nothing: cut them off
    if (result == AERDB_OK && fs_truncate(db->folder, db->name, db->header.page_count * AERDB_PAGE_SIZE) != FS_OK) {
        result = aerdb_fail(db, AERDB_ERR_IO, "couldn't make the file smaller");
    }
    if (result == AERDB_OK) {
        TRACE("compacted: %d row(s) copied onto fresh pages, %u page(s) down to %u", compact.count,
              (unsigned int)pages_before, (unsigned int)db->header.page_count);
    }
    return result;
}
