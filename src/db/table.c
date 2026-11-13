/*
    table.c - AerDB's tables: making and finding them, and adding, reading and deleting rows

    A table is listed on page 1 (aerdb_table_t: its name, its columns, and where its pages are),
    and its rows are on a chain of pages. Each page starts with an aerdb_page_header_t, then its
    rows one after another:

        [ page header: next page, bytes used, rows ][ row ][ row ][ row ] ...   (unused space)

    and each row is

        [ length: 2 bytes ][ flags: 1 byte ][ rowid: 4 bytes ][ its values (see value.c) ]

    Adding a row puts it at the end of the table's last page, or starts a new page when it doesn't
    fit there. Deleting a row sets bit 0 of its flags: it stays where it is, skipped by every scan,
    until aerdb_compact (run.c) gives its space back.
*/

#include "aerdb.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../sys/trace.h"

#define ROW_DELETED 0x01   // bit 0 of a row's flags

// ---- tables -----------------------------------------------------------------------------------

// the table called `name` (ignoring upper and lower case, like AerOS's file names), NULL if
// there isn't one
aerdb_table_t* table_find (aerdb_t* db, const char* name) {
    for (uint32_t i = 0; i < db->header.table_count; i++) {
        if (strcasecmp(db->header.tables[i].name, name) == 0) {
            return &db->header.tables[i];
        }
    }
    return NULL;
}

// Make a new, empty table. `columns` is what CREATE TABLE had between its brackets, e.g.
// "name TEXT, price REAL". It has no pages until its first row arrives
int table_create (aerdb_t* db, const char* name, const char* columns) {
    if (table_find(db, name) != NULL) {
        return aerdb_fail(db, AERDB_ERR_EXISTS, "there is a table called %s already", name);
    }
    if (db->header.table_count == AERDB_TABLES_MAX) {
        return aerdb_fail(db, AERDB_ERR_FULL, "a database can have at most %d tables", AERDB_TABLES_MAX);
    }

    aerdb_table_t* table = &db->header.tables[db->header.table_count];
    memset(table, 0, sizeof(aerdb_table_t));
    scpy(table->name, name, sizeof(table->name));
    scpy(table->columns, columns, sizeof(table->columns));
    table->next_rowid = 1;
    db->header.table_count++;

    if (!header_save(db)) {
        return aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");
    }
    TRACE("made the table %s (%s), with no pages yet", name, columns);
    return AERDB_OK;
}

// Remove a table from the list. Its pages stay in the file, belonging to nothing, until
// aerdb_compact gives them back
int table_drop (aerdb_t* db, const char* name) {
    aerdb_table_t* table = table_find(db, name);
    if (table == NULL) {
        return aerdb_fail(db, AERDB_ERR_NOT_FOUND, "no such table: %s", name);
    }
    // close the gap in the list: every table after it moves down one place
    int index = (int)(table - db->header.tables);
    int after = (int)db->header.table_count - index - 1;
    memmove(table, table + 1, (size_t)after * sizeof(aerdb_table_t));
    db->header.table_count--;

    if (!header_save(db)) {
        return aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");
    }
    TRACE("dropped the table %s (its pages are reused by COMPACT)", name);
    return AERDB_OK;
}

// The names of a table's columns, from its columns text: "name TEXT, price REAL" gives "name"
// and "price" (the first word of each part between the commas). Gives back how many
int table_columns (const aerdb_table_t* table, char names[][AERDB_NAME_MAX], int max) {
    int count = 0;
    const char* at = table->columns;
    while (*at != '\0' && count < max) {
        // skip the spaces before the name, then copy it up to a space or a comma
        while (*at == ' ') {
            at++;
        }
        int length = 0;
        while (*at != '\0' && *at != ' ' && *at != ',' && length < AERDB_NAME_MAX - 1) {
            names[count][length] = *at;
            length++;
            at++;
        }
        names[count][length] = '\0';
        if (length > 0) {
            count++;
        }
        // then skip the rest of this column (its type) up to the next comma
        while (*at != '\0' && *at != ',') {
            at++;
        }
        if (*at == ',') {
            at++;
        }
    }
    return count;
}

// ---- adding rows ----------------------------------------------------------------------------------

// Put a row (its bytes, made by row_encode, with the row header in front) on the end of a table's
// last page, starting a new page if there is no room. rowid is the row's number
int table_append_row (aerdb_t* db, aerdb_table_t* table, uint32_t rowid, const uint8_t* values, uint16_t size) {
    // the whole row: its header (length, flags, rowid), then its values
    uint16_t row_size = AERDB_ROW_HEADER + size;
    if (row_size > AERDB_PAGE_SIZE - sizeof(aerdb_page_header_t)) {
        return aerdb_fail(db, AERDB_ERR_TOO_BIG, "a row can be at most %u bytes",
                          (unsigned int)(AERDB_PAGE_SIZE - sizeof(aerdb_page_header_t) - AERDB_ROW_HEADER));
    }

    uint8_t page[AERDB_PAGE_SIZE];
    aerdb_page_header_t* page_header = (aerdb_page_header_t*)page;

    // the table's last page, if it has one with room for this row
    bool has_room = false;
    if (table->last_page != 0) {
        if (!page_read(db, table->last_page, page)) {
            return aerdb_fail(db, AERDB_ERR_IO, "couldn't read the database");
        }
        has_room = page_header->used + row_size <= AERDB_PAGE_SIZE;
    }

    // no room (or no pages yet): a new page on the end of the chain
    if (!has_room) {
        uint32_t fresh = page_new(db);
        if (fresh == 0) {
            return aerdb_fail(db, AERDB_ERR_FULL, "couldn't add a page: the disk may be full");
        }
        if (table->last_page != 0) {
            // the old last page now points on to the new one
            page_header->next_page = fresh;
            if (!page_write(db, table->last_page, page)) {
                return aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");
            }
            TRACE("page %u of %s was full: started page %u", (unsigned int)table->last_page,
                  table->name, (unsigned int)fresh);
        } else {
            table->first_page = fresh;
            TRACE("%s's first row: started page %u", table->name, (unsigned int)fresh);
        }
        table->last_page = fresh;

        // the new page starts empty, with only its header used
        memset(page, 0, sizeof(page));
        page_header->used = sizeof(aerdb_page_header_t);
    }

    // the row goes where the used part of the page ends: its header, then its values
    uint8_t* row = page + page_header->used;
    uint8_t flags = 0;
    memcpy(row, &row_size, 2);
    memcpy(row + 2, &flags, 1);
    memcpy(row + 3, &rowid, 4);
    memcpy(row + AERDB_ROW_HEADER, values, size);
    page_header->used += row_size;
    page_header->rows++;

    if (!page_write(db, table->last_page, page)) {
        return aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");
    }
    // the next new row gets a bigger number than this one, whichever way this one got its own
    if (rowid >= table->next_rowid) {
        table->next_rowid = rowid + 1;
    }
    if (!header_save(db)) {
        return aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");
    }
    return AERDB_OK;
}

