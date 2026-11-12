"""AerLang: the language itself (files/langtest.aer), and AerLang typed at the prompt."""

import time

from aeros import GUEST_PROMPT


def test_language(t):
    # langtest.aer checks every kind of statement and expression, printing PASS or FAIL for
    # each and ALL PASSED at the end. It goes on a disk, as a program run from /bin/
    disk = t.disk(files={'/bin/langtest.aer': t.files + '/langtest.aer'})
    aeros = t.boot(disk=disk)
    aeros.first_login()
    aeros.cls()
    aeros.run('langtest')
    deadline = time.time() + 30
    while 'ALL PASSED' not in aeros.serial() and 'FAILED' not in aeros.serial():
        if time.time() > deadline:
            aeros.fail('langtest.aer never finished')
        time.sleep(0.5)
    failures = [line for line in aeros.serial().split('\n') if line.startswith('FAIL')]
    if failures or 'ALL PASSED' not in aeros.serial():
        aeros.fail('langtest.aer failed:\n' + '\n'.join(failures))


def test_values_at_the_prompt(t):
    # anything that isn't a command runs as AerLang, and an expression shows its value
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('let x = 5')
    aeros.run('x * 2')
    aeros.run('"hi" + " there"')
    aeros.run('7 / 2.0')
    aeros.expect_line('10', 'hi there', '3.5')

    # a block can carry on over several lines
    aeros.cls()
    aeros.run('for i = 1 to 3 do')
    aeros.run('print(i * 10)')
    aeros.run('end')
    aeros.expect_line('10', '20', '30')


def test_functions_and_cmd(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('func twice(n)')
    aeros.run('return n * 2')
    aeros.run('end')
    aeros.cls()
    aeros.run('twice(21)')
    aeros.expect_line('42')

    # cmd() gives back what a command printed, as a list of lines
    aeros.run('mkf notes.txt hello')
    aeros.cls()
    aeros.run('for f in la() do if find(f, ".txt") >= 0 then print("text:", f) end end')
    aeros.expect_line('text: notes.txt')


def test_errors(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('prnt(1)')
    aeros.run('let total = 1')
    aeros.run('totl = 2')
    aeros.run('1 / 0')
    aeros.expect("unknown name 'prnt' (did you mean print?)",
                 "unknown variable 'totl' (did you mean total?)",
                 'division by zero')
