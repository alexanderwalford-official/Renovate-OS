# Databases

AerOS has a database of its own, AerDB. A database is one file, holding tables of rows, and
you work with it in SQL, the language most databases speak.

## From the terminal

`sql` runs SQL on a database file. The file is made the first time:

```
sql shop.db CREATE TABLE items (name TEXT, price REAL)
sql shop.db INSERT INTO items VALUES ('tea', 2.5)
sql shop.db INSERT INTO items VALUES ('cake', 4.0)
sql shop.db SELECT * FROM items WHERE price < 3
```

shows

```
name | price
tea | 2.5
(1 row)
```

`sql shop.db` on its own opens a `sql>` prompt for statement after statement. An empty line
(or `exit`) leaves it.

## From a program

```
let db = dbopen("shop.db")
dbrun(db, "CREATE TABLE items (name TEXT, price REAL)")
dbrun(db, "INSERT INTO items VALUES (?, ?)", ["tea", 2.5])
for row in dbquery(db, "SELECT name, price FROM items") do
    print(row[0], row[1])
end
dbclose(db)
```

| Function | Does |
| --- | --- |
| `dbopen(path)` | opens a database (making it if need be): a number for it, nil if it can't |
| `dbrun(db, sql, values)` | runs a statement, giving back how many rows it changed |
| `dbquery(db, sql, values)` | runs a SELECT: a list of rows, each a list of its values |
| `dbclose(db)` | closes it (anything left open closes when the program ends) |

Each `?` in the SQL takes the next of the values. Use `?` for anything that comes from
outside the program, rather than adding it to the SQL text. A mistake in the SQL stops the
program and says what was wrong, and `why` shows the line. Like every file, a program
[asks you first](perms.md) before it opens a database in your folders.

## The SQL AerDB understands

```
CREATE TABLE name (column type, ...)        DROP TABLE name
INSERT INTO name VALUES (value, ...)        INSERT INTO name (column, ...) VALUES (...)
SELECT * FROM name                          SELECT column, ... FROM name
       ... WHERE column = value AND ...            ... LIMIT 10
UPDATE name SET column = value, ... WHERE ...
DELETE FROM name WHERE ...
COMPACT
```

A WHERE compares columns with `=`, `!=`, `<`, `<=`, `>`, `>=`, joined by AND, and `IS NULL`
or `IS NOT NULL` finds empty ones. Values are numbers (`42`, `2.5`), text in single quotes
(`'tea'`, with `''` for a quote inside it), `NULL`, or `?`.

A value is NULL, a whole number, a decimal, text or bytes, whatever the column was called.
A row can hold up to 16 values, and has to fit in 4 KB.

DELETE only marks rows as deleted. `COMPACT` gives their space back, and makes the file smaller.

Not there yet: OR, ORDER BY, joins and `count(*)`. AerDB says so when it meets them.

## Watching it work

`show sql shop.db INSERT INTO items VALUES ('jam', 3.0)` tells you what AerDB did inside: which
page the row went on, or that a page was full and a new one started.

See also: [AerLang](aerlang.md), [Files and folders](files.md), [Ask AerOS why](why.md).
