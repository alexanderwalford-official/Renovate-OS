"""The terminal itself: usage messages, Tab completion, scrolling back, and the editor."""

import time

from aeros import GUEST_PROMPT


def test_usage_messages(t):
    # a command typed without what it needs says how to use it
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    for command in ['rd', 'mkf', 'del', 'sd']:
        aeros.run(command)
    aeros.expect('Usage: RD', 'Usage: MKF', 'Usage: DEL', 'Usage: SD')


def test_tab_completion(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    # Tab fills in the rest of a file's name, so Enter reads /bin/hello.aer
    aeros.type('rd /bin/hel')
    aeros.key('tab')
    aeros.wait_for('rd /bin/hello.aer')
    aeros.run('')
    # hello.aer is longer than the screen, so look in everything that was printed
    aeros.expect_printed('rd /bin/hello.aer', 'run it from the terminal by typing: hello')


def test_scrollback(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.run('for i = 1 to 60 do print("line " + str(i)) end')
    aeros.expect_line('line 60')

    # Page Up looks back a screen at a time, and says how far back it is
    aeros.key('pgup')
    aeros.wait_for('24 lines back')
    aeros.expect('PgUp / PgDn')
    aeros.expect_not('line 60')
    aeros.key('pgdn')
    aeros.wait_gone('lines back')

    # typing while looking back goes back to the live screen first
    aeros.key('pgup')
    aeros.wait_for('24 lines back')
    aeros.run('print("typed while scrolled")')
    aeros.expect_not('lines back')
    aeros.expect_line('typed while scrolled')


def test_editor(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.cls()
    aeros.ask('edt t.aer')
    aeros.wait_for('^S Save')
    aeros.type('let x = 5\nprint("x is", x)')
    aeros.key('ctrl-s')
    aeros.wait_for('Saved 26 bytes')
    aeros.expect('^R Run')

    # Ctrl+R runs it from the editor
    aeros.key('ctrl-r')
    aeros.wait_for('x is 5', 20)
    aeros.key('ret')
    time.sleep(0.5)

    # Ctrl+Q leaves, and the file is saved as typed
    before = aeros.prompts()
    aeros.key('ctrl-q')
    aeros.wait_prompt(before)
    aeros.cls()
    aeros.run('rd t.aer')
    aeros.expect_line('let x = 5', 'print("x is", x)')
