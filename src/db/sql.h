/*
    sql.h - how AerDB reads SQL: words first, then a statement_t saying what to do

    sql.c does it in two steps:

        "SELECT name FROM items WHERE price < ?"
            -> sql_words ->  [SELECT] [name] [FROM] [items] [WHERE] [price] [<] [?]
            -> sql_parse ->  a statement_t: kind S_SELECT, table "items", columns ["name"],
                             where: price < (the first parameter)

    and run.c does what the statement_t says. The SQL AerDB understands:

        CREATE TABLE name (column type, column type, ...)
        DROP TABLE name
        INSERT INTO name [(column, ...)] VALUES (value, ...)
        SELECT * | column, ... FROM name [WHERE ...] [LIMIT n]
        UPDATE name SET column = value, ... [WHERE ...]
        DELETE FROM name [WHERE ...]
        COMPACT   (VACUUM does the same)

    A WHERE is conditions joined by AND, each "column op value" with op one of = != <> < <= > >=,
    or "column IS NULL" / "column IS NOT NULL". A value is a number (42, -1, 2.5), 'text' (with ''
    for a ' inside it), NULL, or ? for the next of the parameters given to aerdb_exec.
*/

#ifndef AERDB_SQL_H
#define AERDB_SQL_H

#include "aerdb.h"

#define SQL_WORDS_MAX      128   // words in one statement
#define SQL_WORD_MAX       128   // the longest word, counting its '\0' ('text' longer than this: use ?)
#define SQL_CONDITIONS_MAX 8     // conditions in one WHERE

// ---- words ------------------------------------------------------------------------------------

typedef enum {
    W_NAME,     // a keyword or a name: SELECT, items, price
    W_NUMBER,   // 42, 2.5
    W_TEXT,     // 'tea' (the text without its quotes)
    W_SYMBOL,   // ( ) , * = != <> < <= > >= - ;
    W_PARAM,    // ?
    W_END       // the end of the SQL
} word_kind_t;

typedef struct {
    word_kind_t kind;
    char text[SQL_WORD_MAX];
} word_t;

// Split SQL into words (room for `max`). Gives back how many, the last always W_END, or a
// negative AERDB_ERR_ code with the reason in db's error
int sql_words (aerdb_t* db, const char* sql, word_t* words, int max);

// ---- statements ---------------------------------------------------------------------------------

typedef enum { S_CREATE, S_DROP, S_INSERT, S_SELECT, S_UPDATE, S_DELETE, S_COMPACT } statement_kind_t;

// one condition of a WHERE: column op value
typedef struct {
    char column[AERDB_NAME_MAX];
    char op[4];               // "=", "!=", "<", "<=", ">", ">=", "IS" (IS NULL), "ISN" (IS NOT NULL)
    db_value_t value;
} condition_t;

typedef struct {
    statement_kind_t kind;
    char table[AERDB_NAME_MAX];

    // SELECT: the columns to give back (none means *). INSERT: the columns the values go in
    // (none means all of them, in order). UPDATE: the columns SET changes
    char columns[AERDB_COLUMNS_MAX][AERDB_NAME_MAX];
    int column_count;

    // INSERT: the values. UPDATE: the new value for each of columns[]
    db_value_t values[AERDB_COLUMNS_MAX];
    int value_count;

    condition_t where[SQL_CONDITIONS_MAX];   // WHERE ... AND ... (none: every row)
    int where_count;

    int limit;                 // SELECT's LIMIT, -1 for none
    char create[128];          // CREATE TABLE: the columns, as text ("name TEXT, price REAL")
} statement_t;

// Make sense of a statement's words, filling in `out`. A ? takes the next of params[]. Text values
// point into the words (and params), so those must stay until the statement has been run.
// Gives back AERDB_OK, or an AERDB_ERR_ code with the reason in db's error
int sql_parse (aerdb_t* db, const word_t* words, int count, const db_value_t* params, int param_count,
               statement_t* out);

#endif /* AERDB_SQL_H */
