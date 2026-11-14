"""AerDB, AerOS's database (src/db/): from AerLang (files/aerdb.aer), and the SQL command."""

from aeros import GUEST_PROMPT


def test_from_aerlang(t):
    # files/aerdb.aer checks every statement from AerLang, on a real disk
    disk = t.disk(files={'/bin/aerdb.aer': t.files + '/aerdb.aer'})
    aeros = t.boot(disk=disk)
    aeros.first_login()
    aeros.cls()
    aeros.run_checks('aerdb')
    aeros.quit()
    t.check_disk(disk)

    # booted again, the database is just as the program left it
    aeros = t.boot(disk=disk)
    aeros.login()
    aeros.cls()
    aeros.run('sql shop.db SELECT name, price FROM items')
    aeros.expect_line('name | price', 'cake | 4.0', 'tea | 3.0', '(2 rows)')


def test_sql_command(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('sql pets.db CREATE TABLE pets (name TEXT, legs INTEGER)')
    aeros.run("sql pets.db INSERT INTO pets VALUES ('cat', 4)")
    aeros.run("sql pets.db INSERT INTO pets VALUES ('bird', 2)")
    aeros.expect_line('Done.', '1 row(s) changed')
    aeros.cls()
    aeros.run('sql pets.db SELECT * FROM pets WHERE legs > 2')
    aeros.expect_line('name | legs', 'cat | 4', '(1 row)')

    # mistakes say what was wrong
    aeros.cls()
    aeros.run('sql pets.db SELECT * FROM dogs')
    aeros.run('sql pets.db SELECT * FROM pets WHERE legs = 2 OR legs = 4')
    aeros.run("sql pets.db INSERT INTO pets VALUES ('fish')")
    aeros.expect('Error: no such table: dogs', "Error: AerDB can't do OR in a WHERE yet: only AND",
                 'Error: pets has 2 column(s), but 1 value(s) were given')

    # ORDER BY (docs/plans/ORDER_BY.md), and what it says about mistakes
    aeros.cls()
    aeros.run('sql pets.db SELECT name FROM pets ORDER BY legs')
    aeros.expect_order('bird', 'cat', '(2 rows)')
    aeros.cls()
    aeros.run('sql pets.db SELECT * FROM pets ORDER BY colour')
    aeros.run('sql pets.db SELECT * FROM pets ORDER name')
    aeros.run('sql pets.db SELECT * FROM pets ORDER BY')
    aeros.run('sql pets.db SELECT * FROM pets ORDER BY legs LIMIT 1 DESC')
    aeros.expect('Error: pets has no column called colour', 'Error: expected BY, but found name',
                 'Error: expected a name, but found the end', "Error: didn't expect DESC there")

    # the prompt: statement after statement, until an empty line
    aeros.cls()
    before = aeros.prompts()
    aeros.ask('sql pets.db')
    aeros.wait_for('sql> ')
    aeros.type("DELETE FROM pets WHERE name = 'cat'\n")
    aeros.wait_for('1 row(s) changed')
    aeros.type('SELECT name FROM pets\n')
    aeros.wait_for('(1 row)')
    aeros.type('\n')
    aeros.wait_prompt(before)
    aeros.expect_line('bird')


def test_show_a_query(t):
    # SHOW explains what a statement did inside AerDB
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('sql s.db CREATE TABLE t (a INTEGER)')
    aeros.cls()
    aeros.run('show sql s.db INSERT INTO t VALUES (1)')
    aeros.expect('What AerOS did:', "t's first row: started page 2")
