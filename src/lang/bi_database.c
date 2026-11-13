/*
    bi_database.c
    AerLang's built-in functions for databases (AerDB, see db/aerdb.h):

        let db = dbopen("shop.db")                                  # make it, or open it
        dbrun(db, "CREATE TABLE items (name TEXT, price REAL)")
        dbrun(db, "INSERT INTO items VALUES (?, ?)", ["tea", 2.5])
        for row in dbquery(db, "SELECT name, price FROM items WHERE price < 3") do
            print(row[0], row[1])
        end
        dbclose(db)

    A database is given to the program as a number: its place in open_databases[] below, plus
    one (so 0 is never a database). Any still open when the program ends are closed then
    (database_finish, called by aer_builtins_finish).

    Values go across like this:
        AerLang          AerDB         AerLang
        nil         ->   NULL     ->   nil
        a number    ->   INT      ->   a number
        a decimal   ->   REAL     ->   a decimal
        text        ->   TEXT     ->   text
        bytes       ->   BLOB     ->   bytes
        true, false ->   INT 1, 0

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../db/aerdb.h"
#include "../lib/aer_string.h"

#define DATABASES_MAX 8   // databases one program can have open at once

static aerdb_t* open_databases[DATABASES_MAX];

// The database a program's number stands for (argument i). Stops the program with an error if
// it isn't one that is open
static aerdb_t* database_arg (value_t* argv, int i, int line, const char* fn) {
    int32_t number = int_arg(argv, i, line, fn);
    if (number < 1 || number > DATABASES_MAX || open_databases[number - 1] == NULL) {
        aer_error(line, "%s() needs a database from dbopen(), and %d isn't an open one", fn, (int)number);
    }
    return open_databases[number - 1];
}

// One AerLang value as an AerDB value. Text and bytes aren't copied: the AerDB value points at
// the AerLang one, which stays while the statement runs
static db_value_t to_database (value_t v, int line, const char* fn) {
    db_value_t out = value_null();
    if (v.type == V_INT) {
        out.type = DB_INT;
        out.i = v.as.i;
    } else if (v.type == V_BOOL) {
        out.type = DB_INT;
        out.i = v.as.i ? 1 : 0;
    } else if (v.type == V_FLOAT) {
        out.type = DB_REAL;
        out.r = v.as.f;
    } else if (v.type == V_STR || v.type == V_BYTES) {
        if (v.as.obj->length > 0xFFFF) {
            aer_error(line, "%s(): a value can be at most 65535 bytes", fn);
        }
        out.type = v.type == V_STR ? DB_TEXT : DB_BLOB;
        out.bytes = v.type == V_STR ? (const uint8_t*)v.as.obj->data.chars : v.as.obj->data.bytes;
        out.length = (uint16_t)v.as.obj->length;
    } else if (v.type != V_NIL) {
        aer_error(line, "%s() can't store a list or a function in a database", fn);
    }
    return out;
}

// one AerDB value as a new AerLang value (copied: the AerDB one is only there for a moment)
static value_t from_database (const db_value_t* v) {
    if (v->type == DB_INT) {
        return aer_int(v->i);
    }
    if (v->type == DB_REAL) {
        return aer_float(v->r);
    }
    if (v->type == DB_TEXT) {
        return aer_string((const char*)v->bytes, v->length);
    }
    if (v->type == DB_BLOB) {
        value_t b = aer_bytes(v->length);
        memcpy(b.as.obj->data.bytes, v->bytes, v->length);
        return b;
    }
    return aer_nil();
}

// The ? values for a statement: argument i, a list (or nothing at all, if it wasn't given).
// Gives back how many, put in params[]
static int params_arg (int argc, value_t* argv, int i, int line, const char* fn, db_value_t* params) {
    if (argc <= i) {
        return 0;
    }
    aer_object_t* list = list_arg(argv, i, line, fn);
    if (list->length > AERDB_COLUMNS_MAX) {
        aer_error(line, "%s(): at most %d values for the ?s", fn, AERDB_COLUMNS_MAX);
    }
    for (uint32_t p = 0; p < list->length; p++) {
        params[p] = to_database(list->data.items[p], line, fn);
    }
    return (int)list->length;
}

// dbopen(path): open a database, making an empty one if the file isn't there. A number for it,
// to give the other db functions, or nil if it can't be opened (it isn't a database, say)
value_t bi_dbopen (int argc, value_t* argv, int line) {
    (void)argc;
    char folder[FS_PATH_MAX], name[FS_NAME_MAX];
    // PERM_CHANGE: a program asks before it opens a database of yours, as it may change it
    if (!file_arg(argv, 0, line, "dbopen", PERM_CHANGE, folder, name)) {
        return aer_nil();
    }
    // a free place for it
    int slot = 0;
    while (slot < DATABASES_MAX && open_databases[slot] != NULL) {
        slot++;
    }
    if (slot == DATABASES_MAX) {
        aer_error(line, "dbopen(): at most %d databases can be open at once", DATABASES_MAX);
    }
    int result;
    open_databases[slot] = aerdb_open(folder, name, &result);
    if (open_databases[slot] == NULL) {
        return aer_nil();
    }
    return aer_int(slot + 1);
}

// dbrun(db, sql, values): run a statement that gives back no rows (CREATE, INSERT, UPDATE,
// DELETE...). Each ? in the SQL takes the next of the values. Gives back how many rows it
// added, changed or deleted. A mistake stops the program, saying what was wrong
value_t bi_dbrun (int argc, value_t* argv, int line) {
    aerdb_t* db = database_arg(argv, 0, line, "dbrun");
    const char* sql = str_arg(argv, 1, line, "dbrun")->data.chars;
    db_value_t params[AERDB_COLUMNS_MAX];
    int count = params_arg(argc, argv, 2, line, "dbrun", params);

    if (aerdb_exec(db, sql, params, count, NULL, NULL) != AERDB_OK) {
        aer_error(line, "dbrun(): %s", aerdb_error(db));
    }
    return aer_int(db->changes);
}

// what dbquery gives aerdb_exec for each row: add the row, as a list, to the list of rows
static bool add_row (const db_value_t* values, int count, void* ctx) {
    aer_object_t* rows = ctx;
    value_t row = aer_list((uint32_t)count);
    for (int c = 0; c < count; c++) {
        aer_list_push(row.as.obj, from_database(&values[c]));
    }
    aer_list_push(rows, row);
    return false; // carry on with the next row
}

// dbquery(db, sql, values): run a SELECT. Gives back its rows: a list, with each row a list of
// its values (in the order the SELECT named the columns). ? works as in dbrun
value_t bi_dbquery (int argc, value_t* argv, int line) {
    aerdb_t* db = database_arg(argv, 0, line, "dbquery");
    const char* sql = str_arg(argv, 1, line, "dbquery")->data.chars;
    db_value_t params[AERDB_COLUMNS_MAX];
    int count = params_arg(argc, argv, 2, line, "dbquery", params);

    value_t rows = aer_list(0);
    if (aerdb_exec(db, sql, params, count, add_row, rows.as.obj) != AERDB_OK) {
        aer_release(rows);
        aer_error(line, "dbquery(): %s", aerdb_error(db));
    }
    return rows;
}

// dbclose(db): close a database
value_t bi_dbclose (int argc, value_t* argv, int line) {
    (void)argc;
    aerdb_t* db = database_arg(argv, 0, line, "dbclose");
    int32_t number = int_arg(argv, 0, line, "dbclose"); // database_arg checked it is open
    aerdb_close(db);
    open_databases[number - 1] = NULL;
    return aer_nil();
}

// close every database a program left open (aer_builtins_finish calls this when it ends)
void database_finish (void) {
    for (int slot = 0; slot < DATABASES_MAX; slot++) {
        if (open_databases[slot] != NULL) {
            aerdb_close(open_databases[slot]);
            open_databases[slot] = NULL;
        }
    }
}
