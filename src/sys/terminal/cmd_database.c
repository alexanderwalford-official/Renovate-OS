/*
    cmd_database.c - the terminal's SQL command, for AerDB databases (db/aerdb.h)

        SQL <FILE> <STATEMENT>   run one statement and show what it gave back:
                                     sql shop.db SELECT * FROM items
        SQL <FILE>               a prompt for running statement after statement, until an empty
                                 line (or EXIT)

    The database is made if the file isn't there. A SELECT's rows are shown as a table, with the
    columns' names on top:

        name | price
        tea  | 2.5
        cake | 4.0
        (2 rows)

    AerLang programs use the same databases with dbopen, dbrun and dbquery (lang/bi_database.c).
*/

#include "commands.h"
#include <stdint.h>
#include "../../lib/primary_definitions.h"
#include "../../lib/aer_math.h"
#include "../../drivers/input/IO.h"
#include "../../drivers/video/VGA_linear.h"
#include "../f_sys.h"
#include "../../db/aerdb.h"

#define SQL_LINE_MAX 256   // the longest statement typed at the sql> prompt

// what the rows of one SELECT are printed with: how many so far, so the heading goes first
typedef struct {
    aerdb_t* db;
    int rows;
} printing_t;

// print one value the way a person would write it: 42, 2.5, tea, NULL, or <12 bytes>
static void print_value (const db_value_t* v) {
    if (v->type == DB_INT) {
        prtf("%d", (int)v->i);
    } else if (v->type == DB_REAL) {
        char text[32];
        format_real(v->r, text, sizeof(text));
        prtf("%s", text);
    } else if (v->type == DB_TEXT) {
        prtf("%.*s", (int)v->length, (const char*)v->bytes);
    } else if (v->type == DB_BLOB) {
        prtf("<%u bytes>", (unsigned int)v->length);
    } else {
        prtf("NULL");
    }
}

// what aerdb_exec gives each row of a SELECT: print it, with the columns' names first
static bool print_row (const db_value_t* values, int count, void* ctx) {
    printing_t* printing = ctx;
    if (printing->rows == 0) {
        colour(LIGHT_CYAN, BLACK);
        for (int c = 0; c < printing->db->result_count; c++) {
            prtf(c == 0 ? "%s" : " | %s", printing->db->result_names[c]);
        }
        prtf("\n");
        colour(BRIGHT, BLACK);
    }
    for (int c = 0; c < count; c++) {
        if (c > 0) {
            prtf(" | ");
        }
        print_value(&values[c]);
    }
    prtf("\n");
    printing->rows++;
    return false; // carry on with the next row
}

// run one statement and say how it went: the rows of a SELECT, how many rows changed, or the error
static void run_one (aerdb_t* db, const char* sql) {
    printing_t printing = { db, 0 };
    int result = aerdb_exec(db, sql, NULL, 0, print_row, &printing);
    if (result != AERDB_OK) {
        colour(LIGHT_RED, BLACK);
        prtf("Error: %s\n", aerdb_error(db));
        colour(BRIGHT, BLACK);
        return;
    }
    // a SELECT says how many rows it found; the others how many they changed
    if (db->result_count > 0) {
        prtf("(%d row%s)\n", printing.rows, printing.rows == 1 ? "" : "s");
    } else if (db->changes > 0) {
        prtf("%d row(s) changed\n", db->changes);
    } else {
        prtf("Done.\n");
    }
}

// SQL <FILE> [STATEMENT]: run SQL on an AerDB database: one statement, or a prompt for many
void cli_sql (char* arguments) {
    // the file is the first word, the statement (if any) the rest
    char* statement;
    char* file = split_first_word(arguments, &statement);
    if (file[0] == '\0') {
        show_usage("e.g. SQL shop.db SELECT * FROM items, or SQL shop.db for a prompt");
        return;
    }
    char folder[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        prtf("%s isn't a file name.\n", file);
        return;
    }
    int result;
    aerdb_t* db = aerdb_open(folder, name, &result);
    if (db == NULL) {
        prtf("Can't open %s: %s.\n", file, aerdb_strerror(result));
        return;
    }

    if (statement[0] != '\0') {
        run_one(db, statement);
    } else {
        // the prompt: a statement a line, until an empty line or EXIT
        prtf("SQL on %s. An empty line (or EXIT) leaves.\n", name);
        char line[SQL_LINE_MAX];
        while (true) {
            colour(DARK_GRAY, BLACK);
            prtf("sql> ");
            colour(BRIGHT, BLACK);
            GetLine(line, sizeof(line), NULL);
            char* sql = trim(line);
            if (sql[0] == '\0' || strcasecmp(sql, "exit") == 0) {
                break;
            }
            run_one(db, sql);
        }
    }
    aerdb_close(db);
}
