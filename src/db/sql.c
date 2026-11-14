/*
    sql.c - splitting SQL into words, and making sense of them (see sql.h)

    sql_words goes through the text a character at a time: letters start a name, digits a
    number, a ' starts text (up to the next ', with '' meaning a ' inside it), and anything else
    is a symbol. Spaces only separate words.

    sql_parse then reads the words in order, the way people read SQL: the first word says which
    kind of statement it is (CREATE, INSERT, SELECT...), and one function for each kind reads the
    rest. They all use the same few helpers on a `parser_t`, which remembers where it is:

        at_keyword(p, "FROM")    is the next word FROM?             (doesn't move on)
        take_keyword(p, "FROM")  if the next word is FROM, move past it
        need_keyword(p, "FROM")  the next word must be FROM: move past it, or it's an error
        read_name(p, out)        the next word is a name: copy it to out
        read_value(p, out)       the next word(s) are a value: 42, -2.5, 'tea', NULL or ?
*/

#include "sql.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_ctype.h"
#include "../lib/aer_math.h"
#include "../lib/primary_definitions.h"

// ================================================================================================
//  Words
// ================================================================================================

// can this character be part of a name? (letters, digits and _)
static bool name_character (char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// the symbols AerDB knows, longest first, so "<=" is found before "<"
static const char* const symbols[] = {
    "!=", "<>", "<=", ">=", "(", ")", ",", "*", "=", "<", ">", "-", ";", NULL
};

// split SQL into words (see sql.h)
int sql_words (aerdb_t* db, const char* sql, word_t* words, int max) {
    int count = 0;
    const char* at = sql;

    while (true) {
        // spaces (and new lines) only separate words
        while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
            at++;
        }
        if (count == max - 1) {
            return aerdb_fail(db, AERDB_ERR_TOO_BIG, "the SQL is too long (at most %d words)", max - 1);
        }
        word_t* word = &words[count];
        memset(word, 0, sizeof(word_t));
        int length = 0;

        if (*at == '\0') {
            // the end: always the last word, so the parser never runs off the end of the list
            word->kind = W_END;
            return count + 1;
        }

        if (isalpha((unsigned char)*at) || *at == '_') {
            // a name or a keyword: letters, digits and _
            word->kind = W_NAME;
            while (name_character(*at) && length < SQL_WORD_MAX - 1) {
                word->text[length++] = *at++;
            }
        } else if (isdigit((unsigned char)*at) || (*at == '.' && isdigit((unsigned char)at[1]))) {
            // a number: digits, maybe a decimal point and more digits (2, 2.5, .5)
            word->kind = W_NUMBER;
            while ((isdigit((unsigned char)*at) || *at == '.') && length < SQL_WORD_MAX - 1) {
                word->text[length++] = *at++;
            }
        } else if (*at == '\'') {
            // text: up to the closing ', where '' is a ' inside the text ('it''s' is it's)
            word->kind = W_TEXT;
            at++;
            while (true) {
                if (*at == '\0') {
                    return aerdb_fail(db, AERDB_ERR_SYNTAX, "a ' was opened but never closed");
                }
                if (*at == '\'' && at[1] == '\'') {
                    at += 2;
                    word->text[length++] = '\'';
                } else if (*at == '\'') {
                    at++;
                    break;
                } else {
                    word->text[length++] = *at++;
                }
                if (length == SQL_WORD_MAX - 1) {
                    return aerdb_fail(db, AERDB_ERR_TOO_BIG, "text in the SQL can be at most %d characters "
                                      "(give longer text as a ? value)", SQL_WORD_MAX - 1);
                }
            }
        } else if (*at == '?') {
            word->kind = W_PARAM;
            word->text[0] = '?';
            at++;
        } else {
            // a symbol: the first in the list that the SQL starts with here
            int s = 0;
            while (symbols[s] != NULL && strncmp(at, symbols[s], slen(symbols[s])) != 0) {
                s++;
            }
            if (symbols[s] == NULL) {
                return aerdb_fail(db, AERDB_ERR_SYNTAX, "AerDB doesn't know the symbol %c", *at);
            }
            word->kind = W_SYMBOL;
            scpy(word->text, symbols[s], sizeof(word->text));
            at += slen(symbols[s]);
        }
        count++;
    }
}