// add a new row of values to a table: it gets the next rowid
int table_insert (aerdb_t* db, aerdb_table_t* table, const db_value_t* values, int count) {
    uint8_t bytes[AERDB_PAGE_SIZE];
    uint16_t room = AERDB_PAGE_SIZE - sizeof(aerdb_page_header_t) - AERDB_ROW_HEADER;
    uint16_t size = row_encode(values, count, bytes, room);
    if (size == 0 && count > 0) {
        return aerdb_fail(db, AERDB_ERR_TOO_BIG, "the row is too big to fit on a page");
    }
    return table_append_row(db, table, table->next_rowid, bytes, size);
}

// ---- reading and deleting rows ------------------------------------------------------------------

// Go through every row of a table that isn't deleted, in the order they were added, calling fn
// with each. fn gives back true to stop. The values point into a page that is only there until
// fn returns
void table_scan (aerdb_t* db, const aerdb_table_t* table, table_row_fn fn, void* ctx) {
    uint8_t page[AERDB_PAGE_SIZE];

    // follow the chain of pages from the first to the last (whose next_page is 0)
    uint32_t n = table->first_page;
    while (n != 0) {
        if (!page_read(db, n, page)) {
            return;
        }
        aerdb_page_header_t* page_header = (aerdb_page_header_t*)page;

        // each row on the page, one after another
        uint16_t at = sizeof(aerdb_page_header_t);
        for (uint16_t r = 0; r < page_header->rows; r++) {
            uint16_t row_size;
            uint8_t flags;
            uint32_t rowid;
            memcpy(&row_size, page + at, 2);
            memcpy(&flags, page + at + 2, 1);
            memcpy(&rowid, page + at + 3, 4);
            if (row_size < AERDB_ROW_HEADER || at + row_size > AERDB_PAGE_SIZE) {
                return; // a damaged page: stop rather than read nonsense
            }

            if ((flags & ROW_DELETED) == 0) {
                db_value_t values[AERDB_COLUMNS_MAX];
                int count = row_decode(page + at + AERDB_ROW_HEADER, row_size - AERDB_ROW_HEADER,
                                       values, AERDB_COLUMNS_MAX);
                if (fn(rowid, values, count, ctx)) {
                    return;
                }
            }
            at += row_size;
        }
        n = page_header->next_page;
    }
}

// Go through every row of a table, asking should_delete about each, and mark the ones it says
// yes to as deleted. A page is only written back if a row on it changed. Gives back how many
// rows were deleted, or a negative AERDB_ERR_ code
int table_delete_where (aerdb_t* db, const aerdb_table_t* table, table_row_fn should_delete, void* ctx) {
    uint8_t page[AERDB_PAGE_SIZE];
    int deleted = 0;

    uint32_t n = table->first_page;
    while (n != 0) {
        if (!page_read(db, n, page)) {
            return aerdb_fail(db, AERDB_ERR_IO, "couldn't read the database");
        }
        aerdb_page_header_t* page_header = (aerdb_page_header_t*)page;
        bool page_changed = false;

        uint16_t at = sizeof(aerdb_page_header_t);
        for (uint16_t r = 0; r < page_header->rows; r++) {
            uint16_t row_size;
            uint32_t rowid;
            memcpy(&row_size, page + at, 2);
            memcpy(&rowid, page + at + 3, 4);
            if (row_size < AERDB_ROW_HEADER || at + row_size > AERDB_PAGE_SIZE) {
                break; // a damaged page
            }

            uint8_t* flags = page + at + 2;
            if ((*flags & ROW_DELETED) == 0) {
                db_value_t values[AERDB_COLUMNS_MAX];
                int count = row_decode(page + at + AERDB_ROW_HEADER, row_size - AERDB_ROW_HEADER,
                                       values, AERDB_COLUMNS_MAX);
                if (should_delete(rowid, values, count, ctx)) {
                    *flags |= ROW_DELETED;
                    page_changed = true;
                    deleted++;
                }
            }
            at += row_size;
        }

        if (page_changed && !page_write(db, n, page)) {
            return aerdb_fail(db, AERDB_ERR_IO, "couldn't write the database");
        }
        n = page_header->next_page;
    }
    if (deleted > 0) {
        TRACE("marked %d row(s) of %s deleted (COMPACT gives their space back)", deleted, table->name);
    }
    return deleted;
}
