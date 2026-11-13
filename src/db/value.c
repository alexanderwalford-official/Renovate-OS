/*
    value.c - AerDB's values, and turning a row into bytes and back

    A row's values are stored one after another, each as a type byte and then what that type
    needs (see the top of aerdb.h):

        DB_NULL   [0]
        DB_INT    [1] [4 bytes]
        DB_REAL   [2] [8 bytes]
        DB_TEXT   [3] [length: 2 bytes] [the characters]
        DB_BLOB   [4] [length: 2 bytes] [the bytes]

    row_encode writes values that way, and row_decode reads them back. Neither copies text or
    blobs: decoding gives values whose `bytes` point into the row it was given.
*/

#include "aerdb.h"
#include "../lib/aer_string.h"

// a NULL value (handy for filling in a column that was left out)
db_value_t value_null (void) {
    db_value_t v;
    memset(&v, 0, sizeof(v));
    v.type = DB_NULL;
    return v;
}

// how many bytes a value takes in a row, its type byte included
static uint16_t value_size (const db_value_t* v) {
    switch (v->type) {
        case DB_INT:  return 1 + 4;
        case DB_REAL: return 1 + 8;
        case DB_TEXT:
        case DB_BLOB: return 1 + 2 + v->length;
        default:      return 1; // DB_NULL: the type byte alone
    }
}

// Write `count` values as a row's bytes into out, which has `room` bytes. Gives back how many
// bytes it took, or 0 if they don't fit
uint16_t row_encode (const db_value_t* values, int count, uint8_t* out, uint16_t room) {
    // first check it all fits, so nothing is half written
    uint32_t total = 0;
    for (int i = 0; i < count; i++) {
        total += value_size(&values[i]);
    }
    if (total > room) {
        return 0;
    }

    // then each value: its type byte, then its bytes
    uint8_t* at = out;
    for (int i = 0; i < count; i++) {
        const db_value_t* v = &values[i];
        *at = (uint8_t)v->type;
        at++;
        if (v->type == DB_INT) {
            memcpy(at, &v->i, 4);
            at += 4;
        } else if (v->type == DB_REAL) {
            memcpy(at, &v->r, 8);
            at += 8;
        } else if (v->type == DB_TEXT || v->type == DB_BLOB) {
            memcpy(at, &v->length, 2);
            memcpy(at + 2, v->bytes, v->length);
            at += 2 + v->length;
        }
    }
    return (uint16_t)total;
}

// Read a row's values from its `length` bytes into values[] (room for `max`). Gives back how
// many values there were. A row that is cut short (a damaged file) stops at what is whole
int row_decode (const uint8_t* bytes, uint16_t length, db_value_t* values, int max) {
    const uint8_t* at = bytes;
    const uint8_t* end = bytes + length;
    int count = 0;

    while (at < end && count < max) {
        db_value_t v = value_null();
        v.type = (db_type_t)*at;
        at++;

        // how many bytes this value's data takes, and whether they are all there
        uint32_t need = 0;
        if (v.type == DB_INT) {
            need = 4;
        } else if (v.type == DB_REAL) {
            need = 8;
        } else if (v.type == DB_TEXT || v.type == DB_BLOB) {
            if (at + 2 > end) {
                break;
            }
            memcpy(&v.length, at, 2);
            at += 2;
            need = v.length;
        } else if (v.type != DB_NULL) {
            break; // a type byte AerDB doesn't know: a damaged row
        }
        if (at + need > end) {
            break;
        }

        // the data itself
        if (v.type == DB_INT) {
            memcpy(&v.i, at, 4);
        } else if (v.type == DB_REAL) {
            memcpy(&v.r, at, 8);
        } else if (v.type == DB_TEXT || v.type == DB_BLOB) {
            v.bytes = at; // not a copy: it points into the row
        }
        at += need;
        values[count] = v;
        count++;
    }
    return count;
}

// a value's place in the order SQL sorts types in: NULL first, then numbers, then text, then blobs
static int type_order (db_type_t type) {
    switch (type) {
        case DB_NULL: return 0;
        case DB_INT:
        case DB_REAL: return 1; // whole numbers and decimals compare with each other
        case DB_TEXT: return 2;
        default:      return 3; // DB_BLOB
    }
}

// the value of a number, as a decimal (so 2 and 2.0 compare as equal)
static double as_real (const db_value_t* v) {
    return v->type == DB_INT ? (double)v->i : v->r;
}

// Compare two values: -1 if a comes first, 0 if they are the same, 1 if b comes first. Values
// of different kinds go in type_order; numbers by size; text and blobs byte by byte, a shorter
// one first if it is the start of the longer ("tea" before "teapot")
int value_compare (const db_value_t* a, const db_value_t* b) {
    int order_a = type_order(a->type);
    int order_b = type_order(b->type);
    if (order_a != order_b) {
        return order_a < order_b ? -1 : 1;
    }

    if (order_a == 0) {
        return 0; // two NULLs
    }
    if (order_a == 1) {
        double x = as_real(a);
        double y = as_real(b);
        if (x < y) {
            return -1;
        }
        return x > y ? 1 : 0;
    }

    // text or blobs: the bytes they have in common first, then the length
    uint16_t shorter = a->length < b->length ? a->length : b->length;
    int bytes = memcmp(a->bytes, b->bytes, shorter);
    if (bytes != 0) {
        return bytes < 0 ? -1 : 1;
    }
    if (a->length == b->length) {
        return 0;
    }
    return a->length < b->length ? -1 : 1;
}