// ================================================================================================
//  The parser's helpers
// ================================================================================================

// where the parser is in the words, and what it needs while it reads them
typedef struct {
    aerdb_t* db;                 // for error messages
    const word_t* words;
    int at;                      // the next word to read
    const db_value_t* params;    // the ? values
    int param_count;
    int next_param;              // the next ? takes params[next_param]
} parser_t;

// the next word, without moving past it
static const word_t* peek (const parser_t* p) {
    return &p->words[p->at];
}

// is the next word this keyword (or symbol)? Keywords ignore upper and lower case
static bool at_keyword (const parser_t* p, const char* keyword) {
    const word_t* word = peek(p);
    if (word->kind == W_NAME) {
        return strcasecmp(word->text, keyword) == 0;
    }
    return word->kind == W_SYMBOL && seq(word->text, keyword);
}

// if the next word is this keyword, move past it and give back true
static bool take_keyword (parser_t* p, const char* keyword) {
    if (at_keyword(p, keyword)) {
        p->at++;
        return true;
    }
    return false;
}

// the next word must be this keyword: move past it, or say what was expected instead
static int need_keyword (parser_t* p, const char* keyword) {
    if (take_keyword(p, keyword)) {
        return AERDB_OK;
    }
    const word_t* word = peek(p);
    return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "expected %s, but found %s", keyword,
                      word->kind == W_END ? "the end" : word->text);
}

// the next word must be a name (of a table or a column): copy it to out
static int read_name (parser_t* p, char out[AERDB_NAME_MAX]) {
    const word_t* word = peek(p);
    if (word->kind != W_NAME) {
        return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "expected a name, but found %s",
                          word->kind == W_END ? "the end" : word->text);
    }
    if (slen(word->text) >= AERDB_NAME_MAX) {
        return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "the name %s is too long (at most %d letters)",
                          word->text, AERDB_NAME_MAX - 1);
    }
    scpy(out, word->text, AERDB_NAME_MAX);
    p->at++;
    return AERDB_OK;
}

// The next word(s) must be a value: a number (42, 2.5, with a - in front for a negative one),
// 'text', NULL, or ? (the next parameter). Text points into the word, not a copy
static int read_value (parser_t* p, db_value_t* out) {
    *out = value_null();
    bool negative = take_keyword(p, "-");
    const word_t* word = peek(p);

    if (word->kind == W_NUMBER) {
        // a number with a decimal point is a REAL, one without an INT
        if (strchr(word->text, '.') != NULL) {
            double r;
            const char* end;   // where the number stopped: it must be the whole word ("1.2.3" isn't)
            if (!parse_real(word->text, &r, &end) || *end != '\0') {
                return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "%s isn't a number", word->text);
            }
            out->type = DB_REAL;
            out->r = negative ? -r : r;
        } else {
            long i = strtol(word->text, NULL, 10);
            out->type = DB_INT;
            out->i = (int32_t)(negative ? -i : i);
        }
    } else if (negative) {
        return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "expected a number after -");
    } else if (word->kind == W_TEXT) {
        out->type = DB_TEXT;
        out->bytes = (const uint8_t*)word->text;
        out->length = (uint16_t)slen(word->text);
    } else if (word->kind == W_NAME && strcasecmp(word->text, "NULL") == 0) {
        out->type = DB_NULL;
    } else if (word->kind == W_PARAM) {
        if (p->next_param >= p->param_count) {
            return aerdb_fail(p->db, AERDB_ERR_VALUES, "there are more ?s in the SQL than values given for them");
        }
        *out = p->params[p->next_param];
        p->next_param++;
    } else {
        return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "expected a value, but found %s",
                          word->kind == W_END ? "the end" : word->text);
    }
    p->at++;
    return AERDB_OK;
}

