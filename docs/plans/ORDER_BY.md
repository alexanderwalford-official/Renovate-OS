# Guide: ORDER BY for AerDB

> **Where it stands:** built, following the steps below, so this guide is also a walk-through
> of the code. Where each step ended up:
>
> | Step | Code |
> | --- | --- |
> | 1 | `order_by` and `order_desc` in `statement_t` (`src/db/sql.h`) |
> | 2 | the `ORDER BY` part of `parse_select` (`src/db/sql.c`) |
> | 3 | the start of `run_select` (`src/db/run.c`) |
> | 4 | `sort_row_t`, and `keep_row`, which `select_row` calls when there's an ORDER BY |
> | 5 | `comes_after` (which way round) and `sort_kept_rows` |
> | 6 | `give_row` (moved out of `select_row`), `give_kept_rows` and `forget_kept_rows`, called at the end of `run_select` |
> | 8 | `best_scores()` in `src/disk/bin/games/snake.aer` |
>
> In run.c they're together under "ORDER BY", in the order they run.

`ORDER BY` sorts the rows a SELECT gives back:

```
SELECT name, score FROM scores WHERE game = 'snake' ORDER BY score DESC LIMIT 5
```

gives the five best Snake scores, best first. Without it, rows come back in the order they were
added, and every program has to sort them itself (Snake's `best_scores()` does).

What it should do:

| SQL | Gives back |
| --- | --- |
| `ORDER BY price` | smallest first (ASC, "ascending", is the same thing written out) |
| `ORDER BY price DESC` | biggest first ("descending") |
| `ORDER BY name` | text in alphabetical order, by bytes (`B` before `a`, as in SQLite) |
| `ORDER BY stock` with some NULLs | the NULLs first (last with DESC) |
| `... ORDER BY price LIMIT 2` | the 2 smallest: sorted first, then cut to 2 |
| `SELECT name ... ORDER BY stock` | sorted by a column that isn't given back: that's allowed |

Rows with the same value stay in the order they were added, with DESC too.

Only one column (no `ORDER BY a, b`). That's enough for nearly everything, and a second
column is easy to add once the first works.


## The whole change in one picture

```
 "SELECT name FROM items ORDER BY price DESC LIMIT 2"
     |
     |  sql.c, parse_select: reads ORDER BY price DESC into the statement_t      (steps 1, 2)
     v
 statement_t  { ..., order_by "price", order_desc true, limit 2 }
     |
     |  run.c, run_select: finds which column "price" is                        (step 3)
     |         select_row: KEEPS a copy of each row the WHERE allows             (step 4)
     |                     (instead of giving it back straight away)
     |         then sorts the kept rows by price                                 (step 5)
     |         then gives back the first LIMIT of them, and frees the copies      (step 6)
     v
 [cake] [tea]
```

Everything is in two files you've seen: `src/db/sql.c` (and `sql.h`), and `src/db/run.c`.


## Running the tests

From `src/`:

```bash
make test T=database
```

It runs `tools/test/tests/test_database.py`, which runs `tools/test/files/aerdb.aer` in AerOS.
Every check prints PASS or FAIL with what it got, so after each step you can see what works.
Right now `aerdb.aer` stops at its first ORDER BY with `AerDB can't do ORDER BY yet` (a mistake
in the SQL stops an AerLang program), so the test fails there. When they all pass,
run `make check` too: it checks every function has a comment above it, as the rest of AerOS does.

You can also try things by hand in AerOS, with the `sql` command:

```
sql shop.db SELECT name, price FROM items ORDER BY price DESC
show sql shop.db SELECT name FROM items ORDER BY price
```


## Step 1: somewhere to keep it (sql.h)

`statement_t` is what the parser fills in and run.c reads. It needs two more fields: the column
to sort by, and which way.

```c
    char order_by[AERDB_NAME_MAX];   // SELECT's ORDER BY column, "" for none
    bool order_desc;                 // ORDER BY ... DESC: biggest first
```

Put them next to `limit`. You don't need to set them to "nothing" anywhere: `sql_parse` starts
with `memset(out, 0, ...)`, which makes `order_by` an empty string and `order_desc` false.

Also add `[ORDER BY column [ASC | DESC]]` to the SELECT line in the comment at the top of sql.h,
before `[LIMIT n]`.


## Step 2: reading it (sql.c, parse_select)

In `parse_select` there's this, after the WHERE and before LIMIT:

```c
    if (at_keyword(p, "ORDER")) {
        return aerdb_fail(p->db, AERDB_ERR_UNSUPPORTED, "AerDB can't do ORDER BY yet");
    }
```

Replace it with code that reads `ORDER BY column`, then maybe `ASC` or `DESC`. The helpers at the
top of sql.c do all the work:

| You want | Use |
| --- | --- |
| "if the next word is ORDER, go past it" | `take_keyword(p, "ORDER")`: true if it was there |
| "the next word must be BY" | `need_keyword(p, "BY")`: AERDB_OK, or an error saying what was there |
| "the next word is a column name: copy it" | `read_name(p, s->order_by)` |
| "maybe DESC" | `take_keyword(p, "DESC")` |

The shape (the same as how `parse_create` reads `IF NOT EXISTS`):

```c
    if (take_keyword(p, "ORDER")) {
        result = need_keyword(p, "BY");
        if (result == AERDB_OK) {
            result = ...read the column name into s->order_by...
        }
        if (result != AERDB_OK) {
            return result;
        }
        // DESC: biggest first. ASC (the usual way round) can be written too, and changes nothing
        ...
    }
```

Things to get right:
- **ORDER BY goes before LIMIT,** as in SQL, so put it where the old error was.
- **Keywords ignore case** already (`take_keyword` uses `strcasecmp`), so `order by price desc` works.
- **Don't read ASC and DESC both.** `if (take_keyword(p, "DESC")) { ... } else { take_keyword(p, "ASC"); }`.
- `result` is already declared further up in `parse_select` (`int result = need_keyword(p, "FROM");`),
  so use it, rather than writing `int result` again.

**Check:** try it by hand with the `sql` command: `ORDER name` now says
`expected BY, but found name`, `ORDER BY` on its own says `expected a name, but found the end`,
and `ORDER BY legs LIMIT 1 DESC` says `didn't expect DESC there`. Nothing is sorted yet, so
`make test T=database` still fails, but now `aerdb.aer` runs to the end, with FAIL lines for the
rows in the wrong order instead of stopping at the first ORDER BY.


## Step 3: which column is it? (run.c, run_select)

run.c works with column **numbers** (0 for a table's first column), not names, so it doesn't
compare names for every row. `column_number(run, name)` turns a name into its number, or -1
when the table has no such column.

1. Add to `run_t` (the struct at the top of run.c):

   ```c
       int order_column;   // SELECT's ORDER BY column, as a number (-1 for no ORDER BY)
   ```

2. In `run_select`, before `table_scan`, set it:
   - no ORDER BY (`s->order_by[0] == '\0'`, an empty string): `run->order_column = -1`
   - otherwise: `column_number(run, s->order_by)`, and if that's -1, fail the way
     `columns_to_numbers` does:

   ```c
   return aerdb_fail(run->db, AERDB_ERR_NOT_FOUND, "%s has no column called %s", s->table, s->order_by);
   ```

**Check:** `ORDER BY colour` now says `pets has no column called colour`. That's the last of
the error checks in `test_sql_command`; its other ORDER BY check (bird before cat) needs step 6.


## Step 4: keeping the rows (run.c, select_row)

This is the important idea of the whole change.

Right now `select_row` is called by `table_scan` once for each row, and gives that row back
straight away (`run->row_out(...)`). With ORDER BY it can't: the first row given back has to be
the smallest, and that might be the last row in the table. So with ORDER BY, `select_row` only
**keeps** each row the WHERE allows, and the giving back happens after the scan (step 6).

**Why a copy?** The `values` `select_row` is given point into the page table_scan is reading
(for text, `values[i].bytes` is an address inside that page). As soon as `select_row` returns,
table_scan moves on and that memory holds a different page. Keeping the `values` themselves would
keep pointers to memory that changes underneath you: the classic C mistake. So each row is turned
back into bytes (`row_encode`), and those bytes are copied into memory of its own (`malloc`).

`update_row` and `compact_keep` in run.c already do exactly this. Read `compact_keep` first:
it's short, and step 4 is nearly the same code.

1. A struct for one kept row, above `run_t`:

   ```c
   // SELECT ... ORDER BY: a row kept until every row has been read, so they can be sorted
   typedef struct {
       uint8_t* bytes;     // the row as bytes (row_encode), in memory of its own
       uint16_t size;      // how many bytes
       db_value_t key;     // its ORDER BY column's value (text in it points into bytes)
   } sort_row_t;
   ```

2. In `run_t`, the list of them (it grows one row at a time, with `realloc`):

   ```c
       sort_row_t* kept;     // SELECT ... ORDER BY: the rows, until they are sorted
       int kept_count;
       bool out_of_memory;     // a row couldn't be kept
   ```

   `run_statement` does `memset(run, 0, ...)`, so `kept` starts as NULL and `kept_count` as 0.
   `realloc(NULL, size)` is the same as `malloc(size)`, which is why the first row needs nothing
   special.

3. At the top of `select_row`, after the WHERE check, when there's an ORDER BY
   (`run->order_column >= 0`):

   ```c
       // ORDER BY: keep a copy of the row, to sort once they have all been read (step 5)
       if (run->order_column >= 0) {
           uint8_t bytes[AERDB_PAGE_SIZE];
           uint16_t size = row_encode(values, count, bytes, sizeof(bytes));
           ...make the list one longer (realloc), and copy the bytes (malloc + memcpy),
              the way compact_keep does. If either gives back NULL: free what you got,
              set run->out_of_memory, and return true (stop reading)...
           ...set the new row's bytes and size, and its key (below)...
           run->kept_count++;
           return false;   // carry on reading: nothing is given back yet
       }
   ```

   **The key.** The sort compares each row's ORDER BY value, so keep it with the row. It must
   come from **the copy**, not from `values` (which is about to change). Turn the copy back into
   values with `row_decode`, and keep the one in the ORDER BY column:

   ```c
           db_value_t copied[AERDB_COLUMNS_MAX];
           int n = row_decode(copy, size, copied, AERDB_COLUMNS_MAX);
           // a row with fewer values than the table has columns: the missing ones are NULL
           kept->key = run->order_column < n ? copied[run->order_column] : value_null();
   ```

   (`copy` is your malloc'd bytes, `kept` the new entry in the list. A `db_value_t` copied this
   way is fine to keep: its number is in it, and its text points into `copy`, which stays until
   step 6 frees it.)

   **LIMIT** is left alone here: with ORDER BY every row has to be read and sorted before it's
   known which ones are first, so don't count `given` or stop early.


## Step 5: sorting them (run.c)

Write a function that sorts `run->kept` by `key`:

```c
// SELECT ... ORDER BY: sort the kept rows by their ORDER BY value, smallest first (biggest first
// with DESC). Rows with the same value stay in the order they were read
static void sort_kept_rows (run_t* run) {
    ...
}
```

`value_compare(&a, &b)` (value.c) already says which of two values comes first: -1 if `a` does,
0 if they're the same, 1 if `b` does. It knows the rules: numbers by size (2 and 2.0 are the
same), text by bytes, NULL before everything. So the sort only has to use it.

**Insertion sort** is the simplest sort, and plenty for the size of tables AerDB has: take each
row in turn, and slide it back past every row before it that should come after it. It's what
Snake's `best_scores()` does in AerLang. In C:

```c
    for (int i = 1; i < run->kept_count; i++) {
        sort_row_t row = run->kept[i];     // the row being moved (a copy of the struct)
        int j = i;
        // slide back past each row before it that should come after it
        while (j > 0 && ...kept[j - 1] should come after row...) {
            run->kept[j] = run->kept[j - 1];   // move that one along one place
            j--;
        }
        run->kept[j] = row;
    }
```

The `...` is the one line to work out: with ASC, `kept[j - 1]` comes after `row` when
`value_compare(&run->kept[j - 1].key, &row.key) > 0`. With DESC, the other way round
(`< 0`).

Two traps:
- **Use `> 0`, not `>= 0`.** With `>=`, equal rows swap places, and the "equal values keep their
  order" checks fail.
- **Don't do DESC by sorting ascending and reversing the list.** It gives the right biggest-first
  order, but reverses equal rows too (the `DESC` tie check catches it). Flip the comparison instead.


## Step 6: giving them back, and freeing them (run.c)

After the scan in `run_select`, when there's an ORDER BY:

1. If `run->out_of_memory`: free everything (below) and fail with
   `aerdb_fail(run->db, AERDB_ERR_NO_MEMORY, "not enough memory to sort %s", table->name)`.
2. `sort_kept_rows(run)`.
3. Give the rows back in order, the way the end of `select_row` does: `row_decode` the row's
   bytes into values, put the chosen columns in `out[]`, call `run->row_out`, count `given`,
   and stop at the LIMIT (or when `row_out` says stop).
4. Free each row's `bytes`, then the list itself, `free(run->kept)`. **Every** row, including
   the ones after the LIMIT that were never given back: memory you malloc'd and don't free is
   lost until the program ends (a "leak").

**A tidy way to do step 3.** The end of `select_row` (from `db_value_t out[...]` to the
`return`) is already "give one row back". Move it into its own function, and both places call it:

```c
// give one row back to whoever asked for the SELECT: the chosen columns, in order. Gives back
// true (stop) when they've had enough, or LIMIT rows have been given
static bool give_row (run_t* run, const db_value_t* values, int count) {
    ...the lines moved out of select_row...
}
```

Then `select_row` ends with `return give_row(run, values, count);`, and step 3 is a loop that
`row_decode`s each kept row and calls `give_row`, stopping when it gives back true.

**Check:** `make test T=database`. Everything passes.

**Optional:** a SHOW note, so `show sql ...` explains the sort. Next to the TRACE in `run_select`:

```c
TRACE("kept %d row(s), sorted them by %s, and gave back %d", run->kept_count, s->order_by, run->given);
```


## Step 7: telling people

- `src/disk/docs/database.md`: add `ORDER BY column [DESC]` to "The SQL AerDB understands", and
  take ORDER BY out of the "Not there yet" line.
- `docs/plans/DATABASE.md`: take ORDER BY out of the "Later" list.
- The comment at the top of run.c: its SELECT line can say that with ORDER BY the rows are kept,
  sorted, then given back.


## Step 8: using it (Snake)

`best_scores()` in `src/disk/bin/games/snake.aer` sorts the rows itself and cuts them to five.
With ORDER BY, that whole function becomes a query:

```
func best_scores()
    # Snake's best scores, best first, as a list of [name, score] (at most SCORES_SHOWN)
    if scores_db == nil then
        return []
    end
    return dbquery(scores_db, "SELECT name, score FROM scores WHERE game = 'snake' ORDER BY score DESC LIMIT ?", [SCORES_SHOWN])
end
```

The comment above the "High scores" section ("AerDB gives rows back in the order they were
added, so the best are found by sorting them here") changes too. Then `make test T=snake` plays
it, enters a high score, and checks the list.


## If you get stuck

- **A check fails:** the FAIL line says what came back and what was wanted. `str()` of a list
  shows every row, so you can see whether it's the order, a missing row, or a wrong value.
- **AerOS crashes or prints nonsense** in a SELECT with ORDER BY: almost always a value pointing
  at memory that's gone. Check the key comes from `copy` (step 4), and that nothing reads a row's
  `bytes` after it's freed.
- **It won't compile:** the error names the file and line. "implicit declaration" means a
  function is used above where it's written: move it up, or check its name.
- Or ask me: show me the function, and I'll say what's wrong and why, rather than rewrite it.
