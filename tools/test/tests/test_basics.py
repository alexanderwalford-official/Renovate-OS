"""Booting, the terminal's commands, and files in the temporary file system (no disk)."""

from aeros import GUEST_PROMPT


def test_boot(t):
    # with no disk, AerOS boots straight to the guest prompt and says why
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.expect('You are in VFS mode')
    aeros.run('hlp')
    aeros.expect('DEL', 'WHY', 'RUN')


def test_files(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('mkf notes.txt hello there')
    aeros.run('mkd stuff')
    aeros.cls()
    aeros.run('rd notes.txt')
    aeros.expect_line('hello there')
    aeros.run('la')
    aeros.expect('notes.txt', 'stuff/')

    # deleting it: reading it afterwards says it isn't there
    aeros.cls()
    aeros.run('del notes.txt')
    aeros.run('rd notes.txt')
    aeros.expect('File not found: notes.txt')


def test_unknown_command(t):
    aeros = t.boot()
    aeros.wait_for(GUEST_PROMPT, 60)
    aeros.run('dle notes.txt')
    aeros.run('frob')
    aeros.expect('Did you mean DEL?', 'Unrecognised command: frob')