// read a list of names, separated by commas, into names[] (room for AERDB_COLUMNS_MAX). Gives
// back how many, or a negative AERDB_ERR_ code
static int read_name_list (parser_t* p, char names[][AERDB_NAME_MAX]) {
    int count = 0;
    do {
        if (count == AERDB_COLUMNS_MAX) {
            return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "at most %d columns", AERDB_COLUMNS_MAX);
        }
        int result = read_name(p, names[count]);
        if (result != AERDB_OK) {
            return result;
        }
        count++;
    } while (take_keyword(p, ","));
    return count;
}

// read a list of values, separated by commas, into values[] (room for AERDB_COLUMNS_MAX). Gives
// back how many, or a negative AERDB_ERR_ code
static int read_value_list (parser_t* p, db_value_t* values) {
    int count = 0;
    do {
        if (count == AERDB_COLUMNS_MAX) {
            return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "at most %d values", AERDB_COLUMNS_MAX);
        }
        int result = read_value(p, &values[count]);
        if (result != AERDB_OK) {
            return result;
        }
        count++;
    } while (take_keyword(p, ","));
    return count;
}

// WHERE condition AND condition ..., if there is one. Each condition is "column op value", or
// "column IS NULL" / "column IS NOT NULL"
static int read_where (parser_t* p, statement_t* s) {
    if (!take_keyword(p, "WHERE")) {
        return AERDB_OK; // no WHERE: every row
    }
    do {
        if (s->where_count == SQL_CONDITIONS_MAX) {
            return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "at most %d conditions in a WHERE", SQL_CONDITIONS_MAX);
        }
        condition_t* c = &s->where[s->where_count];
        int result = read_name(p, c->column);
        if (result != AERDB_OK) {
            return result;
        }

        if (take_keyword(p, "IS")) {
            // IS NULL, or IS NOT NULL
            scpy(c->op, take_keyword(p, "NOT") ? "ISN" : "IS", sizeof(c->op));
            result = need_keyword(p, "NULL");
            c->value = value_null();
        } else {
            // one of the comparison symbols, then a value
            const word_t* op = peek(p);
            const char* known[] = { "=", "!=", "<>", "<", "<=", ">", ">=", NULL };
            int k = 0;
            while (known[k] != NULL && !(op->kind == W_SYMBOL && seq(op->text, known[k]))) {
                k++;
            }
            if (known[k] == NULL) {
                return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "expected = != < <= > >= or IS after %s", c->column);
            }
            // <> means the same as !=
            scpy(c->op, seq(known[k], "<>") ? "!=" : known[k], sizeof(c->op));
            p->at++;
            result = read_value(p, &c->value);
        }
        if (result != AERDB_OK) {
            return result;
        }
        s->where_count++;
    } while (take_keyword(p, "AND"));

    if (at_keyword(p, "OR")) {
        return aerdb_fail(p->db, AERDB_ERR_UNSUPPORTED, "AerDB can't do OR in a WHERE yet: only AND");
    }
    return AERDB_OK;
}

// ================================================================================================
//  One function for each kind of statement
// ================================================================================================

// CREATE TABLE [IF NOT EXISTS] name (column type, column type, ...)
// The columns are kept as text ("name TEXT, price REAL"): the words between the brackets,
// joined with spaces, and ", " for each comma. IF NOT EXISTS makes a table that is already
// there fine, rather than an error: how a program makes its table the first time it runs
static int parse_create (parser_t* p, statement_t* s) {
    s->kind = S_CREATE;
    int result = need_keyword(p, "TABLE");
    if (result == AERDB_OK && take_keyword(p, "IF")) {
        result = need_keyword(p, "NOT");
        if (result == AERDB_OK) {
            result = need_keyword(p, "EXISTS");
        }
        s->if_not_exists = true;
    }
    if (result == AERDB_OK) {
        result = read_name(p, s->table);
    }
    if (result == AERDB_OK) {
        result = need_keyword(p, "(");
    }
    if (result != AERDB_OK) {
        return result;
    }

    int columns = 0;
    bool start_of_column = true;
    while (!at_keyword(p, ")")) {
        const word_t* word = peek(p);
        if (word->kind == W_END) {
            return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "CREATE TABLE's ( was never closed");
        }
        if (seq(word->text, ",")) {
            scat(s->create, ", ", sizeof(s->create));
            start_of_column = true;
        } else {
            // a space between words, except straight after a comma or at the very start
            if (!start_of_column) {
                scat(s->create, " ", sizeof(s->create));
            } else {
                columns++;
            }
            scat(s->create, word->text, sizeof(s->create));
            start_of_column = false;
        }
        p->at++;
    }
    p->at++; // the )

    if (columns == 0) {
        return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "a table needs at least one column");
    }
    if (columns > AERDB_COLUMNS_MAX) {
        return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "a table can have at most %d columns", AERDB_COLUMNS_MAX);
    }
    if (slen(s->create) >= sizeof(s->create) - 1) {
        return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "the columns are too long to describe (at most %d characters)",
                          (int)sizeof(s->create) - 2);
    }
    return AERDB_OK;
}

// DROP TABLE name
static int parse_drop (parser_t* p, statement_t* s) {
    s->kind = S_DROP;
    int result = need_keyword(p, "TABLE");
    if (result == AERDB_OK) {
        result = read_name(p, s->table);
    }
    return result;
}

// INSERT INTO name [(column, ...)] VALUES (value, ...)
static int parse_insert (parser_t* p, statement_t* s) {
    s->kind = S_INSERT;
    int result = need_keyword(p, "INTO");
    if (result == AERDB_OK) {
        result = read_name(p, s->table);
    }
    if (result != AERDB_OK) {
        return result;
    }

    // the columns the values go in, if they are named
    if (take_keyword(p, "(")) {
        s->column_count = read_name_list(p, s->columns);
        if (s->column_count < 0) {
            return s->column_count;
        }
        result = need_keyword(p, ")");
        if (result != AERDB_OK) {
            return result;
        }
    }

    result = need_keyword(p, "VALUES");
    if (result == AERDB_OK) {
        result = need_keyword(p, "(");
    }
    if (result != AERDB_OK) {
        return result;
    }
    s->value_count = read_value_list(p, s->values);
    if (s->value_count < 0) {
        return s->value_count;
    }
    return need_keyword(p, ")");
}

// SELECT * | column, ... FROM name [WHERE ...] [ORDER BY column [ASC | DESC]] [LIMIT n]
static int parse_select (parser_t* p, statement_t* s) {
    s->kind = S_SELECT;
    s->limit = -1;

    // * is every column, in order (column_count stays 0)
    if (!take_keyword(p, "*")) {
        s->column_count = read_name_list(p, s->columns);
        if (s->column_count < 0) {
            return s->column_count;
        }
    }

    int result = need_keyword(p, "FROM");
    if (result == AERDB_OK) {
        result = read_name(p, s->table);
    }
    if (result == AERDB_OK) {
        result = read_where(p, s);
    }
    if (result != AERDB_OK) {
        return result;
    }

    // ORDER BY column, then maybe ASC or DESC (ORDER BY comes before LIMIT, as in SQL)
    if (take_keyword(p, "ORDER")) {
        result = need_keyword(p, "BY");
        if (result == AERDB_OK) {
            result = read_name(p, s->order_by);
        }
        if (result != AERDB_OK) {
            return result;
        }

        if (take_keyword(p, "DESC")) {
            s->order_desc = true;     // biggest first
        } else {
            take_keyword(p, "ASC");   // smallest first: the usual way, so ASC changes nothing
        }
    }
    if (take_keyword(p, "LIMIT")) {
        db_value_t limit;
        result = read_value(p, &limit);
        if (result != AERDB_OK) {
            return result;
        }
        if (limit.type != DB_INT || limit.i < 0) {
            return aerdb_fail(p->db, AERDB_ERR_SYNTAX, "LIMIT needs a whole number of 0 or more");
        }
        s->limit = limit.i;
    }
    return AERDB_OK;
}

// UPDATE name SET column = value, ... [WHERE ...]
static int parse_update (parser_t* p, statement_t* s) {
    s->kind = S_UPDATE;
    int result = read_name(p, s->table);
    if (result == AERDB_OK) {
        result = need_keyword(p, "SET");
    }
    if (result != AERDB_OK) {
        return result;
    }

    // column = value, column = value...: the columns in columns[], their new values in values[]
    do {
        if (s->column_count == AERDB_COLUMNS_MAX) {
            return aerdb_fail(p->db, AERDB_ERR_TOO_BIG, "at most %d columns", AERDB_COLUMNS_MAX);
        }
        result = read_name(p, s->columns[s->column_count]);
        if (result == AERDB_OK) {
            result = need_keyword(p, "=");
        }
        if (result == AERDB_OK) {
            result = read_value(p, &s->values[s->column_count]);
        }
        if (result != AERDB_OK) {
            return result;
        }
        s->column_count++;
    } while (take_keyword(p, ","));
    s->value_count = s->column_count;

    return read_where(p, s);
}

// DELETE FROM name [WHERE ...]
static int parse_delete (parser_t* p, statement_t* s) {
    s->kind = S_DELETE;
    int result = need_keyword(p, "FROM");
    if (result == AERDB_OK) {
        result = read_name(p, s->table);
    }
    if (result == AERDB_OK) {
        result = read_where(p, s);
    }
    return result;
}

// make sense of a statement's words (see sql.h)
int sql_parse (aerdb_t* db, const word_t* words, int count, const db_value_t* params, int param_count, statement_t* out) {
    (void)count; // the words always end with W_END, which is what the parser looks for
    memset(out, 0, sizeof(statement_t));
    out->limit = -1;
    parser_t parser = { db, words, 0, params, param_count, 0 };
    parser_t* p = &parser;

    // the first word says which kind of statement it is
    int result;
    if (take_keyword(p, "CREATE")) {
        result = parse_create(p, out);
    } else if (take_keyword(p, "DROP")) {
        result = parse_drop(p, out);
    } else if (take_keyword(p, "INSERT")) {
        result = parse_insert(p, out);
    } else if (take_keyword(p, "SELECT")) {
        result = parse_select(p, out);
    } else if (take_keyword(p, "UPDATE")) {
        result = parse_update(p, out);
    } else if (take_keyword(p, "DELETE")) {
        result = parse_delete(p, out);
    } else if (take_keyword(p, "COMPACT") || take_keyword(p, "VACUUM")) {
        out->kind = S_COMPACT;
        result = AERDB_OK;
    } else {
        const word_t* first = peek(p);
        if (first->kind == W_END) {
            return aerdb_fail(db, AERDB_ERR_SYNTAX, "there is no SQL to run");
        }
        return aerdb_fail(db, AERDB_ERR_UNSUPPORTED, "AerDB doesn't know the statement %s", first->text);
    }
    if (result != AERDB_OK) {
        return result;
    }

    // nothing may be left over but an optional ; at the end
    take_keyword(p, ";");
    if (peek(p)->kind != W_END) {
        return aerdb_fail(db, AERDB_ERR_SYNTAX, "didn't expect %s there", peek(p)->text);
    }
    if (p->next_param != param_count) {
        return aerdb_fail(db, AERDB_ERR_VALUES, "%d value(s) were given for %d ?(s) in the SQL",
                          param_count, p->next_param);
    }
    return AERDB_OK;
}
